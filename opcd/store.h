#ifndef WLAN_OPC_OPCD_STORE_H
#define WLAN_OPC_OPCD_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Write `len` bytes from `data` to `path` atomically via temp-file + rename.
 * The temp file is created next to `path` (same directory) so the rename is
 * within a single filesystem. The file is fsync()'d before rename. `mode`
 * is the final file mode (e.g. 0600 for password).
 *
 * Returns 0 on success, -1 on error (errno preserved).
 */
int opc_store_write_atomic(const char *path, const void *data, size_t len, mode_t mode);

/*
 * Read up to `cap` bytes from `path` into `buf`. Returns the number of bytes
 * read (>= 0) on success, -1 on error.
 */
ssize_t opc_store_read_all(const char *path, void *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_STORE_H */
