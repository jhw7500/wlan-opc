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
 * within a single filesystem. The file is fsync()'d before rename and the
 * parent directory afterward. `mode`
 * is the final file mode (e.g. 0600 for password).
 *
 * Returns 0 on success, -1 on error (errno preserved).
 */
typedef enum {
    OPC_STORE_UNCHANGED = 0,          /* target not replaced by this operation */
    OPC_STORE_REPLACED_UNSYNCED,      /* new target visible, durability unconfirmed */
    OPC_STORE_DURABLE,                /* file and parent-directory sync succeeded */
} opc_store_outcome_t;

/* On local Linux filesystems, outcome distinguishes failure before target
 * replacement from failure after rename. A post-rename error does NOT mean
 * the previous bytes survived and must not cause a blind memory-only rollback.
 * Remote filesystem rename ambiguity is outside this local-store contract. */
int opc_store_write_atomic_ex(const char *path, const void *data, size_t len,
                              mode_t mode, opc_store_outcome_t *outcome);

/* Compatibility wrapper. -1 may mean the target was already replaced;
 * reconciliation callers must use _ex and inspect its outcome. */
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
