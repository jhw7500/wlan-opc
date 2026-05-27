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
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
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

static int stub_get_wlan_mac(int idx, uint8_t mac[6])
{
    if (idx < 0 || idx >= 1) {
        return -ENODEV;
    }
    memset(mac, 0, 6);
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
    return stub_copy_string(buf, cap, "wlan-opc-0.1.0");
}

static int stub_get_hardware_version(char *buf, size_t cap)
{
    return stub_copy_string(buf, cap, "NXP88W9098");
}

static int stub_get_serial_number(char *buf, size_t cap)
{
    return stub_copy_string(buf, cap, "SN-STUB-0001");
}

static int stub_get_manufacture_date(opc_date_t *out)
{
    out->year = 2026;
    out->month = 2;
    out->day = 28;
    return 0;
}

static int stub_get_shipment_date(opc_date_t *out)
{
    out->year = 2026;
    out->month = 3;
    out->day = 15;
    return 0;
}

static int stub_get_caps(opcd_platform_caps_t *out)
{
    memset(out, 0, sizeof *out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Link                                                               */
/* ------------------------------------------------------------------ */

static int stub_get_wlan_count(void)
{
    /* SINGLE — see platform.h note: stub MUST return >=1 even though it
     * has no real hardware to enumerate. opcd treats count=0 as fatal. */
    return 1;
}

static int stub_get_link(int idx, opcd_platform_link_t *out)
{
    if (idx != 0) {
        return -ENODEV;
    }
    memset(out, 0, sizeof *out);
    out->associated = false;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Mutations — stub accepts everything (kernel-less)                  */
/* ------------------------------------------------------------------ */

static int stub_apply_radio_config(const opc_set_radio_config_req_t *cfg)
{
    (void)cfg;
    return 0;
}

static int stub_apply_ip_change(const opc_ipcfg_entry_t *slot)
{
    (void)slot;
    return 0;
}

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
    .get_wlan_mac          = stub_get_wlan_mac,
    .get_firmware_version  = stub_get_firmware_version,
    .get_hardware_version  = stub_get_hardware_version,
    .get_serial_number     = stub_get_serial_number,
    .get_manufacture_date  = stub_get_manufacture_date,
    .get_shipment_date     = stub_get_shipment_date,
    .get_caps              = stub_get_caps,
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
 * this file (see platform.h "EXACTLY ONE" note), so dup-symbol on g_ops
 * acts as the build-time guard. */
static const opcd_platform_ops_t *g_ops;

const opcd_platform_ops_t *opcd_platform(void)
{
    return g_ops;
}

void opcd_platform_stub_register(void)
{
    /* Per platform.h: surface accidental double-register as abort, not
     * silent clobber. */
    assert(g_ops == NULL);
    g_ops = &g_stub_ops;
}
