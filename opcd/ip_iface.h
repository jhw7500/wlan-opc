#ifndef WLAN_OPC_OPCD_IP_IFACE_H
#define WLAN_OPC_OPCD_IP_IFACE_H

/* Management-IP interface selector (opc.conf device_ip_iface).
 *
 * Spec §3.3.4 carries a single per-device "IP Address"; the shipping topology
 * keeps that management address on eth0 (wired), so ETH0 is the default and
 * yields zero behavioral change. MLAN0 is the opt-in for the mlan0-IP
 * ("option X") topology where the management plane lives on the wireless
 * interface (issue #89).
 *
 * The selector governs BOTH the GetDeviceInfo IP/netmask/gateway read source
 * AND the ChangeIp apply target: reading and writing must stay on the same
 * interface, or the VHL's spec loop (§3.3.6 set → §3.3.7 change → §3.3.4
 * re-read) diverges — the VHL would keep re-trying a change that silently
 * lands on the other interface (design review C1, 2026-09-02).
 *
 * Kept as a standalone module (like freq_source) so the pure opc.conf key
 * parsing is host-unit-testable without linking opcd.c's main(). */

typedef enum {
    OPC_IP_IFACE_ETH0 = 0,   /* wired management IP (shipping default) */
    OPC_IP_IFACE_MLAN0,      /* wireless management IP (option-X topology) */
} opcd_ip_iface_t;

/* Platform-facing interface index for get_dev_ipv4()/apply_ip_change()
 * (0=eth0, 1=mlan0). Single mapping point so handler call sites cannot
 * disagree on the encoding. */
static inline int opcd_ip_iface_idx(opcd_ip_iface_t v)
{
    return (v == OPC_IP_IFACE_MLAN0) ? 1 : 0;
}

/* Map an opc.conf value token to the enum. NULL or any unrecognized token
 * (including "") → OPC_IP_IFACE_ETH0 (shipping-default fallback).
 * Case-sensitive. */
opcd_ip_iface_t opcd_ip_iface_from_token(const char *val);

/* Parse opc.conf for `device_ip_iface = eth0|mlan0` (last value wins).
 * Missing/unreadable file, absent key, or unrecognized value all yield
 * OPC_IP_IFACE_ETH0. Same `key = value` line-scan as opcd_freq_source_parse();
 * '#'-comment and unrelated lines are skipped. */
opcd_ip_iface_t opcd_ip_iface_parse(const char *conf_path);

#endif /* WLAN_OPC_OPCD_IP_IFACE_H */
