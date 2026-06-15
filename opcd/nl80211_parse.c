/*
 * Pure nl80211 netlink event parser — see nl80211_parse.h.
 *
 * Raw byte parsing, no libnl. Netlink is host byte order; every multi-byte
 * read is a memcpy into an aligned local (never a misaligned pointer cast —
 * ARM traps unaligned access) and is bounds-checked against the message
 * length before it happens.
 */

#include "nl80211_parse.h"

#include <string.h>

/* ---- on-wire constants (subset of linux/nl80211.h + linux/netlink.h) -----
 * Hardcoded so the parser builds on hosts whose kernel headers predate these
 * commands. Values are ABI-stable. */

/* struct nlmsghdr / genlmsghdr sizes. */
#define NL_NLMSGHDR_LEN   16   /* u32 len, u16 type, u16 flags, u32 seq, u32 pid */
#define NL_GENLMSGHDR_LEN  4   /* u8 cmd, u8 version, u16 reserved               */
#define NL_NLATTR_HDR_LEN  4   /* u16 nla_len, u16 nla_type                      */

/* genl commands. */
#define NL80211_CMD_CONNECT          46
#define NL80211_CMD_ROAM             47
#define NL80211_CMD_DISCONNECT       48
#define NL80211_CMD_CH_SWITCH_NOTIFY 88

/* nl80211 attribute ids. */
#define NL80211_ATTR_IFINDEX      3
#define NL80211_ATTR_MAC          6
#define NL80211_ATTR_WIPHY_FREQ  38
#define NL80211_ATTR_STATUS_CODE 72   /* 72 in nl80211 UAPI; 48 is REG_INITIATOR */
#define NL80211_ATTR_REASON_CODE 54
#define NL80211_ATTR_DISCONNECTED_BY_AP 71  /* NLA_FLAG: zero-length, presence=true */

/* genl CTRL (GENL_ID_CTRL=16) family-resolution constants. ABI-stable —
 * hardcoded for the same header-portability reason as the nl80211 ids. */
#define CTRL_ATTR_FAMILY_ID       1
#define CTRL_ATTR_MCAST_GROUPS    7
#define CTRL_ATTR_MCAST_GRP_NAME  1
#define CTRL_ATTR_MCAST_GRP_ID    2

/* NLA_ALIGN: round up to a 4-byte boundary. */
#define NLA_ALIGN(n) (((n) + 3) & ~3u)

/* nla_type's high bits are flags — NLA_F_NESTED(0x8000) /
 * NLA_F_NET_BYTEORDER(0x4000). The real attribute type is the low 14 bits. */
#define NLA_TYPE_MASK 0x3fffu

/* Aligned host-order reads. The source pointer may be unaligned, so memcpy. */
static uint16_t rd_u16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static uint32_t rd_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

/* freq (MHz) → channel number. 0 for "no association" / unknown band. */
static uint16_t freq_to_channel(uint32_t freq_mhz)
{
    if (freq_mhz >= 2412 && freq_mhz <= 2472)
        return (uint16_t)((freq_mhz - 2407) / 5);   /* 2.4 GHz ch 1..13 */
    if (freq_mhz == 2484)
        return 14;                                  /* 2.4 GHz ch 14    */
    if (freq_mhz >= 5000 && freq_mhz <= 5895)
        return (uint16_t)((freq_mhz - 5000) / 5);   /* 5 GHz            */
    if (freq_mhz >= 5955 && freq_mhz <= 7115)
        return (uint16_t)((freq_mhz - 5950) / 5);   /* 6 GHz            */
    return 0;
}

/* genl cmd → kind. OPCD_NL_IGNORE for anything we do not surface. */
static opcd_nl_kind_t cmd_to_kind(uint8_t cmd)
{
    switch (cmd) {
    case NL80211_CMD_CONNECT:          return OPCD_NL_CONNECT;
    case NL80211_CMD_ROAM:             return OPCD_NL_ROAM;
    case NL80211_CMD_DISCONNECT:       return OPCD_NL_DISCONNECT;
    case NL80211_CMD_CH_SWITCH_NOTIFY: return OPCD_NL_CH_SWITCH;
    default:                           return OPCD_NL_IGNORE;
    }
}

int nl80211_parse_evt(const uint8_t *msg, size_t len, int family_id,
                      opcd_nl_evt_t *out)
{
    /* Fail-closed: caller can always read out->kind. */
    if (out != NULL)
        memset(out, 0, sizeof *out);

    if (msg == NULL || out == NULL)
        return -1;

    /* Need at least nlmsghdr + genlmsghdr. Also handle the (illegal but
     * defensive) family_id that does not fit in nlmsg_type's u16. */
    if (len < (size_t)(NL_NLMSGHDR_LEN + NL_GENLMSGHDR_LEN))
        return -1;
    if (family_id < 0 || family_id > 0xffff)
        return -1;

    /* nlmsghdr: only nlmsg_type (offset 4) matters for dispatch. */
    uint16_t nlmsg_type = rd_u16(msg + 4);
    if (nlmsg_type != (uint16_t)family_id)
        return -1;

    /* genlmsghdr: cmd at offset NL_NLMSGHDR_LEN. */
    uint8_t cmd = msg[NL_NLMSGHDR_LEN];
    opcd_nl_kind_t kind = cmd_to_kind(cmd);
    if (kind == OPCD_NL_IGNORE)
        return -1;

    out->kind = kind;

    /* Attribute area begins right after the genl header. A truncated or
     * malformed attr area is NOT fatal to the cmd dispatch — we stop parsing
     * attrs and return the event we already recognized. */
    size_t off = (size_t)(NL_NLMSGHDR_LEN + NL_GENLMSGHDR_LEN);
    while (off + NL_NLATTR_HDR_LEN <= len) {
        uint16_t nla_len  = rd_u16(msg + off);
        uint16_t nla_type = rd_u16(msg + off + 2) & NLA_TYPE_MASK;

        /* Defensive bounds: a header-undersized or buffer-overflowing
         * nla_len means the attr stream is malformed — stop here. */
        if (nla_len < NL_NLATTR_HDR_LEN)
            break;
        if ((size_t)nla_len > len - off)
            break;

        const uint8_t *pl = msg + off + NL_NLATTR_HDR_LEN;
        uint16_t pl_len = (uint16_t)(nla_len - NL_NLATTR_HDR_LEN);

        switch (nla_type) {
        case NL80211_ATTR_IFINDEX:
            if (pl_len >= 4)
                out->ifindex = (int)rd_u32(pl);
            break;
        case NL80211_ATTR_WIPHY_FREQ:
            if (pl_len >= 4) {
                out->freq_mhz = rd_u32(pl);
                out->channel  = freq_to_channel(out->freq_mhz);
            }
            break;
        case NL80211_ATTR_STATUS_CODE:
            if (pl_len >= 2)
                out->status_code = rd_u16(pl);
            break;
        case NL80211_ATTR_REASON_CODE:
            if (pl_len >= 2)
                out->reason_code = rd_u16(pl);
            break;
        case NL80211_ATTR_MAC:
            if (pl_len >= 6) {
                memcpy(out->mac, pl, 6);
                out->mac_present = true;
            }
            break;
        case NL80211_ATTR_DISCONNECTED_BY_AP:
            /* NLA_FLAG: a zero-length attr whose mere presence means true. */
            out->by_ap = true;
            break;
        default:
            break;
        }

        /* Advance by the aligned attr length. NLA_ALIGN can exceed the
         * remaining bytes for the final attr — guard against wraparound. */
        size_t advance = NLA_ALIGN((size_t)nla_len);
        if (advance == 0 || off + advance <= off)  /* paranoia: no progress */
            break;
        off += advance;
    }

    return 0;
}

/* Scan one CTRL_ATTR_MCAST_GROUPS array element (itself a nested attr stream
 * holding CTRL_ATTR_MCAST_GRP_NAME + CTRL_ATTR_MCAST_GRP_ID). If its name is
 * "mlme", write its id to *out_id and return true. base/blen bound the
 * element's payload (already validated by the caller). */
static bool ctrl_grp_is_mlme(const uint8_t *base, size_t blen, uint16_t *out_id)
{
    bool     have_id   = false;
    bool     is_mlme   = false;
    uint16_t grp_id    = 0;

    size_t off = 0;
    while (off + NL_NLATTR_HDR_LEN <= blen) {
        uint16_t nla_len  = rd_u16(base + off);
        uint16_t nla_type = rd_u16(base + off + 2) & NLA_TYPE_MASK;

        if (nla_len < NL_NLATTR_HDR_LEN)
            break;
        if ((size_t)nla_len > blen - off)
            break;

        const uint8_t *pl = base + off + NL_NLATTR_HDR_LEN;
        uint16_t pl_len = (uint16_t)(nla_len - NL_NLATTR_HDR_LEN);

        switch (nla_type) {
        case CTRL_ATTR_MCAST_GRP_NAME:
            /* NUL-terminated string; compare bounded against "mlme". */
            if (pl_len >= 5 && memcmp(pl, "mlme", 5) == 0) {
                is_mlme = true;
            }
            break;
        case CTRL_ATTR_MCAST_GRP_ID:
            /* Kernel emits this via nla_put_u32; read u32 then truncate. */
            if (pl_len >= 4) {
                uint32_t v32 = rd_u32(pl);
                grp_id = (uint16_t)v32;
                have_id = true;
            }
            break;
        default:
            break;
        }

        size_t advance = NLA_ALIGN((size_t)nla_len);
        if (advance == 0 || off + advance <= off)
            break;
        off += advance;
    }

    if (is_mlme && have_id) {
        *out_id = grp_id;
        return true;
    }
    return false;
}

int nl80211_parse_ctrl_family(const uint8_t *msg, size_t len,
                              uint16_t *family_id, uint16_t *mlme_grp_id)
{
    if (msg == NULL || family_id == NULL || mlme_grp_id == NULL)
        return -1;

    /* Need at least nlmsghdr + genlmsghdr. The CTRL reply's nlmsg_type is
     * GENL_ID_CTRL and its cmd is CTRL_CMD_NEWFAMILY; we do not re-validate
     * those here (the caller issued the GETFAMILY request) — we only need the
     * attribute area, which starts right after the genl header. */
    if (len < (size_t)(NL_NLMSGHDR_LEN + NL_GENLMSGHDR_LEN))
        return -1;

    bool     have_family = false;
    bool     have_mlme   = false;
    uint16_t fam_id      = 0;
    uint16_t grp_id      = 0;

    size_t off = (size_t)(NL_NLMSGHDR_LEN + NL_GENLMSGHDR_LEN);
    while (off + NL_NLATTR_HDR_LEN <= len) {
        uint16_t nla_len  = rd_u16(msg + off);
        uint16_t nla_type = rd_u16(msg + off + 2) & NLA_TYPE_MASK;

        if (nla_len < NL_NLATTR_HDR_LEN)
            break;
        if ((size_t)nla_len > len - off)
            break;

        const uint8_t *pl = msg + off + NL_NLATTR_HDR_LEN;
        uint16_t pl_len = (uint16_t)(nla_len - NL_NLATTR_HDR_LEN);

        switch (nla_type) {
        case CTRL_ATTR_FAMILY_ID:
            if (pl_len >= 2) {
                fam_id = rd_u16(pl);
                have_family = true;
            }
            break;
        case CTRL_ATTR_MCAST_GROUPS:
            /* Nested array: each element is itself a nested attr whose own
             * nla_type is the (1-based) array index and whose payload is the
             * per-group attr stream. Walk elements until we find "mlme". */
            if (!have_mlme) {
                size_t goff = 0;
                while (goff + NL_NLATTR_HDR_LEN <= pl_len) {
                    uint16_t e_len = rd_u16(pl + goff);
                    /* e_type (the array index) is unused. */

                    if (e_len < NL_NLATTR_HDR_LEN)
                        break;
                    if ((size_t)e_len > (size_t)pl_len - goff)
                        break;

                    const uint8_t *epl = pl + goff + NL_NLATTR_HDR_LEN;
                    size_t epl_len = (size_t)(e_len - NL_NLATTR_HDR_LEN);
                    if (ctrl_grp_is_mlme(epl, epl_len, &grp_id)) {
                        have_mlme = true;
                        break;
                    }

                    size_t advance = NLA_ALIGN((size_t)e_len);
                    if (advance == 0 || goff + advance <= goff)
                        break;
                    goff += advance;
                }
            }
            break;
        default:
            break;
        }

        size_t advance = NLA_ALIGN((size_t)nla_len);
        if (advance == 0 || off + advance <= off)
            break;
        off += advance;
    }

    if (have_family && have_mlme) {
        *family_id   = fam_id;
        *mlme_grp_id = grp_id;
        return 0;
    }
    return -1;
}
