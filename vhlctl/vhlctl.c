/*
 * vhlctl — VHL-side UDP/IP CLI simulator for the VHL ↔ wireless-board protocol.
 *
 * Usage:
 *   vhlctl [--host HOST] [--port PORT] [--timeout MS] SUBCOMMAND [args]
 *
 * Subcommands:
 *   login [--password PW]
 *   logout
 *   basic-info
 *   device-info
 *   set-password --old PW --new PW
 *   set-ip-list  --slot N --flag start|cont|end
 *                --ip A.B.C.D --mask A.B.C.D --gw A.B.C.D --ntp A.B.C.D --essid NAME
 *   change-ip    --slot N
 *   set-radio    --station single|dual
 *                --w1-freq F --w1-ch CH --w1-mode N --w1-bw N
 *               [--w2-freq F --w2-ch CH --w2-mode N --w2-bw N --priority HEX]
 *   set-indication --bits HEX --period S --to A.B.C.D:PORT
 *   reset
 *   listen --bind HOST:PORT
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "commands.h"
#include "ids.h"
#include "indications.h"
#include "fielddump.h"

static const char *g_host       = "127.0.0.1";
static int         g_port       = 50607;
static int         g_timeout_ms = 2000;
static uint16_t    g_seq        = 1;
static bool        g_dump       = false;
static bool        g_hex        = false;

static void hex_dump(const char *label, const uint8_t *buf, size_t len)
{
    fprintf(stderr, "  [%s] len=%zu\n", label, len);
    for (size_t i = 0; i < len; i += 16) {
        fprintf(stderr, "    %04zx ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) fprintf(stderr, "%02x ", buf[i + j]);
            else             fprintf(stderr, "   ");
            if (j == 7) fprintf(stderr, " ");
        }
        fprintf(stderr, " |");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = buf[i + j];
            fputc((c >= 0x20 && c < 0x7f) ? c : '.', stderr);
        }
        fprintf(stderr, "|\n");
    }
}

static uint16_t next_seq(void) { return g_seq++; }

static const char *result_str(uint16_t r) { return r == OPC_RESULT_OK ? "OK" : "NG"; }
static const char *err_str(uint16_t e)
{
    /* Canonical names live in protocol/ids.h. The OPC spec overloads 0x0010
     * across four command-specific meanings, so this value→string map can only
     * label it generically — interpret 0x0010 by the command that returned it. */
    switch (e) {
    case 0x0000: return "none";
    case 0x0001: return "login-violation";
    case 0x0002: return "login-condition";
    case 0x0003: return "packet-size";
    case 0x0004: return "nvram";
    /* 0x0010..0x0013 are spec-overloaded across command-specific meanings
     * (see ids.h), so they cannot map to a single named constant — keep the
     * literals with combined labels. Interpret by the issuing command. */
    case 0x0010:                     return "0x0010 (indication-violation/pw-mismatch/slot-range/station-type)";
    case 0x0011:                     return "0x0011 (slot-empty/radio-freq)";
    case 0x0012:                     return "0x0012 (ip-change-conflict/ind-recipient-ip)";
    case 0x0013:                     return "0x0013 (radio-mode/ind-other-ip)";
    case OPC_ERR_RADIO_BW:           return "0x0014 (radio-bw)";
    case OPC_ERR_LIST_SEQUENCE:      return "0x0018 (list-sequence)";
    /* Sent by firmware predating the D9 fix (PR #34) — kept for decode of
     * mixed-version fleets. */
    case 0x0050:                     return "0x0050 (radio-apply, deprecated)";
    default:     return "other";
    }
}

static const char *opt_value(int argc, char **argv, const char *flag, const char *dflt)
{
    for (int i = 0; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    return dflt;
}

static uint32_t parse_ipv4(const char *s)
{
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) return 0;
    return ntohl(a.s_addr);
}

static int parse_host_port(const char *spec, char *host_out, size_t host_cap, int *port_out)
{
    const char *colon = strchr(spec, ':');
    if (!colon) return -1;
    size_t hl = (size_t)(colon - spec);
    if (hl == 0 || hl >= host_cap) return -1;
    memcpy(host_out, spec, hl);
    host_out[hl] = '\0';
    *port_out = atoi(colon + 1);
    return (*port_out > 0) ? 0 : -1;
}

static int open_client_socket(struct sockaddr_in *dst)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct timeval tv = {
        .tv_sec  = g_timeout_ms / 1000,
        .tv_usec = (g_timeout_ms % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    memset(dst, 0, sizeof *dst);
    dst->sin_family = AF_INET;
    dst->sin_port   = htons((uint16_t)g_port);
    if (inet_pton(AF_INET, g_host, &dst->sin_addr) != 1) {
        fprintf(stderr, "invalid --host %s\n", g_host);
        close(fd);
        return -1;
    }
    return fd;
}

static ssize_t send_recv(int fd, struct sockaddr_in *dst,
                         const uint8_t *tx, ssize_t tx_len,
                         uint8_t *rx, size_t rx_cap)
{
    /* A negative tx_len means the request pack failed (capacity/arg). Guard
     * here so callers can pass the raw pack result without casting -1 into a
     * huge size_t that sendto would over-read (ARCH-003 / STYLE-005). */
    if (tx_len < 0) {
        fprintf(stderr, "request pack failed\n");
        return -1;
    }
    if (g_dump) hex_dump("TX", tx, (size_t)tx_len);
    if (sendto(fd, tx, (size_t)tx_len, 0, (struct sockaddr *)dst, sizeof *dst) != (ssize_t)tx_len) {
        perror("sendto"); return -1;
    }
    ssize_t n = recv(fd, rx, rx_cap, 0);
    if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            fprintf(stderr, "timeout waiting for response\n");
        else
            perror("recv");
        return -1;
    }
    if (g_dump) hex_dump("RX", rx, (size_t)n);
    return n;
}

static void print_mac(const char *label, const uint8_t mac[6])
{
    printf("  %s = %02x:%02x:%02x:%02x:%02x:%02x\n",
           label, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void print_ipv4(const char *label, uint32_t host_ip)
{
    printf("  %s = %u.%u.%u.%u\n", label,
           (host_ip >> 24) & 0xff, (host_ip >> 16) & 0xff,
           (host_ip >> 8) & 0xff,  host_ip & 0xff);
}

static int cmd_login(int argc, char **argv)
{
    const char *pw = opt_value(argc, argv, "--password", "MyPassword");
    opc_login_req_t req; memset(&req, 0, sizeof req);
    strncpy(req.password, pw, sizeof req.password - 1);
    struct sockaddr_in dst; int fd = open_client_socket(&dst);
    if (fd < 0) return 2;
    uint8_t tx[OPC_FRAME_MAX], rx[OPC_FRAME_MAX];
    ssize_t tn = opc_login_req_pack(tx, sizeof tx, next_seq(), &req);
    ssize_t rn = send_recv(fd, &dst, tx, tn, rx, sizeof rx);
    close(fd);
    if (rn < 0) return 2;
    opc_login_ack_t ack;
    if (opc_login_ack_unpack(rx, (size_t)rn, &ack) != 0) { fprintf(stderr, "login: malformed ack\n"); return 2; }
    printf("login: %s err=%s(0x%04x)\n", result_str(ack.result), err_str(ack.error_cause), ack.error_cause);
    return ack.result == OPC_RESULT_OK ? 0 : 1;
}

static int cmd_logout(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct sockaddr_in dst; int fd = open_client_socket(&dst);
    if (fd < 0) return 2;
    uint8_t tx[OPC_FRAME_MAX], rx[OPC_FRAME_MAX];
    ssize_t tn = opc_logout_req_pack(tx, sizeof tx, next_seq());
    ssize_t rn = send_recv(fd, &dst, tx, tn, rx, sizeof rx);
    close(fd);
    if (rn < 0) return 2;
    opc_logout_ack_t ack;
    if (opc_logout_ack_unpack(rx, (size_t)rn, &ack) != 0) { fprintf(stderr, "logout: malformed ack\n"); return 2; }
    printf("logout: %s err=%s(0x%04x)\n", result_str(ack.result), err_str(ack.error_cause), ack.error_cause);
    return ack.result == OPC_RESULT_OK ? 0 : 1;
}

static int cmd_basic_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct sockaddr_in dst; int fd = open_client_socket(&dst);
    if (fd < 0) return 2;
    uint8_t tx[OPC_FRAME_MAX], rx[OPC_FRAME_MAX];
    ssize_t tn = opc_get_basic_info_req_pack(tx, sizeof tx, next_seq());
    ssize_t rn = send_recv(fd, &dst, tx, tn, rx, sizeof rx);
    close(fd);
    if (rn < 0) return 2;
    if (g_hex) {
        printf("basic-info (--hex):\n");
        opc_get_basic_info_ack_t ack_hex;   /* basic-info has no result field — validate framing only */
        int bad = (opc_get_basic_info_ack_unpack(rx, (size_t)rn, &ack_hex) != 0);
        if (bad) fprintf(stderr, "basic-info: malformed ack\n");   /* error before the stdout dump */
        fd_dump_basic_info(stdout, rx, (size_t)rn);                /* still dump raw bytes for debugging */
        return bad ? 2 : 0;
    }
    opc_get_basic_info_ack_t ack;
    if (opc_get_basic_info_ack_unpack(rx, (size_t)rn, &ack) != 0) { fprintf(stderr, "basic-info: malformed ack\n"); return 2; }
    printf("basic-info:\n");
    printf("  vendor_code     = 0x%08x\n", ack.vendor_code);
    printf("  product_code    = 0x%04x\n", ack.product_code);
    printf("  product_subcode = 0x%04x\n", ack.product_subcode);
    printf("  device_status   = 0x%08x %s\n", ack.device_status,
           ack.device_status == OPC_DEVICE_LOGGED_IN ? "(logged-in)" :
           ack.device_status == OPC_DEVICE_READY     ? "(ready)" : "(booting)");
    printf("  station_type    = %s\n", ack.station_type == OPC_STATION_DUAL ? "DUAL" : "SINGLE");
    return 0;
}

static int cmd_device_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct sockaddr_in dst; int fd = open_client_socket(&dst);
    if (fd < 0) return 2;
    uint8_t tx[OPC_FRAME_MAX], rx[OPC_FRAME_MAX];
    ssize_t tn = opc_get_device_info_req_pack(tx, sizeof tx, next_seq());
    ssize_t rn = send_recv(fd, &dst, tx, tn, rx, sizeof rx);
    close(fd);
    if (rn < 0) return 2;
    if (g_hex) {
        printf("device-info (--hex):\n");
        opc_get_device_info_ack_t ack_hex;
        int bad = (opc_get_device_info_ack_unpack(rx, (size_t)rn, &ack_hex) != 0);
        if (bad) fprintf(stderr, "device-info: malformed ack\n");   /* error before the stdout dump */
        fd_dump_device_info(stdout, rx, (size_t)rn);                /* still dump raw bytes for debugging */
        if (bad) return 2;
        return ack_hex.result == OPC_RESULT_OK ? 0 : 1;
    }
    opc_get_device_info_ack_t ack;
    if (opc_get_device_info_ack_unpack(rx, (size_t)rn, &ack) != 0) { fprintf(stderr, "device-info: malformed ack\n"); return 2; }
    if (ack.result != OPC_RESULT_OK) {
        printf("device-info: NG err=%s(0x%04x)\n", err_str(ack.error_cause), ack.error_cause);
        return 1;
    }
    printf("device-info:\n");
    printf("  vendor=0x%08x product=0x%04x subcode=0x%04x\n",
           ack.vendor_code, ack.product_code, ack.product_subcode);
    printf("  firmware=%s hardware=%s serial=%s\n",
           ack.firmware_version, ack.hardware_version, ack.serial_number);
    print_mac("ethernet_mac", ack.ethernet_mac);
    print_ipv4("ip_address",      ack.ip_address);
    print_ipv4("subnet_mask",     ack.subnet_mask);
    print_ipv4("default_gateway", ack.default_gateway);
    print_ipv4("ntp_server",      ack.ntp_server);
    printf("  essid='%s'\n", ack.essid);
    printf("  device_status=0x%08x station_type=%s priority_ch=0x%04x\n",
           ack.device_status,
           ack.station_type == OPC_STATION_DUAL ? "DUAL" : "SINGLE",
           ack.priority_ch);
    printf("  802.11r=%u 11ai=%u 11k=%u 11v=%u\n",
           ack.ieee_11r, ack.ieee_11ai, ack.ieee_11k, ack.ieee_11v);
    printf("  WLAN#1: mode=%u bw=%u freq=%u ch=0x%04x status=0x%04x SNR=%d RSSI=%d\n",
           ack.wlan1.mode, ack.wlan1.bandwidth, ack.wlan1.freq_mhz,
           ack.wlan1.channel, ack.wlan1.status, ack.wlan1.snr, ack.wlan1.rssi);
    print_mac("WLAN#1 mac",            ack.wlan1.mac);
    print_mac("WLAN#1 connect_ap_mac", ack.wlan1.connect_ap_mac);
    if (ack.station_type == OPC_STATION_DUAL) {
        printf("  WLAN#2: mode=%u bw=%u freq=%u ch=0x%04x status=0x%04x SNR=%d RSSI=%d\n",
               ack.wlan2.mode, ack.wlan2.bandwidth, ack.wlan2.freq_mhz,
               ack.wlan2.channel, ack.wlan2.status, ack.wlan2.snr, ack.wlan2.rssi);
        print_mac("WLAN#2 mac",            ack.wlan2.mac);
        print_mac("WLAN#2 connect_ap_mac", ack.wlan2.connect_ap_mac);
    }
    return 0;
}

static int cmd_set_password(int argc, char **argv)
{
    const char *old_pw = opt_value(argc, argv, "--old", NULL);
    const char *new_pw = opt_value(argc, argv, "--new", NULL);
    if (!old_pw || !new_pw) { fprintf(stderr, "set-password: --old and --new required\n"); return 2; }
    opc_set_password_req_t req; memset(&req, 0, sizeof req);
    strncpy(req.old_password, old_pw, sizeof req.old_password - 1);
    strncpy(req.new_password, new_pw, sizeof req.new_password - 1);
    struct sockaddr_in dst; int fd = open_client_socket(&dst);
    if (fd < 0) return 2;
    uint8_t tx[OPC_FRAME_MAX], rx[OPC_FRAME_MAX];
    ssize_t tn = opc_set_password_req_pack(tx, sizeof tx, next_seq(), &req);
    ssize_t rn = send_recv(fd, &dst, tx, tn, rx, sizeof rx);
    close(fd);
    if (rn < 0) return 2;
    opc_set_password_ack_t ack;
    if (opc_set_password_ack_unpack(rx, (size_t)rn, &ack) != 0) { fprintf(stderr, "set-password: malformed ack\n"); return 2; }
    printf("set-password: %s err=%s(0x%04x)\n", result_str(ack.result), err_str(ack.error_cause), ack.error_cause);
    return ack.result == OPC_RESULT_OK ? 0 : 1;
}

static int cmd_set_ip_list(int argc, char **argv)
{
    const char *flag_s  = opt_value(argc, argv, "--flag",  "start");
    const char *slot_s  = opt_value(argc, argv, "--slot",  "1");
    const char *ip_s    = opt_value(argc, argv, "--ip",    "192.168.1.10");
    const char *mask_s  = opt_value(argc, argv, "--mask",  "255.255.255.0");
    const char *gw_s    = opt_value(argc, argv, "--gw",    "192.168.1.1");
    const char *ntp_s   = opt_value(argc, argv, "--ntp",   "192.168.1.2");
    const char *essid_s = opt_value(argc, argv, "--essid", "Cantops_WL");
    uint16_t flag;
    if      (!strcmp(flag_s, "start"))     flag = OPC_LIST_BOUNDARY_START;
    else if (!strcmp(flag_s, "cont"))      flag = OPC_LIST_BOUNDARY_CONTINUE;
    else if (!strcmp(flag_s, "end"))       flag = OPC_LIST_BOUNDARY_END;
    else {
        fprintf(stderr, "set-ip-list: --flag must be start|cont|end (got \"%s\")\n", flag_s);
        return 2;
    }
    opc_set_ip_config_list_req_t req; memset(&req, 0, sizeof req);
    req.entry_count = 1;
    req.entries[0].boundary_flag    = flag;
    req.entries[0].list_number      = (uint16_t)atoi(slot_s);
    req.entries[0].ip_address       = parse_ipv4(ip_s);
    req.entries[0].subnet_mask      = parse_ipv4(mask_s);
    req.entries[0].default_gateway  = parse_ipv4(gw_s);
    req.entries[0].ntp_server       = parse_ipv4(ntp_s);
    strncpy(req.entries[0].essid, essid_s, sizeof req.entries[0].essid - 1);
    struct sockaddr_in dst; int fd = open_client_socket(&dst);
    if (fd < 0) return 2;
    uint8_t tx[OPC_FRAME_MAX], rx[OPC_FRAME_MAX];
    ssize_t tn = opc_set_ip_config_list_req_pack(tx, sizeof tx, next_seq(), &req);
    ssize_t rn = send_recv(fd, &dst, tx, tn, rx, sizeof rx);
    close(fd);
    if (rn < 0) return 2;
    opc_set_ip_config_list_ack_t ack;
    if (opc_set_ip_config_list_ack_unpack(rx, (size_t)rn, &ack) != 0) { fprintf(stderr, "set-ip-list: malformed ack\n"); return 2; }
    printf("set-ip-list[slot=%u, flag=%s]: %s err=%s(0x%04x)\n",
           req.entries[0].list_number, flag_s,
           result_str(ack.result), err_str(ack.error_cause), ack.error_cause);
    return ack.result == OPC_RESULT_OK ? 0 : 1;
}

static int cmd_change_ip(int argc, char **argv)
{
    const char *slot_s = opt_value(argc, argv, "--slot", NULL);
    if (!slot_s) { fprintf(stderr, "change-ip: --slot required\n"); return 2; }
    opc_change_ip_address_req_t req = { .list_number = (uint16_t)atoi(slot_s) };
    struct sockaddr_in dst; int fd = open_client_socket(&dst);
    if (fd < 0) return 2;
    uint8_t tx[OPC_FRAME_MAX], rx[OPC_FRAME_MAX];
    ssize_t tn = opc_change_ip_address_req_pack(tx, sizeof tx, next_seq(), &req);
    ssize_t rn = send_recv(fd, &dst, tx, tn, rx, sizeof rx);
    close(fd);
    if (rn < 0) return 2;
    opc_change_ip_address_ack_t ack;
    if (opc_change_ip_address_ack_unpack(rx, (size_t)rn, &ack) != 0) { fprintf(stderr, "change-ip: malformed ack\n"); return 2; }
    printf("change-ip[slot=%u]: %s err=%s(0x%04x)\n", req.list_number,
           result_str(ack.result), err_str(ack.error_cause), ack.error_cause);
    return ack.result == OPC_RESULT_OK ? 0 : 1;
}

static int cmd_set_radio(int argc, char **argv)
{
    const char *station_s  = opt_value(argc, argv, "--station",  "single");
    const char *w1_freq_s  = opt_value(argc, argv, "--w1-freq",  "5180");
    const char *w1_ch_s    = opt_value(argc, argv, "--w1-ch",    "0x0224");
    const char *w1_mode_s  = opt_value(argc, argv, "--w1-mode",  "11");
    const char *w1_bw_s    = opt_value(argc, argv, "--w1-bw",    "2");
    const char *w2_freq_s  = opt_value(argc, argv, "--w2-freq",  "0");
    const char *w2_ch_s    = opt_value(argc, argv, "--w2-ch",    "0");
    const char *w2_mode_s  = opt_value(argc, argv, "--w2-mode",  "0");
    const char *w2_bw_s    = opt_value(argc, argv, "--w2-bw",    "0");
    const char *prio_s     = opt_value(argc, argv, "--priority", "0");
    opc_set_radio_config_req_t req; memset(&req, 0, sizeof req);
    req.station_type = !strcmp(station_s, "dual") ? OPC_STATION_DUAL : OPC_STATION_SINGLE;
    req.priority_ch  = (uint16_t)strtoul(prio_s, NULL, 0);
    req.wlan1.freq_mhz  = (uint16_t)strtoul(w1_freq_s, NULL, 0);
    req.wlan1.channel   = (uint16_t)strtoul(w1_ch_s,   NULL, 0);
    req.wlan1.mode      = (uint8_t) strtoul(w1_mode_s, NULL, 0);
    req.wlan1.bandwidth = (uint8_t) strtoul(w1_bw_s,   NULL, 0);
    req.wlan2.freq_mhz  = (uint16_t)strtoul(w2_freq_s, NULL, 0);
    req.wlan2.channel   = (uint16_t)strtoul(w2_ch_s,   NULL, 0);
    req.wlan2.mode      = (uint8_t) strtoul(w2_mode_s, NULL, 0);
    req.wlan2.bandwidth = (uint8_t) strtoul(w2_bw_s,   NULL, 0);
    struct sockaddr_in dst; int fd = open_client_socket(&dst);
    if (fd < 0) return 2;
    uint8_t tx[OPC_FRAME_MAX], rx[OPC_FRAME_MAX];
    ssize_t tn = opc_set_radio_config_req_pack(tx, sizeof tx, next_seq(), &req);
    ssize_t rn = send_recv(fd, &dst, tx, tn, rx, sizeof rx);
    close(fd);
    if (rn < 0) return 2;
    opc_set_radio_config_ack_t ack;
    if (opc_set_radio_config_ack_unpack(rx, (size_t)rn, &ack) != 0) { fprintf(stderr, "set-radio: malformed ack\n"); return 2; }
    printf("set-radio: %s err=%s(0x%04x)\n", result_str(ack.result), err_str(ack.error_cause), ack.error_cause);
    return ack.result == OPC_RESULT_OK ? 0 : 1;
}

static int cmd_set_indication(int argc, char **argv)
{
    const char *bits_s   = opt_value(argc, argv, "--bits",   "0x81");
    const char *period_s = opt_value(argc, argv, "--period", "5");
    const char *to_s     = opt_value(argc, argv, "--to",     NULL);
    if (!to_s) { fprintf(stderr, "set-indication: --to IP:PORT required\n"); return 2; }
    char host[64]; int port;
    if (parse_host_port(to_s, host, sizeof host, &port) != 0) { fprintf(stderr, "set-indication: bad --to %s\n", to_s); return 2; }
    opc_set_indication_config_req_t req = {
        .recipient_port  = (uint16_t)port,
        .info_bits       = (uint8_t)strtoul(bits_s, NULL, 0),
        .period_seconds  = (uint8_t)atoi(period_s),
        .recipient_ip    = parse_ipv4(host),
    };
    struct sockaddr_in dst; int fd = open_client_socket(&dst);
    if (fd < 0) return 2;
    uint8_t tx[OPC_FRAME_MAX], rx[OPC_FRAME_MAX];
    ssize_t tn = opc_set_indication_config_req_pack(tx, sizeof tx, next_seq(), &req);
    ssize_t rn = send_recv(fd, &dst, tx, tn, rx, sizeof rx);
    close(fd);
    if (rn < 0) return 2;
    opc_set_indication_config_ack_t ack;
    if (opc_set_indication_config_ack_unpack(rx, (size_t)rn, &ack) != 0) { fprintf(stderr, "set-indication: malformed ack\n"); return 2; }
    printf("set-indication[bits=0x%02x period=%us to=%s]: %s err=%s(0x%04x)\n",
           req.info_bits, req.period_seconds, to_s,
           result_str(ack.result), err_str(ack.error_cause), ack.error_cause);
    return ack.result == OPC_RESULT_OK ? 0 : 1;
}

static int cmd_reset(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct sockaddr_in dst; int fd = open_client_socket(&dst);
    if (fd < 0) return 2;
    uint8_t tx[OPC_FRAME_MAX], rx[OPC_FRAME_MAX];
    ssize_t tn = opc_reset_req_pack(tx, sizeof tx, next_seq());
    ssize_t rn = send_recv(fd, &dst, tx, tn, rx, sizeof rx);
    close(fd);
    if (rn < 0) return 2;
    opc_reset_ack_t ack;
    if (opc_reset_ack_unpack(rx, (size_t)rn, &ack) != 0) { fprintf(stderr, "reset: malformed ack\n"); return 2; }
    printf("reset: %s err=%s(0x%04x)\n", result_str(ack.result), err_str(ack.error_cause), ack.error_cause);
    return ack.result == OPC_RESULT_OK ? 0 : 1;
}

static int cmd_listen(int argc, char **argv)
{
    const char *bind_s = opt_value(argc, argv, "--bind", "0.0.0.0:9999");
    char host[64]; int port;
    if (parse_host_port(bind_s, host, sizeof host, &port) != 0) { fprintf(stderr, "listen: bad --bind %s\n", bind_s); return 2; }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 2; }
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { close(fd); return 2; }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("bind"); close(fd); return 2; }
    fprintf(stderr, "listening on %s:%d for indications...\n", host, port);
    while (1) {
        uint8_t buf[OPC_FRAME_MAX];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) { if (errno == EINTR) continue; perror("recv"); break; }
        if (g_dump) hex_dump("IND", buf, (size_t)n);
        opc_header_t hdr;
        if (opc_frame_parse(buf, (size_t)n, &hdr, NULL, NULL) != 0) {
            fprintf(stderr, "  malformed frame %zd bytes\n", n); continue;
        }
        if (hdr.command_type != OPC_CMD_INDICATION) {
            fprintf(stderr, "  not an indication (cmd_type=0x%02x)\n", hdr.command_type); continue;
        }
        if (g_hex) {
            printf("[%u] indication 0x%04x (--hex):\n", hdr.sequence_number, hdr.req_indication_id);
            fd_dump_indication(stdout, hdr.req_indication_id, buf, (size_t)n);
            fflush(stdout);
            continue;
        }
        switch (hdr.req_indication_id) {
        case OPC_IND_INIT_COMPLETE: {
            opc_ind_init_complete_t in;
            if (opc_ind_init_complete_unpack(buf, (size_t)n, &in) == 0)
                printf("[%u] InitComplete       status=0x%08x\n", hdr.sequence_number, in.status);
            break;
        }
        case OPC_IND_WLAN_STATUS_CHANGE: {
            opc_ind_wlan_status_change_t in;
            if (opc_ind_wlan_status_change_unpack(buf, (size_t)n, &in) == 0)
                printf("[%u] WlanStatusChange   status=0x%04x ch=0x%04x\n",
                       hdr.sequence_number, in.wlan_status, in.indication_ch);
            break;
        }
        case OPC_IND_ROAMING: {
            opc_ind_roaming_t in;
            if (opc_ind_roaming_unpack(buf, (size_t)n, &in) == 0) {
                printf("[%u] Roaming            snr=%d rssi=%d ch=0x%04x\n",
                       hdr.sequence_number, in.current_snr, in.current_rssi, in.ch_number);
                print_mac("ap_mac", in.connect_ap_mac);
            }
            break;
        }
        case OPC_IND_AP_DISCONNECT: {
            opc_ind_ap_disconnect_t in;
            if (opc_ind_ap_disconnect_unpack(buf, (size_t)n, &in) == 0) {
                printf("[%u] ApDisconnect       msg=0x%04x reason=0x%04x\n",
                       hdr.sequence_number, in.message_id, in.result_code);
                print_mac("ap_mac", in.disconnect_ap_mac);
            }
            break;
        }
        case OPC_IND_FAULT_DETECT: {
            opc_ind_fault_detect_t in;
            if (opc_ind_fault_detect_unpack(buf, (size_t)n, &in) == 0)
                printf("[%u] FaultDetect        congestion=0x%04x val=%u\n",
                       hdr.sequence_number, in.congestion_id, in.current_val);
            break;
        }
        case OPC_IND_RESET_NOTICE: {
            opc_ind_reset_notice_t in;
            if (opc_ind_reset_notice_unpack(buf, (size_t)n, &in) == 0)
                printf("[%u] ResetNotice        cause=0x%08x\n",
                       hdr.sequence_number, in.reset_cause);
            break;
        }
        case OPC_IND_KEEP_ALIVE: {
            opc_ind_keep_alive_t in;
            if (opc_ind_keep_alive_unpack(buf, (size_t)n, &in) == 0)
                printf("[%u] KeepAlive          ts=%s\n", hdr.sequence_number, in.timestamp);
            break;
        }
        default:
            fprintf(stderr, "  unknown indication id 0x%04x\n", hdr.req_indication_id);
        }
        fflush(stdout);
    }
    close(fd);
    return 0;
}

static void usage(void)
{
    fputs("usage: vhlctl [--host HOST] [--port PORT] [--timeout MS] [--dump] [--hex] SUBCOMMAND [args]\n"
          "subcommands:\n"
          "  login [--password PW]\n"
          "  logout\n"
          "  basic-info\n"
          "  device-info\n"
          "  set-password --old PW --new PW\n"
          "  set-ip-list  --slot N --flag start|cont|end\n"
          "               --ip A.B.C.D --mask A.B.C.D --gw A.B.C.D --ntp A.B.C.D --essid NAME\n"
          "  change-ip    --slot N\n"
          "  set-radio    --station single|dual\n"
          "               --w1-freq F --w1-ch CH --w1-mode N --w1-bw N\n"
          "              [--w2-... --priority HEX]\n"
          "  set-indication --bits HEX --period S --to A.B.C.D:PORT\n"
          "  reset\n"
          "  listen --bind HOST:PORT\n",
          stderr);
}

int main(int argc, char **argv)
{
    int idx = 1;
    while (idx < argc && argv[idx][0] == '-' && argv[idx][1] == '-') {
        if      (!strcmp(argv[idx], "--host")    && idx + 1 < argc) { g_host       = argv[idx + 1]; idx += 2; }
        else if (!strcmp(argv[idx], "--port")    && idx + 1 < argc) { g_port       = atoi(argv[idx + 1]); idx += 2; }
        else if (!strcmp(argv[idx], "--timeout") && idx + 1 < argc) { g_timeout_ms = atoi(argv[idx + 1]); idx += 2; }
        else if (!strcmp(argv[idx], "--dump"))   { g_dump = true; idx += 1; }
        else if (!strcmp(argv[idx], "--hex"))    { g_hex  = true; idx += 1; }
        else if (!strcmp(argv[idx], "--help")) { usage(); return 0; }
        else break;
    }
    if (idx >= argc) { usage(); return 2; }
    const char *cmd = argv[idx];
    int sub_argc = argc - (idx + 1);
    char **sub_argv = argv + idx + 1;
    if (!strcmp(cmd, "login"))           return cmd_login(sub_argc, sub_argv);
    if (!strcmp(cmd, "logout"))          return cmd_logout(sub_argc, sub_argv);
    if (!strcmp(cmd, "basic-info"))      return cmd_basic_info(sub_argc, sub_argv);
    if (!strcmp(cmd, "device-info"))     return cmd_device_info(sub_argc, sub_argv);
    if (!strcmp(cmd, "set-password"))    return cmd_set_password(sub_argc, sub_argv);
    if (!strcmp(cmd, "set-ip-list"))     return cmd_set_ip_list(sub_argc, sub_argv);
    if (!strcmp(cmd, "change-ip"))       return cmd_change_ip(sub_argc, sub_argv);
    if (!strcmp(cmd, "set-radio"))       return cmd_set_radio(sub_argc, sub_argv);
    if (!strcmp(cmd, "set-indication"))  return cmd_set_indication(sub_argc, sub_argv);
    if (!strcmp(cmd, "reset"))           return cmd_reset(sub_argc, sub_argv);
    if (!strcmp(cmd, "listen"))          return cmd_listen(sub_argc, sub_argv);
    fprintf(stderr, "unknown subcommand: %s\n", cmd);
    usage();
    return 2;
}
