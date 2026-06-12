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
/* 0x0011–0x0013 are likewise spec-overloaded: each name below intentionally
 * aliases a value already defined above, scoped to a different command.
 *   0x0011 = SLOT_EMPTY (ChangeIpAddress)        | RADIO_FREQ (SetRadioConfig)
 *   0x0012 = IP_CHANGE_CONFLICT (ChangeIpAddress)| IND_RECIPIENT_IP (SetIndicationConfig)
 *   0x0013 = RADIO_MODE (SetRadioConfig)         | IND_OTHER_IP (SetIndicationConfig)
 * Each handler must use only its own command's names, and a single switch must
 * never mix two same-valued names — that is a duplicate-case compile error.
 * vhlctl's value→label map therefore uses literal cases with combined labels. */
#define OPC_ERR_RADIO_FREQ                    0x0011  /* SetRadioConfig: frequency NG — also reports a
                                                       * platform apply refusal: the spec defines no
                                                       * apply-failure code and the apply step is the
                                                       * frequency change (D9) */
#define OPC_ERR_IND_RECIPIENT_IP              0x0012  /* SetIndicationConfig: recipient IP invalid
                                                       * (non-unicast) — spec "IP 주소 이상" (D10) */
#define OPC_ERR_IND_OTHER_IP                  0x0013  /* SetIndicationConfig: issued from a non-login IP
                                                       * (A14; overlap with 0x0002 — vendor inquiry) */
#define OPC_ERR_LIST_SEQUENCE                 0x0018  /* SetIpConfigList: CONTINUE/END without prior
                                                       * START (A17; value pending vendor confirmation) */

/* Reset cause (Reset ack / ResetNotice indication). */
#define OPC_RESET_CAUSE_USER                  0x00000001  /* operator-issued Reset */

#endif /* WLAN_OPC_IDS_H */
