/*
 * Stub implementation of the opcd ↔ NXP platform boundary.
 *
 * Returns the same canned values that handler.c previously hardcoded — this
 * file is the destination for the "Platform stub fields — real values come
 * from the NXP driver later" comment that used to live in handler.c.
 *
 * Behaviour-preserving: linking this file alongside opcd MUST keep vhlctl
 * 20/20 PASS (no protocol field changes, only the source of those values).
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static int stub_init(void)
{
    return 0;
}

static void stub_teardown(void)
{
    /* no-op — async-signal-safe by construction */
}

/* ------------------------------------------------------------------ */
/* Identity                                                           */
/* ------------------------------------------------------------------ */

static int stub_get_eth_mac(uint8_t mac[6])
{
    memset(mac, 0, 6);
    return 0;
}

static int stub_get_eth_ipv4_host(uint32_t *ip_host)
{
    *ip_host = 0;
    return 0;
}

static int stub_get_eth_netmask_host(uint32_t *netmask_host)
{
    *netmask_host = 0;
    return 0;
}

static int stub_get_eth_gateway_host(uint32_t *gateway_host)
{
    *gateway_host = 0;
    return 0;
}

static int stub_get_wlan_count(void)
{
    /* SINGLE — see platform.h note: stub MUST return >=1 even though it
     * has no real hardware to enumerate. opcd treats count=0 as fatal. */
    return 1;
}

static int stub_get_wlan_mac(int idx, uint8_t mac[6])
{
    if (idx < 0 || idx >= stub_get_wlan_count()) {
        return -ENODEV;
    }
    memset(mac, 0, 6);
    return 0;
}

static int stub_get_essid(int idx, char *buf, size_t cap)
{
    if (idx < 0 || idx >= stub_get_wlan_count()) return -ENODEV;
    if (cap == 0) return -EINVAL;
    buf[0] = '\0';
    return 0;
}

static int stub_copy_string(char *buf, size_t cap, const char *src)
{
    if (cap == 0) return -EINVAL;
    size_t n = strnlen(src, cap - 1);
    memcpy(buf, src, n);
    buf[n] = '\0';
    return 0;
}

static int stub_get_firmware_version(char *buf, size_t cap)
{
    /* Empty: stub has no package manager to query. The default-installed
     * inventory.json still supplies vendor/product codes etc., but stays
     * silent on the live firmware version so callers can distinguish a
     * stub build from a real one. */
    return stub_copy_string(buf, cap, "");
}

static int stub_get_ntp_server(uint32_t *server_host)
{
    /* No timesyncd in stub environment — return "unconfigured" (0). */
    *server_host = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Link                                                               */
/* ------------------------------------------------------------------ */

/* Test-only injectable link state (default: not associated). Lets the handler
 * test exercise the device-info freq-source toggle's associated/live path
 * without a kernel. STUB ONLY — never consulted by platform_nxp.c. */
static bool                 s_link_set[2];
static opcd_platform_link_t s_link[2];

void stub_set_link(int idx, bool assoc, uint16_t freq, uint16_t ch)
{
    if (idx < 0 || idx > 1) return;
    memset(&s_link[idx], 0, sizeof s_link[idx]);
    s_link[idx].associated = assoc;
    s_link[idx].freq_mhz   = freq;
    s_link[idx].channel    = ch;
    s_link_set[idx] = true;
}

void stub_reset_link(void) { s_link_set[0] = s_link_set[1] = false; }

static int stub_get_link(int idx, opcd_platform_link_t *out)
{
    /* Same bounds form as stub_get_wlan_mac — kept identical so that bumping
     * stub_get_wlan_count() in future cannot cause one accessor to succeed
     * while the other returns -ENODEV. */
    if (idx < 0 || idx >= stub_get_wlan_count()) {
        return -ENODEV;
    }
    if (idx < 2 && s_link_set[idx]) {
        *out = s_link[idx];
        return 0;
    }
    memset(out, 0, sizeof *out);
    out->associated = false;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Mutations — stub accepts everything (kernel-less)                  */
/* ------------------------------------------------------------------ */

static int s_apply_radio_fail = 0;
static int s_apply_radio_fail_once = 0;
static int s_apply_radio_calls = 0;
static int s_apply_radio_last_w1_freq = 0;
static int s_apply_radio_last_station = 0;
static int stub_apply_radio_config(const opc_set_radio_config_req_t *cfg)
{
    /* Observability for the apply-failure revert (D9): handler.c re-applies the
     * last-good config on failure, so a failed Set-Radio drives TWO apply calls
     * and the LAST one carries the previous (reverted-to) config — including its
     * station_type, so a DUAL revert is verifiable from the handler test. */
    s_apply_radio_calls++;
    s_apply_radio_last_w1_freq  = cfg ? (int)cfg->wlan1.freq_mhz : 0;
    s_apply_radio_last_station  = cfg ? (int)cfg->station_type   : 0;
    /* Fail-once countdown: fail this call, then auto-clear so the NEXT call
     * (the best-effort revert) succeeds — proves a successful revert still
     * leaves the response NG 0x0050 (L2). */
    if (s_apply_radio_fail_once) { s_apply_radio_fail_once = 0; return -1; }
    /* Direct programmatic override (existing API — test 14f). */
    if (s_apply_radio_fail)
        return -1;
    /* Env-var injection for handler-dispatch error-path tests.
     * OPCD_STUB_APPLY_RADIO_RC=<negative errno>  e.g. -110 → -ETIMEDOUT,
     *                                                      -71 → -EPROTO.
     * Unset or zero → success (default behaviour preserved).
     * STUB ONLY — never consulted by platform_nxp.c. */
    const char *ev = getenv("OPCD_STUB_APPLY_RADIO_RC");
    if (ev && ev[0] != '\0') {
        int rc = atoi(ev);
        if (rc != 0)
            return rc;
    }
    return 0;
}

/* Test-only accessors (declared extern in test_handler.c). */
void stub_apply_radio_set_fail(int fail)      { s_apply_radio_fail = fail; }
void stub_apply_radio_set_fail_once(int fail) { s_apply_radio_fail_once = fail; }
int  stub_apply_radio_calls(void)             { return s_apply_radio_calls; }
void stub_apply_radio_reset_calls(void)       { s_apply_radio_calls = 0;
                                                s_apply_radio_last_w1_freq = 0;
                                                s_apply_radio_last_station = 0; }
int  stub_apply_radio_last_w1_freq(void)      { return s_apply_radio_last_w1_freq; }
int  stub_apply_radio_last_station(void)      { return s_apply_radio_last_station; }

/* Test observability: count apply_ip_change calls and record the last slot's IP
 * so the change-ip → platform wiring (deferred until logout) is verifiable from
 * the handler test. Counters are file-static; the test reads them via the
 * accessors below (keeps them out of the global namespace). */
static unsigned s_apply_ip_calls   = 0;
static uint32_t s_apply_ip_last_ip = 0;
static char     s_apply_ip_last_essid[OPC_ESSID_FIELD_LEN + 1] = {0};
static int      s_apply_ip_fail    = 0;
static int stub_apply_ip_change(const opc_ipcfg_entry_t *slot)
{
    s_apply_ip_calls++;
    s_apply_ip_last_ip = slot->ip_address;
    snprintf(s_apply_ip_last_essid, sizeof s_apply_ip_last_essid,
             "%.*s", (int)sizeof slot->essid, slot->essid);
    return s_apply_ip_fail ? -1 : 0;
}

/* Test-only accessors (declared extern in test_handler.c). */
unsigned    stub_apply_ip_calls(void)           { return s_apply_ip_calls; }
uint32_t    stub_apply_ip_last_ip(void)         { return s_apply_ip_last_ip; }
const char *stub_apply_ip_last_essid(void)      { return s_apply_ip_last_essid; }
void        stub_apply_ip_reset(void)           { s_apply_ip_calls = 0; s_apply_ip_last_ip = 0;
                                                  s_apply_ip_last_essid[0] = '\0'; s_apply_ip_fail = 0; }
void        stub_apply_ip_set_fail(int fail)    { s_apply_ip_fail = fail; }

static int stub_prepare_reset(void)
{
    return 0;
}

/* ------------------------------------------------------------------ */
/* Event multiplexing — stub has no async events                      */
/* ------------------------------------------------------------------ */

static int stub_event_fd(void)
{
    return -1;
}

static int stub_drain_events(opcd_platform_evt_cb cb, void *ctx)
{
    (void)cb;
    (void)ctx;
    return 0;
}

/* ------------------------------------------------------------------ */
/* vtable + registration                                              */
/* ------------------------------------------------------------------ */

static const opcd_platform_ops_t g_stub_ops = {
    .init                  = stub_init,
    .teardown              = stub_teardown,
    .get_eth_mac           = stub_get_eth_mac,
    .get_eth_ipv4_host     = stub_get_eth_ipv4_host,
    .get_eth_netmask_host  = stub_get_eth_netmask_host,
    .get_eth_gateway_host  = stub_get_eth_gateway_host,
    .get_wlan_mac          = stub_get_wlan_mac,
    .get_essid             = stub_get_essid,
    .get_firmware_version  = stub_get_firmware_version,
    .get_ntp_server        = stub_get_ntp_server,
    .get_wlan_count        = stub_get_wlan_count,
    .get_link              = stub_get_link,
    .apply_radio_config    = stub_apply_radio_config,
    .apply_ip_change       = stub_apply_ip_change,
    .prepare_reset         = stub_prepare_reset,
    .event_fd              = stub_event_fd,
    .drain_events          = stub_drain_events,
};

/* Single global ops pointer. Defined here because this file owns the
 * registry; a future platform_nxp.c will be built mutually exclusive with
 * this file (see platform.h "EXACTLY ONE" note). Both files define
 * opcd_platform_register() and opcd_platform() with external linkage, so
 * linking both yields a duplicate-symbol link error — that is the
 * intentional build-time mutual-exclusion guard. g_ops itself is static
 * (internal linkage) and would NOT collide; do not rely on it. */
static const opcd_platform_ops_t *g_ops;

const opcd_platform_ops_t *opcd_platform(void)
{
    return g_ops;
}

void opcd_platform_register(void)
{
    /* Per platform.h: surface accidental double-register as abort, not
     * silent clobber. Explicit check (not assert) survives -DNDEBUG. */
    if (g_ops != NULL) {
        abort();
    }
    g_ops = &g_stub_ops;
}
