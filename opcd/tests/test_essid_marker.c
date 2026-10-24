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
/* NULL-safe: a missing file never matches (slurp returns NULL). */
static int marker_has(const char *needle) { char *m = slurp(g_marker); return m != NULL && strstr(m, needle) != NULL; }
static int conf_has(const char *needle)   { char *c = slurp(g_conf);   return c != NULL && strstr(c, needle) != NULL; }

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
static int arm(unsigned slot, time_t now) { return opc_essid_marker_arm(g_marker, g_conf, slot, now, NULL); }
static int arm_to(unsigned slot, time_t now, const char *ssid) { return opc_essid_marker_arm(g_marker, g_conf, slot, now, ssid); }
static int settle(void)                   { return opc_essid_marker_settle(g_marker, g_conf); }
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
    ASSERT(disarm() == 0 && exists(g_marker) && marker_has("applied=\"site-B\"\n"),
           "#161 disarm: a committed chain survives a later failed apply");
    ASSERT(restore_() == 1 && conf_has("ssid=\"site-A\"\n") && !exists(g_marker),
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
    ASSERT(arm(2, 6) == 0, "#161 powercut2 setup: arm (no requested SSID recorded)");
    put(g_conf, CONF_B);                            /* edit completed, cut before commit */
    ASSERT(restore_() == 2 && strcmp(slurp(g_conf), CONF_B) == 0 && !exists(g_marker),
           "#161 powercut: edit done, not committed, no target -> not reverted, marker removed");
    fresh(CONF_ORIG);
    ASSERT(arm_to(2, 6, "site-B") == 0, "#161 powercut2b setup: arm with the requested SSID");
    m = slurp(g_marker);
    ASSERT(m && strstr(m, "target=736974652d42\n"), "#161 arm: requested SSID recorded as target= hex");
    put(g_conf, CONF_B);                            /* helper installed the conf, cut before commit */
    ASSERT(restore_() == 1 && strcmp(slurp(g_conf), "network={\n    ssid=\"site-A\"\n}\n") == 0 && !exists(g_marker),
           "#161 powercut: edit done, not committed, conf decodes to the requested SSID -> original back");
    fresh(CONF_ORIG);
    ASSERT(arm_to(2, 6, "site-B") == 0, "#161 powercut2c setup: arm with the requested SSID");
    put(g_conf, CONF_FOREIGN);                      /* someone else wrote a different SSID */
    ASSERT(restore_() == 2 && strcmp(slurp(g_conf), CONF_FOREIGN) == 0 && !exists(g_marker),
           "#161 restore: uncommitted + conf is neither original nor requested -> untouched, marker removed");
    fresh("network={\n    ssid=\"site-A\"\n}\n");
    ASSERT(arm_to(2, 6, "si\"te") == 0, "#161 hex-target setup: arm with an SSID the helper renders as hex");
    put(g_conf, "network={\n    ssid=736922746\n}\n");    /* odd length: not a hex token */
    ASSERT(restore_() == 2 && conf_has("ssid=736922746\n") && !exists(g_marker),
           "#161 restore: malformed hex token is not matched");
    fresh("network={\n    ssid=\"site-A\"\n}\n");
    ASSERT(arm_to(2, 6, "si\"te") == 0, "#161 hex-target setup 2: arm");
    put(g_conf, "network={\n    ssid=7369227465\n}\n");    /* hex of si"te, as the helper writes it */
    ASSERT(restore_() == 1 && conf_has("ssid=\"site-A\"\n") && !exists(g_marker),
           "#161 restore: hex-rendered conf token decodes to the requested SSID -> original back");

    /* settle after a failed apply: unchanged conf -> like disarm; changed conf -> committed. */
    fresh(CONF_ORIG);
    ASSERT(arm_to(3, 7, "site-B") == 0, "#161 settle setup: arm");
    ASSERT(settle() == 0 && !exists(g_marker), "#161 settle: conf unchanged after a failed apply -> uncommitted marker removed");
    fresh(CONF_ORIG);
    ASSERT(arm_to(3, 7, "site-B") == 0, "#161 settle2 setup: arm");
    put(g_conf, CONF_B);                            /* helper installed the conf, then failed (exit 6 / TERM) */
    ASSERT(settle() == 1 && marker_has("applied=\"site-B\"\n"),
           "#161 settle: conf changed although the apply failed -> token committed");
    ASSERT(restore_() == 1 && strcmp(slurp(g_conf), "network={\n    ssid=\"site-A\"\n}\n") == 0 && !exists(g_marker),
           "#161 settle: the committed failed apply is restored at boot");
    fresh(CONF_ORIG);
    ASSERT(arm_to(3, 7, "site-B") == 0 && (put(g_conf, CONF_B), commit() == 0), "#161 settle3 setup: committed chain");
    ASSERT(arm_to(4, 8, "site-C") == 0, "#161 settle3 setup: repeat arm");
    ASSERT(settle() == 0 && marker_has("applied=\"site-B\"\n") && marker_has("essid=\"site-A\"\n") &&
           !marker_has("target="),
           "#161 settle: failed repeat apply with an unchanged conf keeps the committed chain without the failed target");
    put(g_conf, "network={\n    ssid=\"site-C\"\n}\n");       /* an operator later stores that very SSID */
    ASSERT(restore_() == 2 && conf_has("ssid=\"site-C\"\n") && !exists(g_marker),
           "#161 restore: a later foreign write of a failed request's SSID is not reverted");

    /* a later ChangeIp in the same boot after an apply whose commit never ran
     * (crash / write error between the helper's exit and commit): the chain
     * continues from the recorded target, so the first original survives. */
    fresh(CONF_ORIG);
    ASSERT(arm_to(1, 1, "site-B") == 0, "#161 uncommitted-chain setup: arm");
    put(g_conf, CONF_B);                            /* helper installed site-B; opcd died before commit */
    ASSERT(arm_to(2, 2, "site-C") == 0, "#161 arm: ok after an uncommitted apply of the requested SSID");
    m = slurp(g_marker);
    ASSERT(m && strstr(m, "essid=\"site-A\"\n") && strstr(m, "applied=\"site-B\"\n") && strstr(m, "target=736974652d43\n"),
           "#161 arm: continues the chain from the uncommitted target and records it as applied");
    ASSERT(settle() == 0 && marker_has("essid=\"site-A\"\n"),
           "#161 arm: a failed second apply keeps the chain");
    ASSERT(restore_() == 1 && conf_has("ssid=\"site-A\"\n") && !exists(g_marker),
           "#161 restore: first original back after an uncommitted apply and a failed retry");
    fresh(CONF_ORIG);
    ASSERT(arm_to(1, 1, "site-B") == 0 && (put(g_conf, CONF_B), arm_to(2, 2, "site-C") == 0), "#161 uncommitted-chain2 setup");
    put(g_conf, "network={\n    ssid=\"site-C\"\n}\n");
    ASSERT(commit() == 0 && restore_() == 1 && conf_has("ssid=\"site-A\"\n"),
           "#161 restore: first original back after an uncommitted apply and a successful retry");
    fresh(CONF_ORIG);
    ASSERT(arm_to(1, 1, "site-B") == 0 && (put(g_conf, CONF_B), arm_to(2, 2, "site-B") == 0), "#161 uncommitted-chain3 setup (same SSID retried)");
    ASSERT(commit() == 0 && restore_() == 1 && conf_has("ssid=\"site-A\"\n"),
           "#161 restore: first original back when the same SSID is retried after an uncommitted apply");

    /* settle never commits a token the helper did not write (another writer during the apply window). */
    fresh(CONF_ORIG);
    ASSERT(arm_to(3, 7, "site-B") == 0, "#161 settle-foreign setup: arm");
    put(g_conf, CONF_FOREIGN);                      /* wifi.sh wrote site-C while the helper was blocked */
    ASSERT(settle() == 2 && !exists(g_marker), "#161 settle: foreign token after a failed apply -> not committed, uncommitted marker removed");
    ASSERT(restore_() == 0 && strcmp(slurp(g_conf), CONF_FOREIGN) == 0, "#161 settle: nothing left to revert the foreign write");
    fresh(CONF_ORIG);
    ASSERT(arm_to(3, 7, "site-B") == 0 && (put(g_conf, CONF_B), commit() == 0), "#161 settle-foreign2 setup: committed chain");
    ASSERT(arm_to(4, 8, "site-C") == 0, "#161 settle-foreign2 setup: repeat arm");
    put(g_conf, "network={\n    ssid=\"site-D\"\n}\n");       /* another writer, not the requested site-C */
    ASSERT(settle() == 2 && marker_has("applied=\"site-B\"\n") && !marker_has("target="),
           "#161 settle: foreign token with a committed chain -> chain kept without the target, nothing committed");
    ASSERT(restore_() == 2 && conf_has("ssid=\"site-D\"\n") && !exists(g_marker),
           "#161 restore: the foreign write survives the boot, marker dropped");
    fresh(CONF_ORIG);
    ASSERT(arm(2, 6) == 0, "#161 powercut3 setup: arm");
    put(g_conf, CONF_B);
    ASSERT(commit() == 0, "#161 powercut3 setup: commit");   /* cut after commit */
    ASSERT(restore_() == 1 && conf_has("ssid=\"site-A\"") && !exists(g_marker),
           "#161 powercut: edit committed -> original ESSID back");

    /* hex-form token is carried verbatim. */
    fresh("network={\n    ssid=616263\n}\n");
    ASSERT(arm(1, 1) == 0, "#161 hex setup: arm");
    put(g_conf, "network={\n    ssid=\"x\"\n}\n");
    ASSERT(commit() == 0, "#161 hex setup: commit");
    ASSERT(restore_() == 1 && conf_has("ssid=616263\n"),
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
    ASSERT(restore_() < 0 && exists(g_marker) && conf_has("ssid=\"extra\""),
           "#161 restore: multi-network conf -> error, marker and conf untouched");

    /* #193 opc_wpa_conf_ssid: the configured SSID GetDeviceInfo reports. */
    {
        char s[32];
#define CONF_SSID(text, cap, want, label) do {                                   \
        put(g_conf, (text));                                                     \
        memset(s, 'x', sizeof s);                                                \
        ASSERT(opc_wpa_conf_ssid(g_conf, s, (cap)) == 0 && strcmp(s, (want)) == 0, \
               (label));                                                         \
    } while (0)
        CONF_SSID("network={\n    ssid=\"site-A\"\n}\n", sizeof s, "site-A", "#193 quoted ssid");
        CONF_SSID("network={\n\tssid=736974652d43\r\n}\n", sizeof s, "site-C", "#193 bare hex ssid, tab indent, CRLF");
        CONF_SSID("network={\n    ssid=\"\"\n}\n", sizeof s, "", "#193 empty quoted ssid");
        CONF_SSID("network={\n    ssid=\"a\"b\"\n}\n", sizeof s, "a\"b", "#193 quoted value is literal between the outer quotes");
        CONF_SSID("network={\n    #ssid=\"old\"\n    scan_ssid=1\n    bssid=00:11:22:33:44:55\n    ssid=\"real\"\n}\n",
                  sizeof s, "real", "#193 #ssid=, scan_ssid=, bssid= are not the ssid");
        CONF_SSID("network={\n    ssid=\"base\"\n}\nnetwork={\n    ssid=\"extra\"\n}\n", sizeof s, "base",
                  "#193 several network blocks (extra_ssid) -> the first, the base SSID");
        CONF_SSID("network={\n    ssid=P\"site\\x41\"\n}\n", sizeof s, "", "#193 P\"...\" escapes are not decoded -> empty");
        CONF_SSID("network={\n    ssid=73697\n}\n", sizeof s, "", "#193 odd-length hex -> empty");
        CONF_SSID("network={\n    ssid=7z\n}\n", sizeof s, "", "#193 non-hex digit -> empty");
        CONF_SSID("network={\n    ssid=7300\n}\n", sizeof s, "", "#193 hex with a NUL byte -> empty, not a cut SSID");
        CONF_SSID("network={\n    ssid=\"0123456789012345678901234567890\"\n}\n", sizeof s,
                  "0123456789012345678901234567890", "#193 31-byte SSID fits a 32-byte field");
        CONF_SSID("network={\n    ssid=\"01234567890123456789012345678901\"\n}\n", sizeof s, "",
                  "#193 32-byte SSID does not fit -> empty, never truncated");
        CONF_SSID("network={\n    ssid=3031323334353637383930313233343536373839303132333435363738393031\n}\n",
                  sizeof s, "", "#193 32-byte hex SSID does not fit -> empty");
        CONF_SSID("network={\n    priority=1\n}\n", sizeof s, "", "#193 no ssid= -> empty");
#undef CONF_SSID
        unlink(g_conf);
        memset(s, 'x', sizeof s);
        ASSERT(opc_wpa_conf_ssid(g_conf, s, sizeof s) < 0 && s[0] == '\0', "#193 unreadable conf -> error, empty");
    }

    printf(failures ? "test_essid_marker: %d FAILED\n" : "test_essid_marker: all passed\n", failures);
    return failures ? 1 : 0;
}
