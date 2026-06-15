/*
 * Host-side unit tests for the pure nl80211 netlink event parser
 * (opcd/nl80211_parse.{c,h}). No sockets — synthetic netlink frames are
 * assembled byte-by-byte in host byte order (netlink is host-endian) and fed
 * straight to nl80211_parse_evt(). Mirrors the ASSERT(cond,label) style of
 * test_handler.c: PASS/FAIL per check, failure counter, nonzero exit on any
 * failure.
 *
 * Frame layout under test:
 *   struct nlmsghdr  (16B: u32 len, u16 type, u16 flags, u32 seq, u32 pid)
 *   struct genlmsghdr ( 4B: u8 cmd, u8 version, u16 reserved)
 *   nlattrs          (TLV: u16 nla_len, u16 nla_type, payload, NLA_ALIGN pad)
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../nl80211_parse.h"

static int failures = 0;

#define ASSERT(cond, label) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; }         \
    else         { fprintf(stdout, "PASS %s\n", label); }                     \
} while (0)

/* genl commands (from linux/nl80211.h) used by these frames. */
#define NL80211_CMD_CONNECT          46
#define NL80211_CMD_ROAM             47
#define NL80211_CMD_DISCONNECT       48
#define NL80211_CMD_CH_SWITCH_NOTIFY 88

/* nl80211 attribute ids used by these frames. */
#define NL80211_ATTR_IFINDEX      3
#define NL80211_ATTR_MAC          6
#define NL80211_ATTR_WIPHY_FREQ  38
#define NL80211_ATTR_STATUS_CODE 48
#define NL80211_ATTR_REASON_CODE 54

/* genl CTRL family-resolution constants used by the NEWFAMILY-reply frames. */
#define GENL_ID_CTRL              16
#define CTRL_CMD_NEWFAMILY         1
#define CTRL_ATTR_FAMILY_ID        1
#define CTRL_ATTR_MCAST_GROUPS     7
#define CTRL_ATTR_MCAST_GRP_NAME   1
#define CTRL_ATTR_MCAST_GRP_ID     2

#define TEST_FAMILY_ID 0x1c   /* arbitrary genl family id for nl80211 */

/* ---- minimal byte-buffer frame builder ----------------------------------
 * A growing buffer with helpers that append a netlink/genl header and aligned
 * nlattrs. All multi-byte fields are written in host byte order to match the
 * kernel's netlink convention. */
typedef struct {
    uint8_t  buf[512];
    size_t   len;
} frame_t;

static void f_reset(frame_t *f) { memset(f, 0, sizeof *f); }

static void f_put_u16(frame_t *f, uint16_t v)
{
    memcpy(f->buf + f->len, &v, sizeof v);
    f->len += sizeof v;
}

static void f_put_u32(frame_t *f, uint32_t v)
{
    memcpy(f->buf + f->len, &v, sizeof v);
    f->len += sizeof v;
}

/* nlmsghdr(16) + genlmsghdr(4). nlmsg_len is back-patched in f_finish(). */
static void f_hdr(frame_t *f, uint16_t nlmsg_type, uint8_t genl_cmd)
{
    f_put_u32(f, 0);            /* nlmsg_len  — patched later */
    f_put_u16(f, nlmsg_type);   /* nlmsg_type */
    f_put_u16(f, 0);            /* nlmsg_flags */
    f_put_u32(f, 0);            /* nlmsg_seq  */
    f_put_u32(f, 0);            /* nlmsg_pid  */
    /* genlmsghdr */
    f->buf[f->len++] = genl_cmd; /* cmd */
    f->buf[f->len++] = 1;        /* version */
    f_put_u16(f, 0);             /* reserved */
}

/* Append one nlattr (u16 len incl. 4B header, u16 type, payload), then pad to
 * NLA_ALIGN(4). */
static void f_attr(frame_t *f, uint16_t type, const void *payload, uint16_t plen)
{
    uint16_t nla_len = (uint16_t)(4 + plen);
    f_put_u16(f, nla_len);
    f_put_u16(f, type);
    memcpy(f->buf + f->len, payload, plen);
    f->len += plen;
    while ((f->len & 3) != 0) f->buf[f->len++] = 0;  /* NLA_ALIGN pad */
}

static void f_attr_u32(frame_t *f, uint16_t type, uint32_t v)
{ f_attr(f, type, &v, sizeof v); }

static void f_attr_u16(frame_t *f, uint16_t type, uint16_t v)
{ f_attr(f, type, &v, sizeof v); }

/* ---- nested-attr builders (for the CTRL NEWFAMILY mcast-groups array) ----
 * Build one mcast-group element body — GRP_NAME(string, incl. NUL) +
 * GRP_ID(u16) — into a scratch frame, then splice it as one nested attr.
 * `index` is the 1-based array element type (the kernel uses the index as the
 * nlattr type for each array slot; the parser ignores it). */
static void f_grp(frame_t *parent, uint16_t index, const char *name, uint16_t id)
{
    frame_t body; f_reset(&body);
    /* GRP_NAME: NUL-terminated string payload. */
    uint16_t nlen = (uint16_t)(strlen(name) + 1);
    f_attr(&body, CTRL_ATTR_MCAST_GRP_NAME, name, nlen);
    f_attr_u16(&body, CTRL_ATTR_MCAST_GRP_ID, id);
    /* Splice as one nested attr of the parent (type = array index). */
    f_attr(parent, index, body.buf, (uint16_t)body.len);
}

/* Back-patch nlmsg_len = total frame length. */
static void f_finish(frame_t *f)
{
    uint32_t total = (uint32_t)f->len;
    memcpy(f->buf, &total, sizeof total);
}

int main(void)
{
    static const uint8_t MAC1[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

    /* 1. CONNECT (cmd 46): IFINDEX + WIPHY_FREQ(5200) + STATUS_CODE(0). */
    {
        frame_t f; f_reset(&f);
        f_hdr(&f, TEST_FAMILY_ID, NL80211_CMD_CONNECT);
        f_attr_u32(&f, NL80211_ATTR_IFINDEX, 7);
        f_attr_u32(&f, NL80211_ATTR_WIPHY_FREQ, 5200);
        f_attr_u16(&f, NL80211_ATTR_STATUS_CODE, 0);
        f_finish(&f);

        opcd_nl_evt_t ev;
        int rc = nl80211_parse_evt(f.buf, f.len, TEST_FAMILY_ID, &ev);
        ASSERT(rc == 0, "connect: rc==0");
        ASSERT(ev.kind == OPCD_NL_CONNECT, "connect: kind CONNECT");
        ASSERT(ev.ifindex == 7, "connect: ifindex 7");
        ASSERT(ev.freq_mhz == 5200, "connect: freq 5200");
        ASSERT(ev.channel == 40, "connect: channel 40");
        ASSERT(ev.status_code == 0, "connect: status 0");
    }

    /* 2. DISCONNECT (cmd 48): IFINDEX + MAC + REASON_CODE. */
    {
        frame_t f; f_reset(&f);
        f_hdr(&f, TEST_FAMILY_ID, NL80211_CMD_DISCONNECT);
        f_attr_u32(&f, NL80211_ATTR_IFINDEX, 7);
        f_attr(&f, NL80211_ATTR_MAC, MAC1, sizeof MAC1);
        f_attr_u16(&f, NL80211_ATTR_REASON_CODE, 3);
        f_finish(&f);

        opcd_nl_evt_t ev;
        int rc = nl80211_parse_evt(f.buf, f.len, TEST_FAMILY_ID, &ev);
        ASSERT(rc == 0, "disconnect: rc==0");
        ASSERT(ev.kind == OPCD_NL_DISCONNECT, "disconnect: kind DISCONNECT");
        ASSERT(ev.ifindex == 7, "disconnect: ifindex 7");
        ASSERT(ev.mac_present, "disconnect: mac_present");
        ASSERT(memcmp(ev.mac, MAC1, 6) == 0, "disconnect: mac bytes match");
        ASSERT(ev.reason_code == 3, "disconnect: reason 3");
    }

    /* 3. ROAM (cmd 47): IFINDEX + MAC + WIPHY_FREQ(2437) → channel 6. */
    {
        frame_t f; f_reset(&f);
        f_hdr(&f, TEST_FAMILY_ID, NL80211_CMD_ROAM);
        f_attr_u32(&f, NL80211_ATTR_IFINDEX, 7);
        f_attr(&f, NL80211_ATTR_MAC, MAC1, sizeof MAC1);
        f_attr_u32(&f, NL80211_ATTR_WIPHY_FREQ, 2437);
        f_finish(&f);

        opcd_nl_evt_t ev;
        int rc = nl80211_parse_evt(f.buf, f.len, TEST_FAMILY_ID, &ev);
        ASSERT(rc == 0, "roam: rc==0");
        ASSERT(ev.kind == OPCD_NL_ROAM, "roam: kind ROAM");
        ASSERT(ev.mac_present, "roam: mac_present");
        ASSERT(memcmp(ev.mac, MAC1, 6) == 0, "roam: mac bytes match");
        ASSERT(ev.freq_mhz == 2437, "roam: freq 2437");
        ASSERT(ev.channel == 6, "roam: channel 6");
    }

    /* 4. CH_SWITCH (cmd 88): IFINDEX + WIPHY_FREQ(5180) → channel 36. */
    {
        frame_t f; f_reset(&f);
        f_hdr(&f, TEST_FAMILY_ID, NL80211_CMD_CH_SWITCH_NOTIFY);
        f_attr_u32(&f, NL80211_ATTR_IFINDEX, 7);
        f_attr_u32(&f, NL80211_ATTR_WIPHY_FREQ, 5180);
        f_finish(&f);

        opcd_nl_evt_t ev;
        int rc = nl80211_parse_evt(f.buf, f.len, TEST_FAMILY_ID, &ev);
        ASSERT(rc == 0, "ch_switch: rc==0");
        ASSERT(ev.kind == OPCD_NL_CH_SWITCH, "ch_switch: kind CH_SWITCH");
        ASSERT(ev.freq_mhz == 5180, "ch_switch: freq 5180");
        ASSERT(ev.channel == 36, "ch_switch: channel 36");
    }

    /* 5. Wrong family (nlmsg_type != family_id) → IGNORE / -1. */
    {
        frame_t f; f_reset(&f);
        f_hdr(&f, TEST_FAMILY_ID + 1, NL80211_CMD_CONNECT);
        f_attr_u32(&f, NL80211_ATTR_IFINDEX, 7);
        f_finish(&f);

        opcd_nl_evt_t ev;
        int rc = nl80211_parse_evt(f.buf, f.len, TEST_FAMILY_ID, &ev);
        ASSERT(rc == -1, "wrong_family: rc==-1");
        ASSERT(ev.kind == OPCD_NL_IGNORE, "wrong_family: kind IGNORE");
    }

    /* 6. Unknown genl cmd (99) → IGNORE / -1. */
    {
        frame_t f; f_reset(&f);
        f_hdr(&f, TEST_FAMILY_ID, 99);
        f_attr_u32(&f, NL80211_ATTR_IFINDEX, 7);
        f_finish(&f);

        opcd_nl_evt_t ev;
        int rc = nl80211_parse_evt(f.buf, f.len, TEST_FAMILY_ID, &ev);
        ASSERT(rc == -1, "unknown_cmd: rc==-1");
        ASSERT(ev.kind == OPCD_NL_IGNORE, "unknown_cmd: kind IGNORE");
    }

    /* 7. Truncated message (len < 20: header(16)+genl(4) does not fit) →
     *    -1, no OOB read. Build a valid frame, then hand a short length. */
    {
        frame_t f; f_reset(&f);
        f_hdr(&f, TEST_FAMILY_ID, NL80211_CMD_CONNECT);
        f_attr_u32(&f, NL80211_ATTR_IFINDEX, 7);
        f_finish(&f);

        opcd_nl_evt_t ev;
        int rc = nl80211_parse_evt(f.buf, 19, TEST_FAMILY_ID, &ev);
        ASSERT(rc == -1, "truncated: rc==-1");
        ASSERT(ev.kind == OPCD_NL_IGNORE, "truncated: kind IGNORE");

        /* Also: a zero-length buffer must not read OOB. */
        rc = nl80211_parse_evt(f.buf, 0, TEST_FAMILY_ID, &ev);
        ASSERT(rc == -1, "truncated: zero-len rc==-1");
    }

    /* 8. Attr with nla_len overflowing the buffer: the cmd is still
     *    recognized (truncated attr area is not fatal to cmd dispatch), the
     *    overflowing attr is NOT read (no OOB). We expect CONNECT dispatched
     *    with ifindex from the valid leading attr, and the bogus trailing
     *    freq attr ignored (channel stays 0). */
    {
        frame_t f; f_reset(&f);
        f_hdr(&f, TEST_FAMILY_ID, NL80211_CMD_CONNECT);
        f_attr_u32(&f, NL80211_ATTR_IFINDEX, 7);
        /* Manually append a malformed attr: nla_len claims 200 bytes but only
         * 4 header bytes remain. */
        f_put_u16(&f, 200);                      /* nla_len (overflows)  */
        f_put_u16(&f, NL80211_ATTR_WIPHY_FREQ);  /* nla_type             */
        f_finish(&f);

        opcd_nl_evt_t ev;
        int rc = nl80211_parse_evt(f.buf, f.len, TEST_FAMILY_ID, &ev);
        ASSERT(rc == 0, "overflow_attr: cmd still dispatched (rc==0)");
        ASSERT(ev.kind == OPCD_NL_CONNECT, "overflow_attr: kind CONNECT");
        ASSERT(ev.ifindex == 7, "overflow_attr: leading ifindex preserved");
        ASSERT(ev.freq_mhz == 0, "overflow_attr: bogus freq not read");
        ASSERT(ev.channel == 0, "overflow_attr: channel 0 (no freq)");
    }

    /* 9. CTRL NEWFAMILY reply: FAMILY_ID(0x1c) + MCAST_GROUPS with two groups
     *    ("scan", then "mlme") → both family id and the mlme group id extracted. */
    {
        frame_t f; f_reset(&f);
        f_hdr(&f, GENL_ID_CTRL, CTRL_CMD_NEWFAMILY);
        f_attr_u16(&f, CTRL_ATTR_FAMILY_ID, 0x1c);

        /* MCAST_GROUPS is a nested array of group elements. Build the array
         * body in a scratch frame, then splice it as one nested attr. */
        frame_t groups; f_reset(&groups);
        f_grp(&groups, 1, "scan", 4);
        f_grp(&groups, 2, "mlme", 7);
        f_attr(&f, CTRL_ATTR_MCAST_GROUPS, groups.buf, (uint16_t)groups.len);
        f_finish(&f);

        uint16_t fam = 0, grp = 0;
        int rc = nl80211_parse_ctrl_family(f.buf, f.len, &fam, &grp);
        ASSERT(rc == 0, "ctrl_family: rc==0");
        ASSERT(fam == 0x1c, "ctrl_family: family id 0x1c");
        ASSERT(grp == 7, "ctrl_family: mlme group id 7");
    }

    /* 10. CTRL NEWFAMILY reply missing the "mlme" group → -1 (family id alone
     *     is not enough). */
    {
        frame_t f; f_reset(&f);
        f_hdr(&f, GENL_ID_CTRL, CTRL_CMD_NEWFAMILY);
        f_attr_u16(&f, CTRL_ATTR_FAMILY_ID, 0x1c);

        frame_t groups; f_reset(&groups);
        f_grp(&groups, 1, "scan", 4);
        f_grp(&groups, 2, "config", 5);
        f_attr(&f, CTRL_ATTR_MCAST_GROUPS, groups.buf, (uint16_t)groups.len);
        f_finish(&f);

        uint16_t fam = 0xffff, grp = 0xffff;
        int rc = nl80211_parse_ctrl_family(f.buf, f.len, &fam, &grp);
        ASSERT(rc == -1, "ctrl_family_no_mlme: rc==-1");
    }

    /* 11. Truncated CTRL reply: a valid frame handed a short length must not
     *     read OOB and must return -1. Also test header-only truncation. */
    {
        frame_t f; f_reset(&f);
        f_hdr(&f, GENL_ID_CTRL, CTRL_CMD_NEWFAMILY);
        f_attr_u16(&f, CTRL_ATTR_FAMILY_ID, 0x1c);
        frame_t groups; f_reset(&groups);
        f_grp(&groups, 1, "mlme", 7);
        f_attr(&f, CTRL_ATTR_MCAST_GROUPS, groups.buf, (uint16_t)groups.len);
        f_finish(&f);

        uint16_t fam = 0, grp = 0;
        /* Cut mid-attribute-area: header(20) survives but attrs are sheared,
         * so neither family id nor mlme can be fully read → -1, no OOB. */
        int rc = nl80211_parse_ctrl_family(f.buf, 22, &fam, &grp);
        ASSERT(rc == -1, "ctrl_family_truncated: rc==-1");

        /* Below header+genl minimum → -1. */
        rc = nl80211_parse_ctrl_family(f.buf, 19, &fam, &grp);
        ASSERT(rc == -1, "ctrl_family_truncated: short-header rc==-1");

        /* Zero length → -1, no OOB. */
        rc = nl80211_parse_ctrl_family(f.buf, 0, &fam, &grp);
        ASSERT(rc == -1, "ctrl_family_truncated: zero-len rc==-1");
    }

    if (failures == 0) {
        fprintf(stdout, "all nl80211_parse tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
