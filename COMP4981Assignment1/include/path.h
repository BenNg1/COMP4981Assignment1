#ifndef PATH_H
#define PATH_H

#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Returns:
//   0   success (out_path filled with canonical file path)
// 400   bad URL/path format
// 403   forbidden (traversal or outside doc root)
// 404   not found
// 500   other filesystem/server error
int resolve_path(const char *doc_root, const char *url_target, char *out_path, size_t out_sz);

#endif
