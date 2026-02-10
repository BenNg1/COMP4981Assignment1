#include "path.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Basic checks for directory traversal attempts.
static int contains_traversal(const char *p) {
    if (!p) return 1;

    // Reject backslashes to avoid weird platform confusion.
    if (strchr(p, '\\')) return 1;

    // Common traversal patterns.
    if (strcmp(p, "/..") == 0) return 1;
    if (strncmp(p, "/../", 4) == 0) return 1;
    if (strstr(p, "/../") != NULL) return 1;
    if (strlen(p) >= 3 && strcmp(p + strlen(p) - 3, "/..") == 0) return 1;

    // Basic encoded ".." patterns.
    if (strstr(p, "%2e%2e") || strstr(p, "%2E%2E") || strstr(p, "%2e.") || strstr(p, ".%2e") || strstr(p, "%2E.")) {
        return 1;
    }

    return 0;
}

// Ensure resolved path stays inside document root.
static int starts_with_doc_root(const char *full, const char *doc_root) {
    size_t root_len = strlen(doc_root);
    if (strncmp(full, doc_root, root_len) != 0) {
        return 0;
    }
    // Exact match or child path.
    return (full[root_len] == '\0' || full[root_len] == '/');
}

// Convert URL target into a safe filesystem path under doc_root.
int resolve_path(const char *doc_root, const char *url_target, char *out_path, size_t out_sz) {
    if (!doc_root || !url_target || !out_path || out_sz == 0) {
        return 500;
    }

    // Only absolute URL paths are allowed.
    if (url_target[0] != '/') {
        return 400;
    }

    // Strip query string and fragment.
    char target[PATH_MAX];
    size_t i = 0;
    while (url_target[i] != '\0' && url_target[i] != '?' && url_target[i] != '#' && i < sizeof(target) - 1) {
        target[i] = url_target[i];
        i++;
    }
    target[i] = '\0';

    if (target[0] == '\0') {
        return 400;
    }

    // Block traversal attempts early.
    if (contains_traversal(target)) {
        return 403;
    }

    char normalized[PATH_MAX];

    // Default document handling.
    if (strcmp(target, "/") == 0) {
        snprintf(normalized, sizeof(normalized), "/index.html");
    } else if (target[strlen(target) - 1] == '/') {
        if (strlen(target) + strlen("index.html") + 1 > sizeof(normalized)) {
            return 400;
        }
        snprintf(normalized, sizeof(normalized), "%sindex.html", target);
    } else {
        snprintf(normalized, sizeof(normalized), "%s", target);
    }

    // Build candidate absolute path.
    char candidate[PATH_MAX];
    if (snprintf(candidate, sizeof(candidate), "%s%s", doc_root, normalized) >= (int)sizeof(candidate)) {
        return 400;
    }

    // Canonicalize to resolve symlinks and "..".
    char canonical[PATH_MAX];
    if (!realpath(candidate, canonical)) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return 404;
        }
        if (errno == EACCES) {
            return 403;
        }
        return 500;
    }

    // Final safety check: path must remain inside doc_root.
    if (!starts_with_doc_root(canonical, doc_root)) {
        return 403;
    }

    // Must be a regular file.
    struct stat st;
    if (stat(canonical, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) return 404;
        if (errno == EACCES) return 403;
        return 500;
    }

    if (!S_ISREG(st.st_mode)) {
        return 403;
    }

    // Return resolved safe path.
    if (snprintf(out_path, out_sz, "%s", canonical) >= (int)out_sz) {
        return 500;
    }

    return 0;
}
