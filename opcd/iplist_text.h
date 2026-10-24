#ifndef WLAN_OPC_OPCD_IPLIST_TEXT_H
#define WLAN_OPC_OPCD_IPLIST_TEXT_H

/* IP address list as the customer-specified config file (#194, spec §4.3.6
 * Figure 4-5): per list N (1..128) five lines,
 *
 *   IPAddressN=a.b.c.d
 *   IPSubnetMaskN=a.b.c.d
 *   IPDefaultGatewayN=a.b.c.d
 *   WL54EssIdN="ESSID"
 *   NTPServerN=a.b.c.d
 *
 * This module is the text grammar only — serialize a list and split a text
 * into per-number fields. Value validation (the SetIpConfigList rules) and the
 * strict/lenient policy live with the callers in handler.c. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "opcd_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest text opcd writes: 128 lists, every value at its longest, LF line
 * ends, is 21,604 bytes; the async store job buffer is sized to this. */
#define OPC_IPLIST_TEXT_MAX   24576
/* Largest file the import (-I) accepts. Bigger than any valid list so a CRLF
 * or blank-line file still fits; anything larger is not a list. */
#define OPC_IPLIST_IMPORT_MAX 65536

/* Bits of opc_iplist_text_t.keys. */
#define OPC_IPLIST_KEY_IP     0x01u
#define OPC_IPLIST_KEY_MASK   0x02u
#define OPC_IPLIST_KEY_GW     0x04u
#define OPC_IPLIST_KEY_ESSID  0x08u
#define OPC_IPLIST_KEY_NTP    0x10u
#define OPC_IPLIST_KEY_ALL    0x1Fu

typedef struct opc_iplist_text {
    opc_ipcfg_entry_t entry[OPC_IPCFG_LIST_MAX_SLOTS];  /* [N-1]; host order, essid NUL-terminated */
    uint8_t keys[OPC_IPCFG_LIST_MAX_SLOTS];             /* OPC_IPLIST_KEY_* seen for N */
    bool    bad[OPC_IPCFG_LIST_MAX_SLOTS];              /* a line for N was malformed or repeated a key */
    int     first_error_line;    /* 1-based line of the first malformed line, 0 if none */
    bool    final_newline;       /* the text is empty or ends with '\n' */
} opc_iplist_text_t;

/* Serialize the populated slots of `l` in number order, LF line ends.
 * Returns the length written (no NUL), or -1 when `cap` is too small. */
ssize_t opc_iplist_text_format(const opcd_ip_list_t *l, char *buf, size_t cap);

/* Split `len` bytes into `out`. A line is blank (empty after an optional
 * trailing CR) or exactly `<Key><N>=<value>`: N decimal 1..128 without a
 * leading zero, IPv4 values as four decimal octets 0..255 without leading
 * zeros, the ESSID as "..." of at most 31 characters with no '"' inside.
 * Anything else — an unknown key, a NUL byte, trailing text, a key repeated
 * for the same N — is malformed: the line is recorded in first_error_line
 * (and `why`, when non-NULL) and, when its N is known, bad[N-1] is set.
 * Parsing always runs to the end. Returns first_error_line. */
int opc_iplist_text_parse(const char *buf, size_t len, opc_iplist_text_t *out,
                          char *why, size_t whycap);

#ifdef __cplusplus
}
#endif

#endif /* WLAN_OPC_OPCD_IPLIST_TEXT_H */
