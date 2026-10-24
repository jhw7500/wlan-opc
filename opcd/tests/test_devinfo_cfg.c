/* Host unit test for the GetDeviceInfo Mode/Band Width fallback before a
 * SetRadioConfig is received (opcd/devinfo_cfg.{c,h}, #193): the device's own
 * radio configuration in wifi_init_conf.json, read the way wifi_init.sh
 * applies it at boot. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../devinfo_cfg.h"
#include "../../protocol/ids.h"

static int failures = 0;
#define ASSERT(cond, label) do {                          \
    if (cond) { printf("PASS %s\n", (label)); }           \
    else      { printf("FAIL %s\n", (label)); failures++; }\
} while (0)

static uint8_t g_mode, g_bw;

static void scan(const char *json, const char *iface)
{
    g_mode = 0x55;
    g_bw = 0x55;
    opcd_devinfo_cfg_from_json(json, iface, &g_mode, &g_bw);
}

/* Shaped like the shipped file: _comment arrays whose strings name the keys,
 * and other objects under the interface that carry their own "mode". */
static const char SHIPPED[] =
    "{\n"
    "  \"global\": {\n"
    "    \"_comment\": [\"STANDARD: (string) n|ac|ax\", \"\\\"STANDARD\\\": \\\"n\\\"\"],\n"
    "    \"STANDARD\": \"\"\n"
    "  },\n"
    "  \"mlan0\": {\n"
    "    \"_comment\": [\"\\\"radio\\\": {\\\"mode\\\": \\\"b\\\", \\\"bw\\\": \\\"20\\\"}\"],\n"
    "    \"STANDARD\": \"ax\",\n"
    "    \"roam\": { \"mode\": \"n\", \"radio\": { \"mode\": \"g\" } },\n"
    "    \"fw\": [ { \"mode\": 1 } ]\n"
    "  },\n"
    "  \"mlan1\": {\n"
    "    \"STANDARD\": \"ac\"\n"
    "  }\n"
    "}\n";

int main(void)
{
    /* ---- STANDARD as the shipped file sets it ---- */
    scan(SHIPPED, "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11AX, "shipped mlan0 STANDARD ax -> 11AX");
    ASSERT(g_bw == OPC_BANDWIDTH_UNSET, "shipped mlan0 has no bw setting -> unset");
    scan(SHIPPED, "mlan1");
    ASSERT(g_mode == OPC_WLAN_MODE_11AC && g_bw == OPC_BANDWIDTH_UNSET, "shipped mlan1 STANDARD ac -> 11AC/unset");

    /* ---- .radio.mode/.radio.bw win over STANDARD ---- */
    scan("{\"mlan0\":{\"STANDARD\":\"ax\",\"radio\":{\"mode\":\"n\",\"bw\":\"40\"}}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11N && g_bw == OPC_BANDWIDTH_40, "radio n/40 wins");
    scan("{\"mlan0\":{\"radio\":{\"mode\":\"b\",\"bw\":20}}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11B && g_bw == OPC_BANDWIDTH_20, "radio b, bw as a JSON number");
    scan("{\"mlan0\":{\"radio\":{\"mode\":\"g\"}}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11G && g_bw == OPC_BANDWIDTH_UNSET, "radio g without bw");
    scan("{\"mlan0\":{\"radio\":{\"mode\":\"a\",\"bw\":\"80\"}}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11A && g_bw == OPC_BANDWIDTH_80, "radio a/80");
    scan("{\"mlan1\":{\"radio\":{\"mode\":\"ac\",\"bw\":\"80\"}}}", "mlan1");
    ASSERT(g_mode == OPC_WLAN_MODE_11AC && g_bw == OPC_BANDWIDTH_80, "mlan1 radio ac/80");
    scan("{\"mlan0\":{\"radio\":{\"mode\":\"ax\"}}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11AX, "mlan0 radio ax");

    /* ---- radio values wifi_init.sh skips leave the STANDARD limit ---- */
    scan("{\"mlan0\":{\"STANDARD\":\"ac\",\"radio\":{\"mode\":\"zz\",\"bw\":\"auto\"}}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11AC && g_bw == OPC_BANDWIDTH_UNSET, "invalid radio.mode -> STANDARD, bw auto -> unset");
    scan("{\"mlan1\":{\"STANDARD\":\"ac\",\"radio\":{\"mode\":\"ax\",\"bw\":\"160\"}}}", "mlan1");
    ASSERT(g_mode == OPC_WLAN_MODE_11AC && g_bw == OPC_BANDWIDTH_UNSET, "mlan1 radio ax skipped -> STANDARD; bw 160 -> unset");
    scan("{\"mlan0\":{\"STANDARD\":\"n\",\"radio\":{\"mode\":\"AX\"}}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11N, "radio.mode is case-sensitive like wifi_init.sh");

    /* ---- STANDARD: global fallback, aliases, case, interface maximum ---- */
    scan("{\"global\":{\"STANDARD\":\"5\"},\"mlan0\":{\"STANDARD\":\"\"}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11AC, "empty interface STANDARD -> global 5 -> 11AC");
    scan("{\"global\":{\"STANDARD\":\"n\"},\"mlan0\":{\"STANDARD\":null}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11N, "null interface STANDARD -> global");
    scan("{\"global\":{\"STANDARD\":\"n\"},\"mlan0\":{}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11N, "absent interface STANDARD -> global");
    scan("{\"global\":{\"STANDARD\":\"6\"},\"mlan1\":{}}", "mlan1");
    ASSERT(g_mode == OPC_WLAN_MODE_11AC, "mlan1 ax/6 -> its maximum 11AC");
    scan("{\"mlan0\":{\"STANDARD\":\"AX\"}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11AX, "STANDARD is case-insensitive");
    scan("{\"mlan0\":{\"STANDARD\":\"4\"}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11N, "STANDARD alias 4 -> 11N");
    scan("{\"global\":{\"STANDARD\":\"n\"},\"mlan0\":{\"STANDARD\":\"wifi7\"}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_UNSET, "invalid interface STANDARD -> unset, no global fallback");
    scan("{\"global\":{\"STANDARD\":\"\"},\"mlan0\":{\"STANDARD\":\"\"}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_UNSET && g_bw == OPC_BANDWIDTH_UNSET, "no STANDARD anywhere -> unset");

    /* ---- only direct members count ---- */
    scan("{\"mlan0\":{\"roam\":{\"STANDARD\":\"n\",\"radio\":{\"mode\":\"b\"}}},\"global\":{\"STANDARD\":\"ac\"}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11AC, "nested STANDARD/radio under another object ignored");
    scan("{\"other\":{\"mlan0\":{\"STANDARD\":\"n\"}}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_UNSET, "an mlan0 object below the root is not .mlan0");
    scan("{\"mlan0\":{\"radio\":{\"mode\":\"n\\\"x\"}}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_UNSET, "escaped string value rejected");

    /* ---- degenerate input ---- */
    scan(NULL, "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_UNSET && g_bw == OPC_BANDWIDTH_UNSET, "NULL json -> unset");
    scan("", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_UNSET && g_bw == OPC_BANDWIDTH_UNSET, "empty json -> unset");
    scan("[1,2]", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_UNSET, "non-object root -> unset");
    scan("{\"mlan0\":{\"STANDARD\":\"ax\"", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11AX, "truncated tail after the value still reads the value");
    scan("\xEF\xBB\xBF{\"mlan0\":{\"STANDARD\":\"n\"}}", "mlan0");
    ASSERT(g_mode == OPC_WLAN_MODE_11N, "UTF-8 BOM skipped");

    /* ---- file read ---- */
    char path[] = "/tmp/opcd_devinfo_cfg_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 2; }
    FILE *f = fdopen(fd, "w");
    fputs(SHIPPED, f);
    fclose(f);
    g_mode = g_bw = 0x55;
    opcd_devinfo_cfg_read(path, "mlan0", &g_mode, &g_bw);
    ASSERT(g_mode == OPC_WLAN_MODE_11AX && g_bw == OPC_BANDWIDTH_UNSET, "read file");
    unlink(path);
    g_mode = g_bw = 0x55;
    opcd_devinfo_cfg_read(path, "mlan0", &g_mode, &g_bw);
    ASSERT(g_mode == OPC_WLAN_MODE_UNSET && g_bw == OPC_BANDWIDTH_UNSET, "missing file -> unset");
    g_mode = g_bw = 0x55;
    opcd_devinfo_cfg_read(NULL, "mlan0", &g_mode, &g_bw);
    ASSERT(g_mode == OPC_WLAN_MODE_UNSET && g_bw == OPC_BANDWIDTH_UNSET, "NULL path -> unset");

    if (failures) { printf("test_devinfo_cfg: %d failure(s)\n", failures); return 1; }
    printf("test_devinfo_cfg: all passed\n");
    return 0;
}
