#include "server.h"

#include "http.h"
#include "path.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Maximum concurrent clients handled by poll.
#define MAX_CLIENTS 1024
// Hard cap for request header bytes.
#define MAX_HEADER_BYTES 16384
// Buffer size for generated response headers.
#define MAX_RESP_HEADER 2048
// Buffer size for generated error response body.
#define MAX_ERROR_BODY 512
// File streaming chunk size.
#define FILE_CHUNK 8192

// Client mode in the event loop.
typedef enum { MODE_READING = 0, MODE_WRITING = 1 } io_mode_t;

// Per-client state.
typedef struct {
    int active;              // Slot in use
    int fd;                  // Client socket
    io_mode_t mode;          // Read request / write response

    // Request buffer
    char req_buf[MAX_HEADER_BYTES + 1];
    size_t req_len;

    // Response header buffer
    char hdr_buf[MAX_RESP_HEADER];
    size_t hdr_len;
    size_t hdr_sent;

    // In-memory body (used for generated error pages)
    char mem_body[MAX_ERROR_BODY];
    size_t mem_len;
    size_t mem_sent;

    int is_head;             // HEAD => headers only

    // File streaming state (GET success path)
    int file_fd;             // -1 if not streaming a file
    off_t file_size;
    off_t file_sent;

    unsigned char chunk[FILE_CHUNK];
    ssize_t chunk_len;
    ssize_t chunk_sent;
} client_t;

// Global stop flag for graceful shutdown.
static volatile sig_atomic_t g_stop = 0;
// Bind IP from CLI args.
static char g_bind_ip[64] = "0.0.0.0";

// Signal handler sets stop flag.
static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

// Validate numeric TCP port.
static int parse_port_number(const char *port_str) {
    if (!port_str || *port_str == '\0') return -1;

    char *end = NULL;
    long v = strtol(port_str, &end, 10);

    if (!end || *end != '\0') return -1;
    if (v < 1 || v > 65535) return -1;

    return (int)v;
}

// Validate literal IPv4/IPv6 string.
static int is_valid_ip_literal(const char *ip) {
    struct in_addr a4;
    struct in6_addr a6;

    if (!ip || *ip == '\0') return 0;
    if (inet_pton(AF_INET, ip, &a4) == 1) return 1;
    if (inet_pton(AF_INET6, ip, &a6) == 1) return 1;
    return 0;
}

// Parse CLI args: <ip> <port> <doc_root>.
int parse_arguments(int argc, char **argv, server_config_t *cfg) {
    if (!cfg) return -1;

    // Default config values.
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_header_size = 8192;
    cfg->backlog = 128;

    // Expect exactly 3 args after program name.
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <ip> <port> <doc_root>\n", argv[0]);
        return -1;
    }

    // Validate and store bind IP.
    if (!is_valid_ip_literal(argv[1])) {
        fprintf(stderr, "Invalid IP: %s\n", argv[1]);
        return -1;
    }
    snprintf(g_bind_ip, sizeof(g_bind_ip), "%s", argv[1]);

    // Validate and store port.
    if (parse_port_number(argv[2]) < 0) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return -1;
    }
    snprintf(cfg->port, sizeof(cfg->port), "%s", argv[2]);

    // Canonicalize and validate document root.
    char canonical[PATH_MAX];
    if (!realpath(argv[3], canonical)) {
        perror("realpath(doc_root)");
        return -1;
    }

    struct stat st;
    if (stat(canonical, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Document root is not a directory: %s\n", canonical);
        return -1;
    }
    snprintf(cfg->doc_root, sizeof(cfg->doc_root), "%s", canonical);

    // Clamp max header size to safe limits.
    if (cfg->max_header_size <= 0 || cfg->max_header_size > MAX_HEADER_BYTES) {
        cfg->max_header_size = 8192;
    }

    return 0;
}

// Create, bind, listen, and set non-blocking listening socket.
static int init_server_socket(const char *bind_ip, const char *port, int backlog) {
    struct addrinfo hints, *res = NULL, *p = NULL;
    int listen_fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;       // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;   // TCP
    hints.ai_flags = AI_NUMERICHOST;   // bind_ip is numeric literal

    int gai = getaddrinfo(bind_ip, port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo(%s:%s): %s\n", bind_ip, port, gai_strerror(gai));
        return -1;
    }

    // Try each candidate until one succeeds.
    for (p = res; p; p = p->ai_next) {
        int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;

        // Allow quick restart after close.
        int yes = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0 &&
            listen(fd, backlog) == 0 &&
            set_nonblocking(fd) == 0) {
            listen_fd = fd;
            break;
        }

        close(fd);
    }

    freeaddrinfo(res);
    return listen_fd;
}

// Return 1 if request buffer contains "\r\n\r\n".
static int has_header_end(const char *buf, size_t len) {
    if (len < 4) return 0;

    for (size_t i = 3; i < len; i++) {
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n' &&
            buf[i - 1] == '\r' && buf[i] == '\n') {
            return 1;
        }
    }
    return 0;
}

// Map filesystem errno to HTTP status.
static int status_from_errno(void) {
    if (errno == ENOENT || errno == ENOTDIR) return 404;
    if (errno == EACCES) return 403;
    return 500;
}

// Reset one client slot.
static void reset_client(client_t *c) {
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->file_fd = -1;
}

// Close client and clear its poll slot.
static void close_client_slot(struct pollfd *pfd, client_t *c) {
    if (c->fd >= 0) close(c->fd);
    if (c->file_fd >= 0) close(c->file_fd);

    pfd->fd = -1;
    pfd->events = 0;
    pfd->revents = 0;

    reset_client(c);
}

// Build generated HTML error response.
static int make_error_response(client_t *c, int status, int is_head, int include_allow) {
    // Error response is memory-backed.
    c->is_head = is_head;
    c->file_fd = -1;
    c->file_size = 0;
    c->file_sent = 0;
    c->chunk_len = 0;
    c->chunk_sent = 0;

    int n = snprintf(c->mem_body, sizeof(c->mem_body),
                     "<html><body><h1>%d %s</h1></body></html>\n",
                     status, http_reason_phrase(status));
    if (n < 0) return -1;

    c->mem_len = (size_t)n;
    c->mem_sent = 0;

    int h = build_response_headers(
        c->hdr_buf,
        sizeof(c->hdr_buf),
        status,
        "text/html; charset=utf-8",
        (off_t)c->mem_len,
        include_allow
    );
    if (h < 0) return -1;

    c->hdr_len = (size_t)h;
    c->hdr_sent = 0;
    c->mode = MODE_WRITING;

    return 0;
}

// Parse request and prepare success/error response state.
static int prepare_response(client_t *c, const server_config_t *cfg) {
    http_request_t req;
    int rc = parse_http_request(c->req_buf, c->req_len, &req);

    // Parsing/method errors.
    if (rc == 400) return make_error_response(c, 400, 0, 0);
    if (rc == 405) return make_error_response(c, 405, 0, 1);

    int is_head = (req.method == HTTP_METHOD_HEAD);

    // Resolve URL target under doc root safely.
    char fs_path[PATH_MAX];
    rc = resolve_path(cfg->doc_root, req.target, fs_path, sizeof(fs_path));
    if (rc != 0) {
        if (rc != 400 && rc != 403 && rc != 404) rc = 500;
        return make_error_response(c, rc, is_head, 0);
    }

    // Stat for existence/type/size.
    struct stat st;
    if (stat(fs_path, &st) != 0) {
        return make_error_response(c, status_from_errno(), is_head, 0);
    }

    // Build 200 response headers.
    int h = build_response_headers(
        c->hdr_buf,
        sizeof(c->hdr_buf),
        200,
        guess_mime_type(fs_path),
        st.st_size,
        0
    );
    if (h < 0) return make_error_response(c, 500, is_head, 0);

    c->hdr_len = (size_t)h;
    c->hdr_sent = 0;

    // No memory body for success path.
    c->mem_len = 0;
    c->mem_sent = 0;

    c->is_head = is_head;

    // Reset file stream state.
    c->file_fd = -1;
    c->file_size = st.st_size;
    c->file_sent = 0;
    c->chunk_len = 0;
    c->chunk_sent = 0;

    // GET streams file body; HEAD skips body.
    if (!is_head) {
        c->file_fd = open(fs_path, O_RDONLY);
        if (c->file_fd < 0) {
            return make_error_response(c, status_from_errno(), is_head, 0);
        }
    }

    c->mode = MODE_WRITING;
    return 0;
}

// Read request bytes until full headers are received.
static int read_client_request(client_t *c, const server_config_t *cfg) {
    for (;;) {
        ssize_t n = recv(
            c->fd,
            c->req_buf + c->req_len,
            (size_t)cfg->max_header_size - c->req_len,
            0
        );

        if (n > 0) {
            c->req_len += (size_t)n;
            c->req_buf[c->req_len] = '\0';

            // Reject oversized headers.
            if (c->req_len >= (size_t)cfg->max_header_size) {
                return make_error_response(c, 400, 0, 0);
            }

            // When headers complete, move to response prep.
            if (has_header_end(c->req_buf, c->req_len)) {
                return prepare_response(c, cfg);
            }

            // Keep draining readable bytes this loop.
            continue;
        }

        if (n == 0) return -1; // Peer closed.
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0; // Try later.
        return -1; // Hard recv error.
    }
}

// Send memory buffer with partial-send support.
static int send_buffer(int fd, const void *buf, size_t len, size_t *sent) {
    const char *p = (const char *)buf;

    while (*sent < len) {
        ssize_t n = send(fd, p + *sent, len - *sent, 0);

        if (n > 0) {
            *sent += (size_t)n;
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }

    return 1; // Done.
}

// Stream file body chunk-by-chunk.
static int flush_file(client_t *c) {
    if (c->file_fd < 0) return 1;

    for (;;) {
        // Load new chunk if needed.
        if (c->chunk_len == 0 || c->chunk_sent == c->chunk_len) {
            ssize_t r = read(c->file_fd, c->chunk, sizeof(c->chunk));
            if (r == 0) return 1; // EOF
            if (r < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            c->chunk_len = r;
            c->chunk_sent = 0;
        }

        // Send current chunk.
        while (c->chunk_sent < c->chunk_len) {
            ssize_t n = send(
                c->fd,
                c->chunk + c->chunk_sent,
                (size_t)(c->chunk_len - c->chunk_sent),
                0
            );

            if (n > 0) {
                c->chunk_sent += n;
                c->file_sent += n;
                continue;
            }

            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
            return -1;
        }
    }
}

// Write headers first, then optional body.
static int write_client_response(client_t *c) {
    int r = send_buffer(c->fd, c->hdr_buf, c->hdr_len, &c->hdr_sent);
    if (r <= 0) return r; // -1 error, 0 would block

    // HEAD is headers-only.
    if (c->is_head) return 1;

    // Error pages have memory body.
    if (c->mem_len > 0) {
        return send_buffer(c->fd, c->mem_body, c->mem_len, &c->mem_sent);
    }

    // Normal GET streams file.
    return flush_file(c);
}

// Add new client socket to first free slot.
static int add_client_to_slot(struct pollfd *pfds, client_t *clients, int client_fd) {
    for (int i = 1; i <= MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            reset_client(&clients[i]);

            clients[i].active = 1;
            clients[i].fd = client_fd;
            clients[i].mode = MODE_READING;
            clients[i].file_fd = -1;

            pfds[i].fd = client_fd;
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
            return 0;
        }
    }
    return -1; // No room.
}

// Accept all pending client connections.
static void accept_new_clients(int listen_fd, struct pollfd *pfds, client_t *clients) {
    for (;;) {
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);

        int cfd = accept(listen_fd, (struct sockaddr *)&addr, &len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept");
            break;
        }

        // Non-blocking client sockets are required for poll loop.
        if (set_nonblocking(cfd) != 0) {
            close(cfd);
            continue;
        }

        // Drop if client table is full.
        if (add_client_to_slot(pfds, clients, cfd) != 0) {
            close(cfd);
        }
    }
}

// Run poll-based server loop.
int run_server(const server_config_t *cfg) {
    if (!cfg) return 1;

    // Signal behavior:
    // - SIGINT/SIGTERM -> graceful stop
    // - SIGPIPE ignored so send() gives error instead of process kill
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    // Initialize listening socket.
    int listen_fd = init_server_socket(g_bind_ip, cfg->port, cfg->backlog);
    if (listen_fd < 0) {
        fprintf(stderr, "Failed to initialize server socket\n");
        return 1;
    }

    // Allocate poll and client arrays.
    struct pollfd *pfds = calloc((size_t)MAX_CLIENTS + 1, sizeof(*pfds));
    client_t *clients = calloc((size_t)MAX_CLIENTS + 1, sizeof(*clients));
    if (!pfds || !clients) {
        perror("calloc");
        free(pfds);
        free(clients);
        close(listen_fd);
        return 1;
    }

    // Initialize all slots to empty.
    for (int i = 0; i <= MAX_CLIENTS; i++) {
        pfds[i].fd = -1;
        pfds[i].events = 0;
        pfds[i].revents = 0;
        reset_client(&clients[i]);
    }

    // Poll slot 0 reserved for listening socket.
    pfds[0].fd = listen_fd;
    pfds[0].events = POLLIN;

    fprintf(stdout, "Server listening on %s:%s\n", g_bind_ip, cfg->port);
    fprintf(stdout, "Document root: %s\n", cfg->doc_root);

    // Main event loop.
    while (!g_stop) {
        int n = poll(pfds, MAX_CLIENTS + 1, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (n == 0) continue; // timeout

        // Accept new connections.
        if (pfds[0].revents & POLLIN) {
            accept_new_clients(listen_fd, pfds, clients);
        }

        // Handle client events.
        for (int i = 1; i <= MAX_CLIENTS; i++) {
            if (!clients[i].active) continue;

            short rev = pfds[i].revents;
            if (rev == 0) continue;

            // Close on poll/socket errors.
            if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
                close_client_slot(&pfds[i], &clients[i]);
                continue;
            }

            // Read phase.
            if (clients[i].mode == MODE_READING && (rev & POLLIN)) {
                if (read_client_request(&clients[i], cfg) < 0) {
                    close_client_slot(&pfds[i], &clients[i]);
                    continue;
                }

                // If response is prepared, switch to write events.
                if (clients[i].mode == MODE_WRITING) {
                    pfds[i].events = POLLOUT;
                }
            }

            // Write phase.
            if (clients[i].active &&
                clients[i].mode == MODE_WRITING &&
                (rev & POLLOUT)) {
                int wr = write_client_response(&clients[i]);

                // Done or failed -> close connection.
                if (wr < 0 || wr > 0) {
                    close_client_slot(&pfds[i], &clients[i]);
                }
            }
        }
    }

    // Cleanup active clients.
    for (int i = 1; i <= MAX_CLIENTS; i++) {
        if (clients[i].active) close_client_slot(&pfds[i], &clients[i]);
    }

    close(listen_fd);
    free(pfds);
    free(clients);

    fprintf(stdout, "Server stopped.\n");
    return 0;
}
