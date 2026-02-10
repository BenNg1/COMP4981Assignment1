#ifndef SERVER_H
#define SERVER_H

#include <limits.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char port[16];
    char doc_root[PATH_MAX];   // canonical (realpath)
    size_t max_header_size;
    int backlog;
} server_config_t;

int parse_arguments(int argc, char **argv, server_config_t *cfg);
int run_server(const server_config_t *cfg);

#endif
