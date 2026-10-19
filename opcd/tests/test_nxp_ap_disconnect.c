/* Exercise raw nl80211 parsing -> real NXP staging -> wire MAC. Including the
 * backend keeps its private event stager testable without production test
 * hooks, sockets, or radio changes. */
#include "../platform_nxp.c"

static int failures;
#define ASSERT(cond, label) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; } \
} while (0)

/* Deauth captured from the AP in #130: receiver=STA, transmitter=BSSID,
 * little-endian reason=3. Expected MACs below are independent literals. */
static const uint8_t deauth[] = {
    0xc0, 0x00, 0x4c, 0x00, 0x00, 0xe0, 0x4c, 0x68, 0x2b, 0x1f,
    0x58, 0x86, 0x94, 0xd2, 0x73, 0xe8,
    0x58, 0x86, 0x94, 0xd2, 0x73, 0xe8, 0xd0, 0xbe, 0x03, 0x00,
};
static const uint8_t ap_mac[6] = {0x58, 0x86, 0x94, 0xd2, 0x73, 0xe8};
static const uint8_t new_ap[6] = {0x00, 0x80, 0x4c, 0xc7, 0x7d, 0xdd};
static const uint8_t zero_mac[6];

static void test_attr(uint8_t *buf, size_t *off, uint16_t type,
                      const void *payload, uint16_t len)
{
    uint16_t size = 4 + len;
    memcpy(buf + *off, &size, 2);
    memcpy(buf + *off + 2, &type, 2);
    if (len) memcpy(buf + *off + 4, payload, len);
    *off += (size + 3u) & ~3u;
}

static opcd_nl_evt_t test_decode(uint8_t cmd, const uint8_t *frame, size_t len,
                                const uint8_t *mac, bool by_ap)
{
    uint8_t buf[256] = {0};
    uint16_t family = 28, reason = 3;
    uint32_t ifindex = 7, freq = 5180;
    size_t off = 20;
    memcpy(buf + 4, &family, 2);
    buf[16] = cmd;
    buf[17] = 1;
    test_attr(buf, &off, 3, &ifindex, 4);
    if (frame) test_attr(buf, &off, 51, frame, (uint16_t)len);
    if (mac) test_attr(buf, &off, 6, mac, 6);
    if (cmd == 46) test_attr(buf, &off, 38, &freq, 4);
    if (cmd == 48) test_attr(buf, &off, 54, &reason, 2);
    if (by_ap) test_attr(buf, &off, 71, NULL, 0);
    uint32_t total = (uint32_t)off;
    memcpy(buf, &total, 4);
    opcd_nl_evt_t evt;
    ASSERT(nl80211_parse_evt(buf, off, family, &evt) == 0,
           "raw netlink event recognized");
    return evt;
}

static void test_stage(opcd_platform_evt_t *tab, size_t *count, int idx,
                        uint8_t cmd, const uint8_t *frame, size_t len,
                        const uint8_t *mac, bool by_ap)
{
    opcd_nl_evt_t evt = test_decode(cmd, frame, len, mac, by_ap);
    nl_stage_evt(tab, count, &evt, idx);
}

static void test_reset(void)
{
    opcd_platform_evt_t tab[NL_COALESCE_MAX] = {0};
    size_t count = 0;
    test_stage(tab, &count, 0, 46, NULL, 0, new_ap, false);
    test_stage(tab, &count, 1, 46, NULL, 0, new_ap, false);
}

static void expect_wire(opcd_platform_evt_t *tab, size_t count, int idx,
                         uint16_t message, const uint8_t *mac, const char *label)
{
    const opcd_platform_evt_t *evt = NULL;
    for (size_t i = 0; i < count; i++)
        if (tab[i].kind == OPCD_PEVT_AP_DISCONNECT &&
            tab[i].u.ap_disconnect.idx == idx) evt = &tab[i];
    ASSERT(evt != NULL, "AP-origin disconnect emitted");
    if (!evt) return;
    opc_ind_ap_disconnect_t ind = {
        .message_id = evt->u.ap_disconnect.reason_msg_id,
        .result_code = evt->u.ap_disconnect.result_code,
    };
    memcpy(ind.disconnect_ap_mac, evt->u.ap_disconnect.mac, 6);
    uint8_t wire[76];
    ASSERT(opc_ind_ap_disconnect_pack(wire, sizeof wire, 215, &ind) == 76,
           "AP disconnect wire length");
    ASSERT(wire[64] == 0 && wire[65] == message &&
           wire[66] == 0 && wire[67] == 3, "wire subtype and reason preserved");
    ASSERT(memcmp(wire + 68, mac, 6) == 0, label);
}

static void test_captured_deauth_and_fast_reconnect(void)
{
    test_reset();
    opcd_platform_evt_t tab[NL_COALESCE_MAX] = {0};
    size_t count = 0;
    test_stage(tab, &count, 0, 39, deauth, sizeof deauth, NULL, false);
    test_stage(tab, &count, 0, 48, NULL, 0, NULL, true);
    /* Same drain/coalescing interval as the field event: the new connection
     * must not replace the AP that sent the earlier deauth. */
    test_stage(tab, &count, 0, 46, NULL, 0, new_ap, false);
    expect_wire(tab, count, 0, 12, ap_mac, "captured deauth AP MAC reaches wire");
}

static void test_disassoc_and_explicit_mac(void)
{
    test_reset();
    uint8_t frame[sizeof deauth];
    memcpy(frame, deauth, sizeof frame);
    frame[0] = 0xa0;
    opcd_platform_evt_t tab[NL_COALESCE_MAX] = {0};
    size_t count = 0;
    test_stage(tab, &count, 0, 40, frame, sizeof frame, NULL, false);
    test_stage(tab, &count, 0, 48, NULL, 0, NULL, true);
    expect_wire(tab, count, 0, 10, ap_mac, "disassoc frame AP MAC reaches wire");

    count = 0;
    test_stage(tab, &count, 0, 39, deauth, sizeof deauth, NULL, false);
    test_stage(tab, &count, 0, 48, NULL, 0, new_ap, true);
    expect_wire(tab, count, 0, 12, new_ap, "explicit disconnect MAC takes precedence");

    count = 0;
    test_stage(tab, &count, 0, 39, deauth, sizeof deauth, NULL, false);
    test_stage(tab, &count, 0, 48, NULL, 0, zero_mac, true);
    expect_wire(tab, count, 0, 12, ap_mac, "zero MAC attribute uses frame AP MAC");
}

static void test_hint_lifetime_and_interface(void)
{
    test_reset();
    opcd_platform_evt_t tab[NL_COALESCE_MAX] = {0};
    size_t count = 0;
    test_stage(tab, &count, 0, 39, deauth, sizeof deauth, NULL, false);
    test_stage(tab, &count, 1, 48, NULL, 0, NULL, true);
    expect_wire(tab, count, 1, 12, zero_mac, "other interface cannot use AP hint");
    test_stage(tab, &count, 0, 48, NULL, 0, NULL, true);
    expect_wire(tab, count, 0, 12, ap_mac, "same interface retains AP hint");
    count = 0;
    test_stage(tab, &count, 0, 48, NULL, 0, NULL, true);
    expect_wire(tab, count, 0, 12, zero_mac, "disconnect consumes AP hint once");

    count = 0;
    test_stage(tab, &count, 0, 39, deauth, sizeof deauth, NULL, false);
    test_stage(tab, &count, 0, 46, NULL, 0, new_ap, false);
    test_stage(tab, &count, 0, 48, NULL, 0, NULL, true);
    expect_wire(tab, count, 0, 12, zero_mac, "new connection invalidates old AP hint");

    count = 0;
    test_stage(tab, &count, 0, 39, deauth, sizeof deauth, NULL, false);
    test_stage(tab, &count, 0, 47, NULL, 0, new_ap, false);
    test_stage(tab, &count, 0, 48, NULL, 0, NULL, true);
    expect_wire(tab, count, 0, 12, zero_mac, "roaming invalidates old AP hint");

    count = 0;
    test_stage(tab, &count, 0, 39, deauth, sizeof deauth, NULL, false);
    test_stage(tab, &count, 0, 48, NULL, 0, NULL, false);
    for (size_t i = 0; i < count; i++)
        ASSERT(tab[i].kind != OPCD_PEVT_AP_DISCONNECT, "local disconnect stays silent");
    count = 0;
    test_stage(tab, &count, 0, 48, NULL, 0, NULL, true);
    expect_wire(tab, count, 0, 12, zero_mac, "local disconnect clears stale AP hint");
}

/* nl80211 ROAM carries no SNR/RSSI, so the stager seeds them from link.json.
 * When that readback is unavailable they must surface as the -128 "no value"
 * marker (FF 80 on the wire), not 0. The host has no link.json (asserted as
 * the premise), so the failed-readback branch is the one reached here. */
static void test_roaming_without_link_readback(void)
{
    int before = failures;
    ASSERT(access(MLAN0_LINK_JSON, F_OK) != 0, "host has no mlan0 link.json (premise)");
    opcd_platform_evt_t tab[NL_COALESCE_MAX] = {0};
    size_t count = 0;
    test_stage(tab, &count, 0, 47, NULL, 0, new_ap, false);
    const opcd_platform_evt_t *evt = NULL;
    for (size_t i = 0; i < count; i++)
        if (tab[i].kind == OPCD_PEVT_ROAMING && tab[i].u.roaming.idx == 0) evt = &tab[i];
    ASSERT(evt != NULL, "ROAM event emitted");
    if (evt) {
        ASSERT(evt->u.roaming.snr == -128 && evt->u.roaming.rssi == -128,
               "ROAM without link readback: snr/rssi are -128");
        opc_ind_roaming_t ind = {
            .current_snr = evt->u.roaming.snr,
            .current_rssi = evt->u.roaming.rssi,
            .ch_number = evt->u.roaming.channel,
        };
        memcpy(ind.connect_ap_mac, evt->u.roaming.mac, 6);
        uint8_t wire[76];
        ASSERT(opc_ind_roaming_pack(wire, sizeof wire, 216, &ind) == 76,
               "roaming wire length");
        ASSERT(wire[64] == 0xFF && wire[65] == 0x80 &&
               wire[66] == 0xFF && wire[67] == 0x80,
               "ROAM without link readback: FF 80 FF 80 on wire");
        ASSERT(memcmp(wire + 68, new_ap, 6) == 0, "roaming AP MAC reaches wire");
    }
    if (failures == before) puts("ROAM without link readback -> FF 80 FF 80 passed");
}

static void test_untrusted_frames(void)
{
    /* A short frame, wrong subtype, STA-origin, zero AP, and multicast AP
     * must not manufacture an AP identity for a later disconnect. */
    for (unsigned variant = 0; variant < 5; variant++) {
        test_reset();
        uint8_t frame[sizeof deauth];
        memcpy(frame, deauth, sizeof frame);
        size_t len = sizeof frame;
        if (variant == 0) len = 25;
        if (variant == 1) frame[0] = 0xb0;
        if (variant == 2) memcpy(frame + 10, deauth + 4, 6);
        if (variant == 3) { memset(frame + 10, 0, 6); memset(frame + 16, 0, 6); }
        if (variant == 4) { frame[10] |= 1; frame[16] |= 1; }
        opcd_platform_evt_t tab[NL_COALESCE_MAX] = {0};
        size_t count = 0;
        test_stage(tab, &count, 0, 39, deauth, sizeof deauth, NULL, false);
        test_stage(tab, &count, 0, 39, frame, len, NULL, false);
        test_stage(tab, &count, 0, 48, NULL, 0, NULL, true);
        expect_wire(tab, count, 0, 12, zero_mac, "untrusted management frame ignored");
    }
}

/* #166: the WlanStatusChange disconnect indication carries the CH that was in
 * effect (band<<8 | channel), remembered per interface; 0xFFFF (spec 4.2.2
 * unset marker) when no association channel is known. The "no history" case
 * must run before any other test connects an interface (state is process-wide). */
static const opcd_platform_evt_t *find_status(const opcd_platform_evt_t *tab,
                                              size_t count, int idx)
{
    const opcd_platform_evt_t *evt = NULL;
    for (size_t i = 0; i < count; i++)
        if (tab[i].kind == OPCD_PEVT_WLAN_STATUS &&
            tab[i].u.wlan_status.idx == idx) evt = &tab[i];
    return evt;
}

static void expect_disc_ch(int idx, uint16_t want, const char *label)
{
    opcd_platform_evt_t tab[NL_COALESCE_MAX] = {0};
    size_t count = 0;
    test_stage(tab, &count, idx, 48, NULL, 0, NULL, false);
    const opcd_platform_evt_t *evt = find_status(tab, count, idx);
    ASSERT(evt != NULL && evt->u.wlan_status.status == OPCD_WLAN_STATUS_UP,
           "disconnect stages WLAN_STATUS(UP)");
    if (!evt) return;
    ASSERT(evt->u.wlan_status.channel == want, label);
}

static void stage_manual(int idx, opcd_nl_kind_t kind, uint32_t freq, uint16_t ch)
{
    opcd_platform_evt_t tab[NL_COALESCE_MAX] = {0};
    size_t count = 0;
    opcd_nl_evt_t evt = {0};
    evt.kind = kind; evt.freq_mhz = freq; evt.channel = ch;
    nl_stage_evt(tab, &count, &evt, idx);
}

/* Codex review of PR #169: a daemon started while WLAN#1 is already associated
 * sees no CONNECT replay, so nxp_init seeds the disconnect CH from the live
 * interface. Runs last: it rewrites the process-wide cache. */
static int fake_iface_freq(int ifindex, uint32_t *freq_mhz, uint16_t *channel)
{
    if (ifindex != 7) return -1;
    *freq_mhz = 5180;
    *channel  = 36;
    return 0;
}

static void test_ch_cache_seed_at_init(void)
{
    nl_note_ch(0, 0);                       /* forget history -> 0xFFFF */
    expect_disc_ch(0, 0xFFFF, "#166 seed: history forgotten -> 0xFFFF");
    g_iface_freq_query = fake_iface_freq;
    nl_seed_ch_cache_idx(0, 0);             /* interface absent: nothing seeded */
    expect_disc_ch(0, 0xFFFF, "#166 seed: missing interface leaves the cache unset");
    nl_seed_ch_cache_idx(0, 9);             /* kernel query fails: nothing seeded */
    expect_disc_ch(0, 0xFFFF, "#166 seed: failed kernel query leaves the cache unset");
    nl_seed_ch_cache_idx(0, 7);             /* associated on 5180 MHz / ch 36 */
    expect_disc_ch(0, 0x0224, "#166 seed: live interface state seeds the disconnect CH at init");
    nl_seed_ch_cache_idx(1, 7);
    expect_disc_ch(1, 0x0224, "#166 seed: each interface is seeded on its own");
    g_iface_freq_query = nxp_get_iface_freq;
}

static void test_disconnect_channel(void)
{
    /* no association history since start -> 0xFFFF on both interfaces */
    expect_disc_ch(0, 0xFFFF, "#166: disconnect without history carries 0xFFFF (mlan0)");
    expect_disc_ch(1, 0xFFFF, "#166: disconnect without history carries 0xFFFF (mlan1)");

    /* CONNECT on 5180 MHz = 5 GHz ch 36 -> 0x0224 */
    opcd_platform_evt_t tab[NL_COALESCE_MAX] = {0};
    size_t count = 0;
    test_stage(tab, &count, 0, 46, NULL, 0, new_ap, false);
    expect_disc_ch(0, 0x0224, "#166: disconnect after connect(CH 36) carries 0x0224");
    expect_disc_ch(1, 0xFFFF, "#166: other interface keeps its own (unknown) channel");

    /* channel change -> new channel (2437 MHz = 2.4 GHz ch 6) */
    stage_manual(0, OPCD_NL_CH_SWITCH, 2437, 6);
    expect_disc_ch(0, 0x0106, "#166: disconnect after channel change carries new CH");

    /* roaming to 5745 MHz = 5 GHz ch 149 */
    stage_manual(0, OPCD_NL_ROAM, 5745, 149);
    expect_disc_ch(0, 0x0295, "#166: disconnect after roaming carries roamed CH");

    /* the remembered channel is per interface */
    stage_manual(1, OPCD_NL_CH_SWITCH, 5180, 36);
    expect_disc_ch(1, 0x0224, "#166: mlan1 channel tracked independently");
    expect_disc_ch(0, 0x0295, "#166: mlan0 channel unaffected by mlan1");

    /* roaming whose channel cannot be resolved must not leave a stale one */
    stage_manual(0, OPCD_NL_ROAM, 0, 0);
    expect_disc_ch(0, 0xFFFF, "#166: unresolvable roam channel falls back to 0xFFFF");
}

int main(void)
{
    test_disconnect_channel();
    test_captured_deauth_and_fast_reconnect();
    test_disassoc_and_explicit_mac();
    test_hint_lifetime_and_interface();
    test_untrusted_frames();
    test_roaming_without_link_readback();
    test_ch_cache_seed_at_init();
    if (failures) {
        fprintf(stderr, "%d AP disconnect checks failed\n", failures);
        return 1;
    }
    puts("AP disconnect frame-to-wire regression passed");
    return 0;
}
