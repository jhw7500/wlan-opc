#ifndef WLAN_OPC_OPCD_JSON_UTIL_H
#define WLAN_OPC_OPCD_JSON_UTIL_H

/*
 * Minimal JSON value extractor used by opcd (cantops link.json reader and
 * /usr/local/opc/etc/device_info.json loader).
 *
 * No external dependency. Top-level and one-level-nested ("section": { ... })
 * key lookups, string and signed-decimal integer values. The matcher requires
 * the character after the closing quote of a key to be a colon or JSON
 * whitespace so that prefix collisions like "mac" / "mac_address" cannot
 * silently misfire.
 *
 * The same parser was previously a static block in platform_nxp.c; it now
 * lives here so both platform_nxp.c and inventory.c can share it.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read entire file into a heap buffer; caller frees with free().
 * Returns NULL on failure with errno set (fopen / fseek / ftell / fread errors,
 * or synthetic EFBIG for the 1MB cap). */
char *opc_json_slurp_file(const char *path);

/* Extract a top-level `"key": "VALUE"` quoted string into `out` (NUL-terminated,
 * truncated to cap). Returns 0 on success, -ENOENT if absent, -EINVAL on bad
 * args. */
int opc_json_string(const char *json, const char *key, char *out, size_t cap);

/* Extract a top-level `"key": NUMBER` signed-decimal integer. Returns 0 on
 * success, -ENOENT if absent, -EINVAL on bad args or non-numeric value. */
int opc_json_integer(const char *json, const char *key, long *out);

/* Extract `"key": "VALUE"` string confined to a top-level
 * `"section": { ... }` object body. Brace-depth tracking handles one level
 * of nesting; assumes JSON string values contain no `{`/`}`. */
int opc_json_string_section(const char *json, const char *section,
                            const char *key, char *out, size_t cap);

/* Same as opc_json_string_section but for signed-decimal integer values. */
int opc_json_integer_section(const char *json, const char *section,
                             const char *key, long *out);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_JSON_UTIL_H */
