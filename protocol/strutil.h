#ifndef WLAN_OPC_PROTOCOL_STRUTIL_H
#define WLAN_OPC_PROTOCOL_STRUTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Length of a NUL-terminated string, capped at `cap` (never scans past it).
 * Equivalent to strnlen(), but provided here so the codec does not depend on
 * the _GNU_SOURCE/_POSIX_C_SOURCE feature-test gating strnlen() in <string.h>.
 * Shared by commands.c and indications.c (previously a duplicated static
 * definition in each — STYLE-003).
 */
static inline size_t opc_bounded_strnlen(const char *s, size_t cap)
{
    size_t i = 0;
    while (i < cap && s[i] != '\0') {
        i++;
    }
    return i;
}

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_PROTOCOL_STRUTIL_H */
