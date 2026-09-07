#ifndef RETROARCH_ROMX_HOST_TRANSACTION_H
#define RETROARCH_ROMX_HOST_TRANSACTION_H

#include <stdbool.h>

/* Host-path primitives shared by persistence and SAVE editing. They never
 * follow a symbolic link/reparse point while deleting a transaction tree. */
bool romx_host_path_is_link(const char *path);
bool romx_host_remove_tree(const char *path);

#endif
