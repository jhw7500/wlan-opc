#ifndef WLAN_OPC_IDS_H
#define WLAN_OPC_IDS_H

/*
 * Request/Indication identifier table (spec Rev1.00, tables 3-1 and 3-4).
 *
 * Request IDs are placed in the 16-bit req_indication_id field of an
 * OPC_CMD_REQUEST or OPC_CMD_ACK frame. Indication IDs go in the same field
 * for OPC_CMD_INDICATION frames AND double as the bitmask values used by
 * Set Indication Config (0x1005) to select which notifications to enable.
 */

/* ---- Request / Query IDs (Login/Logout, Get*, Set*, Reset) ---- */
#define OPC_REQ_LOGIN                   0xF001
#define OPC_REQ_LOGOUT                  0xF002
#define OPC_REQ_GET_BASIC_INFO          0x0001
#define OPC_REQ_GET_DEVICE_INFO         0x0002
#define OPC_REQ_SET_PASSWORD            0x1001
#define OPC_REQ_SET_IP_CONFIG_LIST      0x1002
#define OPC_REQ_CHANGE_IP_ADDRESS       0x1003
#define OPC_REQ_SET_RADIO_CONFIG        0x1004
#define OPC_REQ_SET_INDICATION_CONFIG   0x1005
#define OPC_REQ_RESET                   0x2001

/* ---- Indication IDs (also Indication Info bitmask values) ---- */
#define OPC_IND_INIT_COMPLETE           0x0001
#define OPC_IND_WLAN_STATUS_CHANGE      0x0002
#define OPC_IND_ROAMING                 0x0004
#define OPC_IND_AP_DISCONNECT           0x0008
#define OPC_IND_FAULT_DETECT            0x0010
#define OPC_IND_RESET_NOTICE            0x0020
#define OPC_IND_KEEP_ALIVE              0x0080

/* ---- Indication Info bitmask aliases (clearer at call sites) ---- */
#define OPC_IND_BIT_INIT_COMPLETE       0x01
#define OPC_IND_BIT_WLAN_STATUS_CHANGE  0x02
#define OPC_IND_BIT_ROAMING             0x04
#define OPC_IND_BIT_AP_DISCONNECT       0x08
#define OPC_IND_BIT_FAULT_DETECT        0x10
#define OPC_IND_BIT_RESET_NOTICE        0x20
#define OPC_IND_BIT_KEEP_ALIVE          0x80

/* ---- WLAN Mode (spec table 3-2) ---- */
#define OPC_WLAN_MODE_11B               5
#define OPC_WLAN_MODE_11A               4
#define OPC_WLAN_MODE_11G               6
#define OPC_WLAN_MODE_11N               7
#define OPC_WLAN_MODE_11AC              9
#define OPC_WLAN_MODE_11AX              11

/* ---- Frequency-bandwidth (spec table 3-3) ---- */
#define OPC_BANDWIDTH_20                0
#define OPC_BANDWIDTH_40                1
#define OPC_BANDWIDTH_80                2
#define OPC_BANDWIDTH_160               3
#define OPC_BANDWIDTH_80_80             4
#define OPC_BANDWIDTH_320               12

/* ---- Station type ---- */
#define OPC_STATION_SINGLE              0x0001
#define OPC_STATION_DUAL                0x0002

/* ---- Frequency band (used in Priority CH / Indication CH high byte) ---- */
#define OPC_BAND_2_4GHZ                 0x01
#define OPC_BAND_5GHZ                   0x02
#define OPC_BAND_6GHZ                   0x06

/* ---- Device Status (in GetBasicInfo / GetDeviceInfo / InitComplete) ---- */
#define OPC_DEVICE_BOOTING              0x00000000
#define OPC_DEVICE_READY                0x00000001
#define OPC_DEVICE_LOGGED_IN            0x00000002

/* ---- InitComplete Status field (Indication 0x0001) ---- */
#define OPC_INIT_STATE_BOOT             0x00000000
#define OPC_INIT_STATE_READY            0x00000001
#define OPC_INIT_STATE_RADIO_UP         0x00000002
#define OPC_INIT_STATE_LOGGED_IN        0x00000003
#define OPC_INIT_STATE_LOGGED_OUT       0x00000004

/* Command-specific error causes (0x0010+). The OPC spec fixes these wire
 * values, and several distinct meanings necessarily share 0x0010 — the named
 * aliases below disambiguate the call sites while preserving the exact byte on
 * the wire. A receiver must use the command context to interpret a 0x0010:
 * the same value means indication-violation, password-mismatch, slot-range,
 * or invalid-station-type depending on which command produced it. This
 * overload is a spec limitation, not something opcd can fix unilaterally. */
#define OPC_ERR_INDICATION_SETTING_VIOLATION  0x0010  /* GetDeviceInfo while indication enabled */
#define OPC_ERR_PASSWORD_MISMATCH             0x0010  /* Login/SetPassword: wrong or empty password */
#define OPC_ERR_SLOT_RANGE                    0x0010  /* SetIpConfigList/ChangeIpAddress: slot # out of range */
#define OPC_ERR_STATION_TYPE                  0x0010  /* SetRadioConfig: invalid station_type */
#define OPC_ERR_SLOT_EMPTY                    0x0011  /* ChangeIpAddress: target slot empty */
#define OPC_ERR_IP_CHANGE_CONFLICT            0x0012  /* ChangeIpAddress during in-progress list update */
#define OPC_ERR_RADIO_MODE                    0x0013  /* SetRadioConfig: invalid WLAN mode */
#define OPC_ERR_RADIO_BW                      0x0014  /* SetRadioConfig: invalid WLAN bandwidth */
/* 0x0011–0x0014 are likewise spec-overloaded: each name below intentionally
 * aliases the same wire value with a different, command-scoped meaning.
 *   0x0011 = SLOT_EMPTY (ChangeIp) | RADIO_FREQ (SetRadio) | IPCFG_IP (SetIpConfigList)
 *   0x0012 = IP_CHANGE_CONFLICT (ChangeIp) | IND_RECIPIENT_IP (SetIndication)
 *            | PW_NUL (Login/SetPassword) | IPCFG_NETMASK (SetIpConfigList)
 *            | RADIO_CH (SetRadio)
 *   0x0013 = RADIO_MODE (SetRadio) | IND_OTHER_IP (SetIndication) | IPCFG_GW (SetIpConfigList)
 *   0x0014 = RADIO_BW (SetRadio) | NEW_PW_NUL (SetPassword) | IPCFG_NTP (SetIpConfigList)
 * Each handler must use only its own command's names, and a single switch must
 * never mix two same-valued names — that is a duplicate-case compile error.
 * vhlctl's value→label map therefore uses literal cases with combined labels. */
#define OPC_ERR_RADIO_FREQ                    0x0011  /* SetRadioConfig: unsupported frequency *value*
                                                       * (D8 input validation). A platform apply failure
                                                       * no longer maps here — it is a runtime fault, not
                                                       * a bad input, so it carries its own code
                                                       * OPC_ERR_RADIO_APPLY (D9). */
#define OPC_ERR_IND_RECIPIENT_IP              0x0012  /* SetIndicationConfig: recipient IP invalid
                                                       * (non-unicast) — spec "IP 주소 이상" (D10) */
#define OPC_ERR_IND_OTHER_IP                  0x0013  /* SetIndicationConfig: issued from a non-login IP
                                                       * (A14; overlap with 0x0002 — vendor inquiry) */
#define OPC_ERR_LIST_SEQUENCE                 0x0018  /* SetIpConfigList: CONTINUE/END without prior
                                                       * START (A17).
                                                       * FIXME: wire value unconfirmed — 0x0018 is the
                                                       * vendor-answer proposal; update when the formal
                                                       * confirmation arrives. */
#define OPC_ERR_RADIO_APPLY                   0x0050  /* SetRadioConfig: the platform refused / failed to
                                                       * apply an otherwise-valid request (runtime fault,
                                                       * NOT an input error — the frequency/CH/mode/bw
                                                       * already passed validation). §3.3.8 defines no
                                                       * apply-failure cause; kept distinct from 0x0011 so
                                                       * a VHL operator is never told to "fix the
                                                       * frequency" for a runtime fault. On this NG the
                                                       * handler best-effort re-applies the last-good
                                                       * config, so a partial apply leaves no net change
                                                       * (D9, re-decided 2026-06-16).
                                                       * FIXME(#35): wire value pending 발주처 confirmation
                                                       * — 0x0050 is the proposal (pre-PR#34 firmware used
                                                       * it); update when formally confirmed. */
#define OPC_ERR_PW_NUL                        0x0012  /* Login §3.3.1 / SetPassword(old) §3.3.5:
                                                       * password field not NUL-terminated */
#define OPC_ERR_NEW_PW_NUL                    0x0014  /* SetPassword: new password not NUL-terminated */
#define OPC_ERR_IPCFG_IP                      0x0011  /* SetIpConfigList: impossible IP (0.0.0.0/bcast/mcast) */
#define OPC_ERR_IPCFG_NETMASK                 0x0012  /* SetIpConfigList: not a valid netmask */
#define OPC_ERR_IPCFG_GW                      0x0013  /* SetIpConfigList: gateway outside the entry's subnet */
#define OPC_ERR_IPCFG_NTP                     0x0014  /* SetIpConfigList: impossible NTP server IP */
#define OPC_ERR_IPCFG_ESSID_CHAR              0x0015  /* SetIpConfigList: invalid ESSID characters.
                                                       * UNUSED — semantics undefined in the spec;
                                                       * reserved pending the A5 inquiry (#35) */
#define OPC_ERR_IPCFG_ESSID_NUL               0x0016  /* SetIpConfigList: ESSID not NUL-terminated */
#define OPC_ERR_IPCFG_LIST_SIZE               0x0017  /* SetIpConfigList: Length is not 56 + 64*n */
#define OPC_ERR_RADIO_CH                      0x0012  /* SetRadioConfig: unsupported CH/band (incl. 6 GHz — A21) */
#define OPC_ERR_IND_BITS                      0x0010  /* SetIndicationConfig: unassigned info bit set */

/* Indication info bits (§3.4): 0x40 is the only unassigned/reserved bit —
 * the seven assigned OPC_IND_BIT_* above OR to 0xBF. */
#define OPC_IND_BITS_RESERVED                 0x40

/* Reset cause (Reset ack / ResetNotice indication). */
#define OPC_RESET_CAUSE_USER                  0x00000001  /* operator-issued Reset */

#endif /* WLAN_OPC_IDS_H */
