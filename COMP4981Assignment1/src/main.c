#include "server.h"

#include <stdio.h>

// Entry point: parse CLI args, then start server.
int main(int argc, char **argv) {
    server_config_t cfg;

    // Validate and load config from command line.
    if (parse_arguments(argc, argv, &cfg) != 0) {
        return 1;
    }

    // Run server loop.
    return run_server(&cfg);
}
