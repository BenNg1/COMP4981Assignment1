#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Find the end of the first request line (\r\n)
static int find_request_line_end(const char *raw, size_t raw_len) {
    for (size_t i = 1; i < raw_len; i++) {
        if (raw[i - 1] == '\r' && raw[i] == '\n') {
            return (int)(i - 1);
        }
    }
    return -1;
}

// Parse HTTP request line and fill method/target
int parse_http_request(const char *raw, size_t raw_len, http_request_t *out) {
    // Basic input validation
    if (!raw || !out || raw_len == 0) {
        return 400;
    }

    // Must contain a complete first line
    int line_end = find_request_line_end(raw, raw_len);
    if (line_end <= 0 || line_end >= 2048) {
        return 400;
    }

    // Copy request line into a local buffer
    char line[2048];
    memcpy(line, raw, (size_t)line_end);
    line[line_end] = '\0';

    char method[16] = {0};
    char target[PATH_MAX] = {0};
    char version[16] = {0};

    // Expect: METHOD TARGET VERSION
    int parts = sscanf(line, "%15s %4095s %15s", method, target, version);
    if (parts != 3) {
        return 400;
    }

    // Only HTTP/1.0 and HTTP/1.1 are accepted
    if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) {
        return 400;
    }

    // Target must start with '/'
    if (target[0] != '/') {
        return 400;
    }

    // Reject control chars in target
    for (size_t i = 0; target[i] != '\0'; i++) {
        unsigned char c = (unsigned char)target[i];
        if (iscntrl(c)) {
            return 400;
        }
    }

    // Map supported methods
    if (strcasecmp(method, "GET") == 0) {
        out->method = HTTP_METHOD_GET;
    } else if (strcasecmp(method, "HEAD") == 0) {
        out->method = HTTP_METHOD_HEAD;
    } else {
        // Unsupported method
        out->method = HTTP_METHOD_UNSUPPORTED;
        strncpy(out->target, target, sizeof(out->target) - 1);
        out->target[sizeof(out->target) - 1] = '\0';
        return 405;
    }

    // Save parsed target
    strncpy(out->target, target, sizeof(out->target) - 1);
    out->target[sizeof(out->target) - 1] = '\0';

    return 0;
}

// Return reason phrase for HTTP status code
const char *http_reason_phrase(int status_code) {
    switch (status_code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 500:
        default:
            return "Internal Server Error";
    }
}

// Guess MIME type from file extension
const char *guess_mime_type(const char *path) {
    if (!path) {
        return "application/octet-stream";
    }

    const char *dot = strrchr(path, '.');
    if (!dot) {
        return "application/octet-stream";
    }

    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(dot, ".txt") == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcasecmp(dot, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcasecmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    if (strcasecmp(dot, ".svg") == 0) return "image/svg+xml";

    return "application/octet-stream";
}

// Format current UTC time in HTTP date format
void format_http_date(char *dst, size_t dst_sz) {
    time_t now = time(NULL);
    struct tm gm;

#if defined(_POSIX_THREAD_SAFE_FUNCTIONS) && !defined(__APPLE__)
    gmtime_r(&now, &gm);
#else
    struct tm *tmp = gmtime(&now);
    if (tmp) {
        gm = *tmp;
    } else {
        memset(&gm, 0, sizeof(gm));
    }
#endif

    strftime(dst, dst_sz, "%a, %d %b %Y %H:%M:%S GMT", &gm);
}

// Build HTTP response headers into dst
int build_response_headers(char *dst,
                           size_t cap,
                           int status_code,
                           const char *content_type,
                           off_t content_length,
                           int include_allow_header) {
    if (!dst || !content_type) {
        return -1;
    }

    char date[128];
    format_http_date(date, sizeof(date));

    int n = snprintf(
        dst,
        cap,
        "HTTP/1.1 %d %s\r\n"
        "Date: %s\r\n"
        "Server: comp4981-httpd/1.0\r\n"
        "Connection: close\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "%s"
        "\r\n",
        status_code,
        http_reason_phrase(status_code),
        date,
        content_type,
        (long long)content_length,
        include_allow_header ? "Allow: GET, HEAD\r\n" : "");

    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }

    return n;
}
