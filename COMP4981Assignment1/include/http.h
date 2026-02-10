#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_UNSUPPORTED
} http_method_t;

typedef struct {
    http_method_t method;
    char target[PATH_MAX];
} http_request_t;

// Returns 0 on success.
// Returns 400 for bad request syntax/version.
// Returns 405 for unsupported method.
int parse_http_request(const char *raw, size_t raw_len, http_request_t *out);

const char *http_reason_phrase(int status_code);
const char *guess_mime_type(const char *path);
void format_http_date(char *dst, size_t dst_sz);

// Returns number of bytes written, or -1 on truncation/error.
int build_response_headers(char *dst,
                           size_t cap,
                           int status_code,
                           const char *content_type,
                           off_t content_length,
                           int include_allow_header);

#endif
