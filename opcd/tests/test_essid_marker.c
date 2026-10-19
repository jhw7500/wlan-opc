/* Host unit test for the ChangeIpAddress ESSID restore marker
 * (opcd/essid_marker.{c,h}, issue #161): the marker is written BEFORE the
 * wpa_supplicant conf is edited, is committed with the applied token after a
 * successful apply, keeps the first original while that chain is intact, and
 * restore puts back just the `ssid=` line. Real files under a private tmp dir. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../essid_marker.h"

static int failures = 0;
#define ASSERT(cond, label) do {                          \
    if (cond) { printf("PASS %s\n", (label)); }           \
    else      { printf("FAIL %s\n", (label)); failures++; }\
} while (0)

static char g_dir[64], g_conf[96], g_marker[96];

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static char *slurp(const char *path)
{
    static char buf[2048];
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int exists(const char *p) { return access(p, F_OK) == 0; }

static const char CONF_ORIG[] =
    "ctrl_interface=/var/run/wpa_supplicant\n"
    "network={\n"
    "    ssid=\"site-A\"\n"
    "    psk=\"secret\"\n"
    "    freq_list=2412 2437\n"
    "}\n";

static void fresh(const char *conf)
{
    unlink(g_marker);
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_conf);
    unlink(tmp);
    snprintf(tmp, sizeof tmp, "%s.tmp", g_marker);
    unlink(tmp);
    put(g_conf, conf);
}
static int arm(unsigned slot, time_t now) { return opc_essid_marker_arm(g_marker, g_conf, slot, now); }
static int commit(void)                   { return opc_essid_marker_commit(g_marker, g_conf); }
static int disarm(void)                   { return opc_essid_marker_disarm(g_marker); }
static int restore_(void)                 { return opc_essid_marker_restore(g_marker, g_conf); }

static const char CONF_B[] = "network={\n    ssid=\"site-B\"\n}\n";
static const char CONF_FOREIGN[] = "network={\n    ssid=\"site-C\"\n    psk=\"other\"\n}\n";

int main(void)
{
    snprintf(g_dir, sizeof g_dir, "/tmp/test_essid_marker_%ld", (long)getpid());
    mkdir(g_dir, 0700);
    snprintf(g_conf, sizeof g_conf, "%s/wpa.conf", g_dir);
    snprintf(g_marker, sizeof g_marker, "%s/essid.restore", g_dir);

    /* arm: marker holds original token + slot + time, conf untouched, uncommitted. */
    fresh(CONF_ORIG);
    ASSERT(arm(7, 1700000000) == 0, "#161 arm: ok on single-network conf");
    char *m = slurp(g_marker);
    ASSERT(m && strstr(m, "essid=\"site-A\"\n"), "#161 arm: marker keeps the original ssid token");
    ASSERT(m && strstr(m, "slot=7\n"), "#161 arm: marker keeps the ChangeIp slot number");
    ASSERT(m && strstr(m, "time=1700000000\n"), "#161 arm: marker keeps the apply time");
    ASSERT(m && !strstr(m, "applied="), "#161 arm: marker is uncommitted until the apply succeeds");
    ASSERT(strcmp(slurp(g_conf), CONF_ORIG) == 0, "#161 arm: conf is not edited by arm");

    /* commit after a successful apply records the token the apply wrote. */
    put(g_conf, CONF_B);
    ASSERT(commit() == 0, "#161 commit: ok after the apply");
    m = slurp(g_marker);
    ASSERT(m && strstr(m, "essid=\"site-A\"\n") && strstr(m, "applied=\"site-B\"\n") &&
           strstr(m, "slot=7\n") && strstr(m, "time=1700000000\n"),
           "#161 commit: marker records the original and the applied tokens");

    /* second ChangeIp in the same chain (conf still holds what we applied):
     * FIRST original stays, latest slot recorded. */
    ASSERT(arm(9, 1700000100) == 0, "#161 arm: repeat arm ok");
    m = slurp(g_marker);
    ASSERT(m && strstr(m, "essid=\"site-A\"\n") && strstr(m, "applied=\"site-B\"\n"),
           "#161 arm: repeat change keeps only the first original ESSID");
    ASSERT(m && strstr(m, "slot=9\n"), "#161 arm: repeat arm records the latest slot");

    /* the repeat apply fails: disarm keeps the committed chain, restore uses it. */
    ASSERT(disarm() == 0 && exists(g_marker) && strstr(slurp(g_marker), "applied=\"site-B\"\n"),
           "#161 disarm: a committed chain survives a later failed apply");
    ASSERT(restore_() == 1 && strstr(slurp(g_conf), "ssid=\"site-A\"\n") && !exists(g_marker),
           "#161 restore: committed chain -> original back after the failed repeat");

    /* chain broken by another writer: arm starts a new chain from the current conf. */
    fresh(CONF_ORIG);
    ASSERT(arm(1, 1) == 0, "#161 broken-chain setup: arm");
    put(g_conf, CONF_B);
    ASSERT(commit() == 0, "#161 broken-chain setup: commit");
    put(g_conf, CONF_FOREIGN);                      /* wifi.sh / factory reset / operator */
    ASSERT(arm(2, 2) == 0, "#161 arm: ok after a foreign ssid change");
    m = slurp(g_marker);
    ASSERT(m && strstr(m, "essid=\"site-C\"\n") && !strstr(m, "applied=") && !strstr(m, "site-A"),
           "#161 arm: a foreign ssid change starts a new chain from the current conf");

    /* failed apply on a fresh (uncommitted) marker: disarm removes it. */
    fresh(CONF_ORIG);
    ASSERT(arm(1, 1) == 0, "#161 disarm setup: arm");
    ASSERT(disarm() == 0 && !exists(g_marker), "#161 disarm: failed apply removes the uncommitted marker");
    ASSERT(restore_() == 0 && strcmp(slurp(g_conf), CONF_ORIG) == 0,
           "#161 disarm: nothing left for restore to revert");
    ASSERT(disarm() == 0, "#161 disarm: no marker -> 0");

    /* arm refuses when the conf cannot be restored line-for-line. */
    fresh("network={\n ssid=\"a\"\n}\nnetwork={\n ssid=\"b\"\n}\n");
    ASSERT(arm(1, 1) < 0 && !exists(g_marker), "#161 arm: multi-network conf -> error and no marker");
    unlink(g_marker);
    unlink(g_conf);
    ASSERT(arm(1, 1) < 0 && !exists(g_marker), "#161 arm: missing conf -> error and no marker");

    /* restore: only ssid= goes back; freq_list changed by SetRadioConfig stays. */
    fresh(CONF_ORIG);
    ASSERT(arm(3, 5) == 0, "#161 restore setup: arm");
    put(g_conf,
        "ctrl_interface=/var/run/wpa_supplicant\n"
        "network={\n"
        "    ssid=\"site-B\"\n"
        "    psk=\"secret\"\n"
        "    freq_list=5180\n"
        "}\n");
    ASSERT(commit() == 0, "#161 restore setup: commit");
    ASSERT(restore_() == 1, "#161 restore: returns 1 when restored");
    ASSERT(strcmp(slurp(g_conf),
        "ctrl_interface=/var/run/wpa_supplicant\n"
        "network={\n"
        "    ssid=\"site-A\"\n"
        "    psk=\"secret\"\n"
        "    freq_list=5180\n"
        "}\n") == 0, "#161 restore: ssid= reverted, freq_list (SetRadioConfig) untouched");
    ASSERT(!exists(g_marker), "#161 restore: marker removed");
    ASSERT(restore_() == 0, "#161 restore: no marker -> 0");

    /* restore never reverts a value this chain did not apply. */
    fresh(CONF_ORIG);
    ASSERT(arm(3, 5) == 0, "#161 foreign setup: arm");
    put(g_conf, CONF_B);
    ASSERT(commit() == 0, "#161 foreign setup: commit");
    put(g_conf, CONF_FOREIGN);                      /* changed again by another writer */
    ASSERT(restore_() == 2 && strcmp(slurp(g_conf), CONF_FOREIGN) == 0 && !exists(g_marker),
           "#161 restore: ssid changed by another writer -> marker dropped, conf byte-identical");

    /* the applied value equals the original (ESSID unchanged): nothing to put back. */
    fresh(CONF_ORIG);
    ASSERT(arm(3, 5) == 0 && commit() == 0, "#161 same-value setup: arm + commit");
    ASSERT(restore_() == 0 && strcmp(slurp(g_conf), CONF_ORIG) == 0 && !exists(g_marker),
           "#161 restore: applied value equals the original -> no edit, marker removed");

    /* Power cuts. (1) after arm, before/while the conf was edited: nothing was
     * applied, the conf is left alone and the marker goes. (2) after the edit
     * but before commit: the change cannot be verified as ours, so it is NOT
     * reverted (logged by -R) and the marker goes. (3) after commit: restored. */
    fresh(CONF_ORIG);
    ASSERT(arm(2, 6) == 0, "#161 powercut setup: arm");
    {
        char tmp[128];
        snprintf(tmp, sizeof tmp, "%s.tmp", g_conf);
        put(tmp, "network={\n    ssid=\"hal");   /* torn temp from the cut */
    }
    ASSERT(restore_() == 0 && strcmp(slurp(g_conf), CONF_ORIG) == 0 && !exists(g_marker),
           "#161 powercut: marker present + conf not yet edited -> nothing applied, marker removed");
    fresh(CONF_ORIG);
    ASSERT(arm(2, 6) == 0, "#161 powercut2 setup: arm");
    put(g_conf, CONF_B);                            /* edit completed, cut before commit */
    ASSERT(restore_() == 2 && strcmp(slurp(g_conf), CONF_B) == 0 && !exists(g_marker),
           "#161 powercut: edit done but not committed -> not reverted (unverifiable), marker removed");
    fresh(CONF_ORIG);
    ASSERT(arm(2, 6) == 0, "#161 powercut3 setup: arm");
    put(g_conf, CONF_B);
    ASSERT(commit() == 0, "#161 powercut3 setup: commit");   /* cut after commit */
    ASSERT(restore_() == 1 && strstr(slurp(g_conf), "ssid=\"site-A\"") && !exists(g_marker),
           "#161 powercut: edit committed -> original ESSID back");

    /* hex-form token is carried verbatim. */
    fresh("network={\n    ssid=616263\n}\n");
    ASSERT(arm(1, 1) == 0, "#161 hex setup: arm");
    put(g_conf, "network={\n    ssid=\"x\"\n}\n");
    ASSERT(commit() == 0, "#161 hex setup: commit");
    ASSERT(restore_() == 1 && strstr(slurp(g_conf), "ssid=616263\n"),
           "#161 restore: hex ssid token restored verbatim");

    /* unusable marker: both files left alone, by restore and by disarm. */
    fresh(CONF_ORIG);
    put(g_marker, "garbage\n");
    ASSERT(restore_() < 0 && exists(g_marker) && strcmp(slurp(g_conf), CONF_ORIG) == 0,
           "#161 restore: corrupt marker -> error, marker and conf untouched");
    ASSERT(disarm() < 0 && exists(g_marker), "#161 disarm: corrupt marker left for -R to report");

    /* multi-network conf at restore time (Mode A extra blocks): untouched, marker kept. */
    fresh(CONF_ORIG);
    ASSERT(arm(1, 1) == 0, "#161 multi setup: arm");
    put(g_conf, CONF_B);
    ASSERT(commit() == 0, "#161 multi setup: commit");
    put(g_conf, "network={\n    ssid=\"site-B\"\n}\nnetwork={\n    ssid=\"extra\"\n}\n");
    ASSERT(restore_() < 0 && exists(g_marker) && strstr(slurp(g_conf), "ssid=\"extra\""),
           "#161 restore: multi-network conf -> error, marker and conf untouched");

    printf(failures ? "test_essid_marker: %d FAILED\n" : "test_essid_marker: all passed\n", failures);
    return failures ? 1 : 0;
}
