#ifndef WLAN_OPC_OPCD_NTP_PARSE_H
#define WLAN_OPC_OPCD_NTP_PARSE_H

/*
 * /etc/systemd/timesyncd.conf NTP= line parser.
 *
 * Extracted from platform_nxp.c so the line-level grammar (whitespace,
 * comment, multi-server, hostname tolerance) is exercised by a host-side
 * unit test independent of the live timesyncd.conf at /etc.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a single line. Recognised form:
 *   [ whitespace ] "NTP=" SERVER [ whitespace SERVER ... ] [ # comment ]
 *
 * SERVER tokens are inspected in order; the first one that is a literal
 * IPv4 address writes its host-byte-order value into `*server_host` and
 * the function returns 0. Hostname tokens are silently skipped because
 * opcd has no resolver in the response path. If no token is a literal
 * IPv4 (or the line is not an NTP= line at all) returns -ENOENT and the
 * output is left unchanged.
 *
 * Lines from `strtok_r(... "\n" ...)` are accepted as-is; the parser tolerates
 * trailing whitespace/CR but does NOT require a NL to be present. */
int opc_ntp_parse_line(const char *line, uint32_t *server_host);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_NTP_PARSE_H */
