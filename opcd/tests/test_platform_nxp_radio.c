#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"
#include "../../protocol/scan_chlist.h"

#define ASSERT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } \
} while (0)

static char g_exec_log[128];
static const char *g_cleanup_marker = "/tmp/test_platform_nxp_radio_cleanup";
static const char *g_survivor_marker = "/tmp/test_platform_nxp_radio_survivor";
static const char *g_helper_pid_marker = "/tmp/test_platform_nxp_radio_pid";

static void timeout_helper_term(int sig)
{
    (void)sig;
    int fd = open(g_cleanup_marker, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) close(fd);
    _exit(0);
}

static int run_timeout_helper(void)
{
    int pfd = open(g_helper_pid_marker, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (pfd < 0) return 122;
    dprintf(pfd, "%ld\n", (long)getpid());
    close(pfd);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = getenv("OPCD_NXP_TIMEOUT_IGNORE_TERM") != NULL
                  ? SIG_IGN : timeout_helper_term;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    pid_t descendant = fork();
    if (descendant < 0) return 121;
    if (descendant == 0) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 200 * 1000 * 1000L };
        nanosleep(&ts, NULL);
        int fd = open(g_survivor_marker, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) close(fd);
        _exit(0);
    }
    for (;;) pause();
}

/* run_argv_bounded remains real (fork, wait, timeout, status handling).  Only
 * the external exec boundary is replaced so the test can observe the exact
 * helper contract without touching target files or tools. */
int __wrap_execv(const char *path, char *const argv[])
{
    if (getenv("OPCD_TEST_REAL_EXEC") != NULL) {
        extern int __real_execv(const char *, char *const []);
        return __real_execv(path, argv);
    }
    const char *delay_env = getenv("OPCD_NXP_EXEC_DELAY_MS");
    if (delay_env != NULL) {
        long delay_ms = strtol(delay_env, NULL, 10);
        struct timespec delay = {
            .tv_sec = delay_ms / 1000,
            .tv_nsec = (delay_ms % 1000) * 1000L * 1000L,
        };
        nanosleep(&delay, NULL);
    }
    int fd = open(g_exec_log, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) _exit(120);
    dprintf(fd, "path=%s", path);
    for (size_t i = 0; argv[i] != NULL; i++)
        dprintf(fd, "|%s", argv[i]);
    dprintf(fd, "\n");
    if (getenv("OPCD_TEST_MARKER_PATH") != NULL)   /* #161: marker state at exec time */
        dprintf(fd, "marker_present=%d\n", access(getenv("OPCD_TEST_MARKER_PATH"), F_OK) == 0);
    close(fd);
    if (getenv("OPCD_NXP_FAIL_MLAN1") != NULL &&
        argv[1] != NULL && strcmp(argv[1], "mlan1") == 0)
        _exit(42);
    if (getenv("OPCD_NXP_FAIL_WLAN_APPLY") != NULL &&   /* #161: ESSID apply refused/failed */
        argv[0] != NULL && strcmp(argv[0], "opc_wlan_apply.sh") == 0)
        _exit(42);
    if (argv[0] != NULL && strcmp(argv[0], "wlan_reboot_policy.sh") == 0) {   /* #164 outcomes of the real script */
        if (getenv("OPCD_NXP_FAIL_RESET") != NULL)       _exit(2);      /* non-zero exit: systemd-run/executor failure (policy gates are bypassed by --force) */
        if (getenv("OPCD_NXP_RESET_STOPPED") != NULL) {                 /* #179 on the board: the shutdown stops D-Bus, the client exits 1, */
            kill(getppid(), SIGTERM);                                   /* and opcd.service's stop job TERMs opcd (the caller) right after */
            _exit(1);
        }
        if (getenv("OPCD_NXP_RESET_EXIT1") != NULL)      _exit(1);      /* #179: exit 1 with no stop signal following it */
        if (getenv("OPCD_NXP_RESET_NOEXEC") != NULL)     _exit(127);    /* execv failure / command not found (bash 127) */
        if (getenv("OPCD_NXP_RESET_SIGNALED") != NULL)   raise(SIGTERM); /* the reboot's stop job killed the systemd-run client */
        if (getenv("OPCD_NXP_RESET_KILLED") != NULL)     kill(getpid(), SIGKILL); /* died by another signal: no hand-off evidence */
    }
    if (getenv("OPCD_NXP_FAIL_WLAN_APPLY_AFTER_EDIT") != NULL &&   /* #161: helper installed the conf, then failed */
        argv[0] != NULL && strcmp(argv[0], "opc_wlan_apply.sh") == 0) {
        const char *conf = getenv("OPCD_TEST_CONF_PATH");
        const char *ssid = NULL;
        for (size_t i = 1; argv[i] != NULL && argv[i + 1] != NULL; i++)
            if (strcmp(argv[i], "ssid") == 0) ssid = argv[i + 1];
        if (strcmp(getenv("OPCD_NXP_FAIL_WLAN_APPLY_AFTER_EDIT"), "foreign") == 0)
            ssid = "site-X";                         /* another writer's value, not the request */
        if (conf != NULL && ssid != NULL) {
            FILE *f = fopen(conf, "w");
            if (f) { fprintf(f, "network={\n    ssid=\"%s\"\n}\n", ssid); fclose(f); }
        }
        _exit(6);
    }
    _exit(0);
}

/* ChangeIpAddress fixture dir — must equal the -D path overrides given to
 * platform_nxp_test.o in tests/Makefile. */
#define IPT_DIR     "/tmp/opcd_nxp_ip_test"
#define IPT_CONF    IPT_DIR "/wpa_supplicant-mlan0.conf"
#define IPT_MARKER  IPT_DIR "/changeip_essid.restore"
#define IPT_DROPIN  IPT_DIR "/timesyncd.conf.d/opc-ntp.conf"
#define IPT_TSYNCD  IPT_DIR "/timesyncd.conf"

static void ipt_put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static char *ipt_slurp(const char *path)
{
    static char buf[1024];
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void ipt_reset(const char *conf)
{
    unlink(IPT_MARKER);
    unlink(IPT_DROPIN);
    rmdir(IPT_DIR "/timesyncd.conf.d");
    unlink(g_exec_log);
    ipt_put(IPT_CONF, conf);
    ipt_put(IPT_TSYNCD, "[Time]\nNTP=10.0.0.9\n");
}

/* #164: does the captured stderr file contain needle? (small files only) */
static int file_has(const char *path, const char *needle)
{
    static char buf[8192];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static int read_log(char *buf, size_t cap)
{
    int fd = open(g_exec_log, O_RDONLY);
    if (fd < 0) return -errno;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -errno;
    buf[n] = '\0';
    return 0;
}

static pid_t read_helper_pid(void)
{
    char buf[32];
    int fd = open(g_helper_pid_marker, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return (pid_t)strtol(buf, NULL, 10);
}

static int signal_masks_equal(const sigset_t *left, const sigset_t *right)
{
    for (int sig = 1; sig <= SIGRTMAX; sig++) {
        int left_member = sigismember(left, sig);
        int right_member = sigismember(right, sig);
        if (left_member != right_member) return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (getenv("OPCD_NXP_TIMEOUT_HELPER") != NULL)
        return run_timeout_helper();
    (void)argc;
    (void)argv;
    snprintf(g_exec_log, sizeof g_exec_log, "/tmp/test_platform_nxp_radio_%ld.log",
             (long)getpid());
    unlink(g_exec_log);

    opcd_platform_register();
    const opcd_platform_ops_t *ops = opcd_platform();
    ASSERT(ops != NULL, "NXP platform registered");

    /* #164: capture opcd's stderr around one prepare_reset call so the
     * classification wording (what on-target triage will read) is asserted,
     * not just the return value. */
    static int g_saved_err = -1;
    #define ERR_CAPTURE_BEGIN(path) do { fflush(stderr); g_saved_err = dup(2); \
        int efd_ = open((path), O_WRONLY | O_CREAT | O_TRUNC, 0600); dup2(efd_, 2); close(efd_); } while (0)
    #define ERR_CAPTURE_END() do { fflush(stderr); dup2(g_saved_err, 2); close(g_saved_err); g_saved_err = -1; } while (0)
    char errlog[128];
    snprintf(errlog, sizeof errlog, "test_nxp_err_%ld.tmp", (long)getpid());
    #define ERR_HAS(needle) file_has(errlog, (needle))

    /* ---- #164: Reset → reboot through the package's reboot policy script.
     * The fake helper models what the real script does (see nxp_prepare_reset):
     * exit 0 = dedupe collision (another request holds the lock), SIGTERM'd
     * client = the reboot's stop job reached it, exit 2 = a non-zero exit
     * (systemd-run/executor failure; --force bypasses the policy gates), exit
     * 127 = could not run (execv failure / command not found), no exit within the
     * bound (400 ms in this build) = handed off and still running. ---- */
    {
        char log[2048];
        int execs;
        unlink(g_exec_log);
        ERR_CAPTURE_BEGIN(errlog);
        int zrc = ops->prepare_reset();
        ERR_CAPTURE_END();
        ASSERT(zrc == 0 && ERR_HAS("another reboot request already holds the dedupe lock") && ERR_HAS("may still refuse"),
               "#164 prepare_reset: script exit 0 (dedupe: another request holds the reboot lock) -> 0, logged as held-by-another-request, not as a reboot under way");
        ASSERT(read_log(log, sizeof log) == 0 &&
               strstr(log, "path=/proc/self/exe|wlan_reboot_policy.sh|--source|opc_reset|--reason|opc_reset_command|--force\n") != NULL,
               "#164 prepare_reset: wlan_reboot_policy.sh --source opc_reset --reason opc_reset_command --force");
        execs = 0;
        for (const char *p = log; (p = strstr(p, "path=")) != NULL; p++) execs++;
        ASSERT(execs == 1, "#164 prepare_reset: exactly one exec — the policy script, nothing else");

        unlink(g_exec_log);
        setenv("OPCD_NXP_RESET_SIGNALED", "1", 1);
        ERR_CAPTURE_BEGIN(errlog);
        int src = ops->prepare_reset();
        ERR_CAPTURE_END();
        unsetenv("OPCD_NXP_RESET_SIGNALED");
        ASSERT(src == 0 && ERR_HAS("terminated by signal 15") && ERR_HAS("reboot under way"),
               "#164 prepare_reset: client killed by SIGTERM (the shutdown's stop job) -> 0, logged as reboot under way");

        unlink(g_exec_log);
        setenv("OPCD_NXP_RESET_KILLED", "1", 1);
        ERR_CAPTURE_BEGIN(errlog);
        int krc = ops->prepare_reset();
        ERR_CAPTURE_END();
        unsetenv("OPCD_NXP_RESET_KILLED");
        ASSERT(krc == OPC_RESET_UNCONFIRMED && ERR_HAS("killed by signal 9") && ERR_HAS("reboot not confirmed") &&
               !ERR_HAS("reboot under way") && !ERR_HAS("no reboot started"),
               "#164 prepare_reset: client killed by SIGKILL (not the stop job) -> OPC_RESET_UNCONFIRMED, logged as reboot not confirmed (neither under way nor no reboot started)");

        unlink(g_exec_log);
        setenv("OPCD_NXP_FAIL_RESET", "1", 1);
        ERR_CAPTURE_BEGIN(errlog);
        int frc = ops->prepare_reset();
        ERR_CAPTURE_END();
        unsetenv("OPCD_NXP_FAIL_RESET");
        ASSERT(frc == 2 && ERR_HAS("exited 2") && ERR_HAS("no reboot started") && !ERR_HAS("refused") && !ERR_HAS("could not run"),
               "#164 prepare_reset: script exit 2 (executor failure) -> 2, logged as no reboot started (neutral, not 'refused')");
        execs = 0;
        ASSERT(read_log(log, sizeof log) == 0 && strstr(log, "|wlan_reboot_policy.sh|") != NULL,
               "#164 prepare_reset: the refused attempt was the policy script");
        for (const char *p = log; (p = strstr(p, "path=")) != NULL; p++) execs++;
        ASSERT(execs == 1 && strstr(log, "path=/sbin/") == NULL && strstr(log, "systemctl") == NULL,
               "#164 prepare_reset: no direct reboot fallback after the script refuses");

        /* #179: on the board the client exited 1 when the shutdown the
         * executor had started stopped D-Bus, and opcd.service's stop job
         * TERMed opcd right after (sys.log "policy script exited 1 — no reboot
         * started" while the board rebooted). opcd keeps TERM blocked for its
         * signalfd; this test blocks it the same way around the call. */
        sigset_t term_only, saved_mask;
        sigemptyset(&term_only);
        sigaddset(&term_only, SIGTERM);
        sigprocmask(SIG_BLOCK, &term_only, &saved_mask);
        unlink(g_exec_log);
        setenv("OPCD_NXP_RESET_STOPPED", "1", 1);
        ERR_CAPTURE_BEGIN(errlog);
        int drc = ops->prepare_reset();
        ERR_CAPTURE_END();
        unsetenv("OPCD_NXP_RESET_STOPPED");
        sigset_t pending;
        sigpending(&pending);
        int term_left = sigismember(&pending, SIGTERM);
        if (term_left) {                       /* a regression left it pending: drain it so */
            struct timespec zero = { 0, 0 };   /* the ASSERT below reports, not the TERM   */
            (void)sigtimedwait(&term_only, NULL, &zero);
        }
        sigprocmask(SIG_SETMASK, &saved_mask, NULL);
        ASSERT(drc == 0 && ERR_HAS("exited 1") && ERR_HAS("stop signal") && ERR_HAS("reboot under way") &&
               !ERR_HAS("no reboot started"),
               "#179 prepare_reset: client exit 1 followed by opcd's stop TERM -> 0, logged as reboot under way (not 'no reboot started')");
        ASSERT(term_left == 0, "#179 prepare_reset: the stop TERM was consumed by the classification wait");
        execs = 0;
        ASSERT(read_log(log, sizeof log) == 0, "#179 prepare_reset: exec log readable");
        for (const char *p = log; (p = strstr(p, "path=")) != NULL; p++) execs++;
        ASSERT(execs == 1 && strstr(log, "path=/sbin/") == NULL && strstr(log, "systemctl") == NULL,
               "#179 prepare_reset: the classification spawned nothing — only the policy script ran");

        unlink(g_exec_log);
        setenv("OPCD_NXP_RESET_EXIT1", "1", 1);
        ERR_CAPTURE_BEGIN(errlog);
        int nsrc = ops->prepare_reset();
        ERR_CAPTURE_END();
        unsetenv("OPCD_NXP_RESET_EXIT1");
        ASSERT(nsrc == 1 && ERR_HAS("exited 1") && ERR_HAS("no reboot started") && !ERR_HAS("stop signal"),
               "#179 prepare_reset: client exit 1 with no stop TERM within the wait -> 1, still no reboot started");

        unlink(g_exec_log);
        setenv("OPCD_NXP_RESET_NOEXEC", "1", 1);
        ERR_CAPTURE_BEGIN(errlog);
        int nrc = ops->prepare_reset();
        ERR_CAPTURE_END();
        unsetenv("OPCD_NXP_RESET_NOEXEC");
        ASSERT(nrc == 127 && ERR_HAS("could not run") && ERR_HAS("exit 127") && !ERR_HAS("executor aborted") && !ERR_HAS("exhausted"),
               "#164 prepare_reset: exit 127 (could not run: execv failure / command not found) -> 127, logged as could-not-run, no reboot started");

        unlink(g_exec_log);
        setenv("OPCD_NXP_EXEC_DELAY_MS", "1500", 1);        /* helper outlives the 400 ms test bound */
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int trc = ops->prepare_reset();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        unsetenv("OPCD_NXP_EXEC_DELAY_MS");
        long took_ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        /* run_argv_bounded sends SIGTERM 150 ms before the bound, which ends the
         * sleeping fake helper, so the call returns between 250 and 400 ms —
         * well before the helper's own 1500 ms would have elapsed. */
        ASSERT(trc == 0 && took_ms >= 200 && took_ms < 1400,
               "#164 prepare_reset: script still running at the bound -> 0 (handed off, continuing in the transient unit), returned at the bound");
        unlink(g_exec_log);
        unlink(errlog);
    }
    #undef ERR_HAS
    #undef ERR_CAPTURE_BEGIN
    #undef ERR_CAPTURE_END

    /* ---- #161/#162: ChangeIpAddress apply — ESSID marker, Gateway, NTP ---- */
    {
        mkdir(IPT_DIR, 0700);
        setenv("OPCD_TEST_MARKER_PATH", IPT_MARKER, 1);
        char log[2048];
        opc_ipcfg_entry_t e;
        memset(&e, 0, sizeof e);
        e.ip_address      = 0xC0A80165u;   /* 192.168.1.101 */
        e.subnet_mask     = 0xFFFFFF00u;
        e.default_gateway = 0xC0A80101u;   /* 192.168.1.1 */
        e.ntp_server      = 0xC0A80105u;   /* 192.168.1.5 */
        snprintf(e.essid, sizeof e.essid, "%s", "site-B");

        ipt_reset("network={\n    ssid=\"site-A\"\n    freq_list=2412\n}\n");
        ASSERT(ops->apply_ip_change(&e, 0, 4) == 0, "#162 apply_ip_change succeeds");
        ASSERT(read_log(log, sizeof log) == 0, "#162 apply invocations captured");
        const char *p_ip  = strstr(log, "ip addr replace 192.168.1.101/24 dev eth0");
        const char *p_gw  = strstr(log, "ip route replace default via 192.168.1.1 dev eth0");
        ASSERT(p_ip != NULL, "#162 IP address applied with `ip addr replace` (re-selecting the current address is not EEXIST)");
        ASSERT(strstr(log, "ip addr replace 192.168.1.101/24 dev eth0 && ip -4 -o addr show dev eth0 | "
                           "grep -q 'inet 192.168.1.101/24 .*scope global' && ip -4 addr show dev eth0 | awk") != NULL,
               "#162 other addresses are deleted only after the new one is verified present at global scope (host-scope /32 mirror guard)");
        ASSERT(p_gw != NULL && p_ip != NULL && p_gw > p_ip,
               "#162 Gateway: default route applied (volatile `ip route`) after the address");
        ASSERT(strstr(log, "onlink") == NULL, "#162 Gateway: no onlink for an in-subnet gateway");
        ASSERT(strstr(log, "systemctl --no-block try-restart systemd-timesyncd.service") != NULL,
               "#162 NTP: timesyncd re-reads its volatile drop-in");
        ASSERT(ipt_slurp(IPT_DROPIN) != NULL &&
               strcmp(ipt_slurp(IPT_DROPIN), "[Time]\nNTP=\nNTP=192.168.1.5\n") == 0,
               "#162 NTP: drop-in resets the /etc server list, then sets the ChangeIp server (volatile /run)");
        uint32_t ntp = 0;
        ASSERT(ops->get_ntp_server(&ntp) == 0 && ntp == 0xC0A80105u,
               "#162 GetDeviceInfo NTP reads the runtime drop-in, not only /etc");
        ASSERT(strstr(log, "|opc_wlan_apply.sh|mlan0|ssid|site-B\nmarker_present=1\n") != NULL,
               "#161 marker is durable BEFORE the ESSID conf edit is requested");
        const char *mk = ipt_slurp(IPT_MARKER);
        ASSERT(mk != NULL && strstr(mk, "essid=\"site-A\"\n") != NULL && strstr(mk, "slot=4\n") != NULL,
               "#161 marker holds the original ESSID and the applied slot");
        ASSERT(mk != NULL && strstr(mk, "applied=\"site-A\"\n") != NULL,
               "#161 marker is committed after a successful ESSID apply (conf token recorded)");
        ASSERT(mk != NULL && strstr(mk, "target=736974652d42\n") != NULL,
               "#161 marker records the requested SSID (target= hex) before the apply");
        /* #193: GetDeviceInfo ESSID is the configured one — the conf's ssid=,
         * read with no link.json on this host. The fake helper does not edit
         * the conf on success, so it still holds the original. */
        char es[32];
        ASSERT(ops->get_essid(0, es, sizeof es) == 0 && strcmp(es, "site-A") == 0,
               "#193 ESSID = the conf's ssid while no link.json exists");

        ipt_reset("network={\n    ssid=\"site-A\"\n}\n");
        setenv("OPCD_NXP_FAIL_WLAN_APPLY", "1", 1);
        ASSERT(ops->apply_ip_change(&e, 0, 4) == 0, "#161 failed ESSID apply: IP result stays success");
        ASSERT(read_log(log, sizeof log) == 0 &&
               strstr(log, "|opc_wlan_apply.sh|mlan0|ssid|site-B\nmarker_present=1\n") != NULL &&
               access(IPT_MARKER, F_OK) != 0,
               "#161 failed ESSID apply disarms the marker (nothing for -R to revert)");
        unsetenv("OPCD_NXP_FAIL_WLAN_APPLY");

        ipt_reset("network={\n    ssid=\"site-A\"\n}\n");
        setenv("OPCD_TEST_CONF_PATH", IPT_CONF, 1);
        setenv("OPCD_NXP_FAIL_WLAN_APPLY_AFTER_EDIT", "1", 1);
        ASSERT(ops->apply_ip_change(&e, 0, 4) == 0, "#161 apply failed after the conf install: IP result stays success");
        mk = ipt_slurp(IPT_MARKER);
        ASSERT(mk != NULL && strstr(mk, "essid=\"site-A\"\n") != NULL && strstr(mk, "applied=\"site-B\"\n") != NULL,
               "#161 apply failed after the conf install -> marker settled with the installed token (restorable)");
        ASSERT(ops->get_essid(0, es, sizeof es) == 0 && strcmp(es, "site-B") == 0,
               "#193 ESSID follows the conf the ChangeIp helper installed");
        ipt_reset("network={\n    ssid=\"site-A\"\n}\n");
        setenv("OPCD_NXP_FAIL_WLAN_APPLY_AFTER_EDIT", "foreign", 1);
        ASSERT(ops->apply_ip_change(&e, 0, 4) == 0, "#161 apply failed while another writer changed the conf: IP result stays success");
        ASSERT(access(IPT_MARKER, F_OK) != 0,
               "#161 apply failed with a foreign ssid in the conf -> nothing committed, uncommitted marker removed");
        unsetenv("OPCD_NXP_FAIL_WLAN_APPLY_AFTER_EDIT");
        unsetenv("OPCD_TEST_CONF_PATH");

        ipt_reset("network={\n    ssid=\"site-A\"\n}\n");
        e.subnet_mask = 0xFFFFFFFFu;               /* /32 host slot, off-block gateway (P2P) */
        e.default_gateway = 0x0A000001u;           /* 10.0.0.1 */
        ASSERT(ops->apply_ip_change(&e, 0, 4) == 0, "#162 /32 slot apply succeeds");
        ASSERT(read_log(log, sizeof log) == 0 &&
               strstr(log, "ip route replace default via 10.0.0.1 dev eth0 onlink") != NULL,
               "#162 Gateway: a /32 slot installs its off-block gateway with onlink");
        e.subnet_mask = 0xFFFFFF00u;
        e.default_gateway = 0xC0A80101u;

        ipt_reset("network={\n    ssid=\"site-A\"\n}\n");
        ASSERT(ops->apply_ip_change(&e, 1, 4) == 0, "#162 apply on mlan0 succeeds");
        ASSERT(read_log(log, sizeof log) == 0 &&
               strstr(log, "ip route replace default via 192.168.1.1 dev mlan0") != NULL,
               "#162 Gateway follows the management interface (mlan0)");

        ipt_reset("network={\n    ssid=\"site-A\"\n}\n");
        e.default_gateway = 0;
        e.ntp_server = 0;
        ASSERT(ops->apply_ip_change(&e, 0, 4) == 0, "#162 apply with GW/NTP unset succeeds");
        ASSERT(read_log(log, sizeof log) == 0 &&
               strstr(log, "route replace default") == NULL && strstr(log, "try-restart") == NULL &&
               access(IPT_DROPIN, F_OK) != 0,
               "#162 0.0.0.0 gateway/NTP are unset: nothing applied");
        e.default_gateway = 0xC0A80101u;
        e.ntp_server = 0xC0A80105u;

        ipt_reset("network={\n ssid=\"a\"\n}\nnetwork={\n ssid=\"b\"\n}\n");
        ASSERT(ops->apply_ip_change(&e, 0, 4) == 0, "#161 multi-network: IP result stays success");
        ASSERT(read_log(log, sizeof log) == 0 && strstr(log, "opc_wlan_apply.sh") == NULL &&
               access(IPT_MARKER, F_OK) != 0,
               "#161 no marker -> the persistent ESSID edit is NOT attempted");

        ipt_reset("");
        unlink(IPT_DROPIN);
        ASSERT(ops->get_ntp_server(&ntp) == 0 && ntp == 0x0A000009u,
               "#162 GetDeviceInfo NTP falls back to /etc timesyncd.conf without a drop-in");

        unsetenv("OPCD_TEST_MARKER_PATH");
        unlink(g_exec_log);
    }

    opc_set_radio_config_req_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.station_type = OPC_STATION_SINGLE;
    cfg.priority_ch = OPC_PRIORITY_CH_UNSET;
    cfg.wlan1.mode = OPC_WLAN_MODE_11AC;
    cfg.wlan1.bandwidth = OPC_BANDWIDTH_80;
    cfg.wlan1.scan_band = OPC_SCAN_BAND_5GHZ;
    opc_scan_list_set_channel(cfg.wlan1.scan_chlist, OPC_SCAN_BAND_5GHZ, 36);

    ASSERT(ops->apply_radio_config(&cfg) == 0, "single radio apply succeeds");

    char log[512];
    ASSERT(read_log(log, sizeof log) == 0, "captured helper invocation");
    ASSERT(strcmp(log,
                  "path=/proc/self/exe|opc_wlan_apply.sh|mlan0|freq|5180|mode|ac|bw|80\n") == 0,
           "NXP apply passes frequency, mode and bandwidth in one helper call");

    struct mapping_case {
        uint8_t mode;
        uint8_t bandwidth;
        uint16_t band;
        uint8_t channel;
        const char *want;
    } mappings[] = {
        { OPC_WLAN_MODE_11B,  OPC_BANDWIDTH_20, OPC_SCAN_BAND_2_4GHZ, 1,
          "path=/proc/self/exe|opc_wlan_apply.sh|mlan0|freq|2412|mode|b|bw|20\n" },
        { OPC_WLAN_MODE_11G,  OPC_BANDWIDTH_40, OPC_SCAN_BAND_2_4GHZ, 6,
          "path=/proc/self/exe|opc_wlan_apply.sh|mlan0|freq|2437|mode|g|bw|40\n" },
        { OPC_WLAN_MODE_11A,  OPC_BANDWIDTH_20, OPC_SCAN_BAND_5GHZ, 36,
          "path=/proc/self/exe|opc_wlan_apply.sh|mlan0|freq|5180|mode|a|bw|20\n" },
        { OPC_WLAN_MODE_11N,  OPC_BANDWIDTH_40, OPC_SCAN_BAND_2_4GHZ, 11,
          "path=/proc/self/exe|opc_wlan_apply.sh|mlan0|freq|2462|mode|n|bw|40\n" },
        { OPC_WLAN_MODE_11AC, OPC_BANDWIDTH_80, OPC_SCAN_BAND_5GHZ, 40,
          "path=/proc/self/exe|opc_wlan_apply.sh|mlan0|freq|5200|mode|ac|bw|80\n" },
        { OPC_WLAN_MODE_11AX, OPC_BANDWIDTH_80, OPC_SCAN_BAND_5GHZ, 44,
          "path=/proc/self/exe|opc_wlan_apply.sh|mlan0|freq|5220|mode|ax|bw|80\n" },
    };
    for (size_t i = 0; i < sizeof mappings / sizeof mappings[0]; i++) {
        unlink(g_exec_log);
        memset(&cfg.wlan1, 0, sizeof cfg.wlan1);
        cfg.wlan1.mode = mappings[i].mode;
        cfg.wlan1.bandwidth = mappings[i].bandwidth;
        cfg.wlan1.scan_band = mappings[i].band;
        opc_scan_list_set_channel(cfg.wlan1.scan_chlist, mappings[i].band,
                                  mappings[i].channel);
        ASSERT(ops->apply_radio_config(&cfg) == 0, "mode/bandwidth mapping apply succeeds");
        ASSERT(read_log(log, sizeof log) == 0, "mapping helper invocation captured");
        ASSERT(strcmp(log, mappings[i].want) == 0,
               "protocol mode/bandwidth maps to literal helper tokens");
    }

    /* SetRadioConfig persists before its response, so each sequential DUAL
     * helper gets the normal bounded apply window. A 500 ms helper is valid on
     * the target and must not be killed by splitting 900 ms between radios. */
    unlink(g_exec_log);
    setenv("OPCD_NXP_EXEC_DELAY_MS", "500", 1);
    memset(&cfg, 0, sizeof cfg);
    cfg.station_type = OPC_STATION_DUAL;
    cfg.priority_ch = OPC_PRIORITY_CH_UNSET;
    cfg.wlan1.mode = OPC_WLAN_MODE_11AX;
    cfg.wlan1.bandwidth = OPC_BANDWIDTH_80;
    cfg.wlan1.scan_band = OPC_SCAN_BAND_5GHZ;
    opc_scan_list_set_channel(cfg.wlan1.scan_chlist, OPC_SCAN_BAND_5GHZ, 40);
    cfg.wlan2.mode = OPC_WLAN_MODE_11AC;
    cfg.wlan2.bandwidth = OPC_BANDWIDTH_40;
    cfg.wlan2.scan_band = OPC_SCAN_BAND_5GHZ;
    opc_scan_list_set_channel(cfg.wlan2.scan_chlist, OPC_SCAN_BAND_5GHZ, 36);
    ASSERT(ops->apply_radio_config(&cfg) == 0,
           "valid dual apply allows each sequential helper its bounded window");
    ASSERT(read_log(log, sizeof log) == 0, "captured both dual helper invocations");
    ASSERT(strcmp(log,
                  "path=/proc/self/exe|opc_wlan_apply.sh|mlan0|freq|5200|mode|ax|bw|80\n"
                  "path=/proc/self/exe|opc_wlan_apply.sh|mlan1|freq|5180|mode|ac|bw|40\n") == 0,
           "dual apply passes exact interface, frequency, mode and bandwidth argv");

    /* A bounded secondary failure must be reached after a valid 500 ms mlan0
     * call and returned to the handler for its existing last-good reapply. */
    unlink(g_exec_log);
    setenv("OPCD_NXP_FAIL_MLAN1", "1", 1);
    ASSERT(ops->apply_radio_config(&cfg) == -EPROTO,
           "dual secondary helper failure is returned after mlan0 succeeds");
    unsetenv("OPCD_NXP_FAIL_MLAN1");
    unsetenv("OPCD_NXP_EXEC_DELAY_MS");
    ASSERT(read_log(log, sizeof log) == 0,
           "secondary failure still records both bounded helper calls");
    ASSERT(strcmp(log,
                  "path=/proc/self/exe|opc_wlan_apply.sh|mlan0|freq|5200|mode|ax|bw|80\n"
                  "path=/proc/self/exe|opc_wlan_apply.sh|mlan1|freq|5180|mode|ac|bw|40\n") == 0,
           "dual secondary failure occurs after reaching mlan1");

    /* Capability rejection is pure and must also be enforced defensively by
     * apply itself before the first interface changes. */
    unlink(g_exec_log);
    cfg.wlan1.bandwidth = OPC_BANDWIDTH_160;
    ASSERT(ops->validate_radio_config(&cfg) == OPC_ERR_RADIO_BW,
           "NXP rejects unsupported 160 MHz with field-attributed error");
    ASSERT(ops->apply_radio_config(&cfg) != 0,
           "NXP apply defensively rejects unsupported bandwidth");
    ASSERT(access(g_exec_log, F_OK) != 0,
           "unsupported bandwidth causes no helper side effect");

    memset(&cfg, 0, sizeof cfg);
    cfg.station_type = OPC_STATION_DUAL;
    cfg.wlan1.mode = OPC_WLAN_MODE_11AC;
    cfg.wlan1.bandwidth = OPC_BANDWIDTH_80;
    cfg.wlan1.scan_band = OPC_SCAN_BAND_5GHZ;
    cfg.wlan2.mode = OPC_WLAN_MODE_11AX;
    cfg.wlan2.bandwidth = OPC_BANDWIDTH_80;
    cfg.wlan2.scan_band = OPC_SCAN_BAND_5GHZ;
    ASSERT(ops->validate_radio_config(&cfg) == OPC_ERR_RADIO_MODE,
           "NXP rejects mlan1 11ax with mode error");
    ASSERT(ops->apply_radio_config(&cfg) != 0,
           "NXP apply defensively rejects mlan1 11ax");
    ASSERT(access(g_exec_log, F_OK) != 0,
           "unsupported mlan1 mode rejects before mlan0 changes");

    /* #186: a legacy mode paired with the other band's SCAN Band is an
     * unsupported Mode combination (0x0013) in both directions; an unset band
     * leaves 11a unrestricted. */
    struct band_case {
        uint8_t mode;
        uint16_t band;
        uint16_t want;
        const char *label;
    } band_cases[] = {
        { OPC_WLAN_MODE_11A, OPC_SCAN_BAND_2_4GHZ, OPC_ERR_RADIO_MODE,
          "#186: 11a + 2.4 GHz SCAN Band is rejected with 0x0013" },
        { OPC_WLAN_MODE_11A, OPC_SCAN_BAND_5GHZ, 0,
          "#186: 11a + 5 GHz SCAN Band stays accepted" },
        { OPC_WLAN_MODE_11A, OPC_SCAN_BAND_UNSET, 0,
          "#186: 11a + unset SCAN Band stays accepted" },
        { OPC_WLAN_MODE_11B, OPC_SCAN_BAND_5GHZ, OPC_ERR_RADIO_MODE,
          "#186: 11b + 5 GHz SCAN Band stays rejected with 0x0013" },
        { OPC_WLAN_MODE_11G, OPC_SCAN_BAND_5GHZ, OPC_ERR_RADIO_MODE,
          "#186: 11g + 5 GHz SCAN Band stays rejected with 0x0013" },
    };
    for (size_t i = 0; i < sizeof band_cases / sizeof band_cases[0]; i++) {
        memset(&cfg, 0, sizeof cfg);
        cfg.station_type = OPC_STATION_SINGLE;
        cfg.wlan1.mode = band_cases[i].mode;
        cfg.wlan1.bandwidth = OPC_BANDWIDTH_20;
        cfg.wlan1.scan_band = band_cases[i].band;
        ASSERT(ops->validate_radio_config(&cfg) == band_cases[i].want,
               band_cases[i].label);
    }
    unlink(g_exec_log);
    memset(&cfg, 0, sizeof cfg);
    cfg.station_type = OPC_STATION_SINGLE;
    cfg.wlan1.mode = OPC_WLAN_MODE_11A;
    cfg.wlan1.bandwidth = OPC_BANDWIDTH_20;
    cfg.wlan1.scan_band = OPC_SCAN_BAND_2_4GHZ;
    opc_scan_list_set_channel(cfg.wlan1.scan_chlist, OPC_SCAN_BAND_2_4GHZ, 6);
    ASSERT(ops->apply_radio_config(&cfg) != 0,
           "#186: apply defensively rejects 11a + 2.4 GHz");
    ASSERT(access(g_exec_log, F_OK) != 0,
           "#186: 11a + 2.4 GHz reaches no helper");

    /* A timed-out helper may have spawned a driver subprocess. The platform
     * must signal the entire process group so the helper's TERM cleanup runs
     * and no descendant continues mutating after the response deadline. */
    unlink(g_cleanup_marker);
    unlink(g_survivor_marker);
    unlink(g_helper_pid_marker);
    setenv("OPCD_TEST_REAL_EXEC", "1", 1);
    setenv("OPCD_NXP_TIMEOUT_HELPER", "1", 1);
    sigset_t opcd_signal_mask, saved_parent_mask, blocked_parent_mask;
    sigemptyset(&opcd_signal_mask);
    sigaddset(&opcd_signal_mask, SIGINT);
    sigaddset(&opcd_signal_mask, SIGTERM);
    ASSERT(sigprocmask(SIG_BLOCK, &opcd_signal_mask, &saved_parent_mask) == 0,
           "test fixture blocks the same signals as opcd signalfd");
    ASSERT(sigprocmask(SIG_SETMASK, NULL, &blocked_parent_mask) == 0,
           "test fixture reads the blocked parent signal mask");
    memset(&cfg, 0, sizeof cfg);
    cfg.station_type = OPC_STATION_SINGLE;
    cfg.wlan1.mode = OPC_WLAN_MODE_11AC;
    cfg.wlan1.bandwidth = OPC_BANDWIDTH_80;
    cfg.wlan1.scan_band = OPC_SCAN_BAND_5GHZ;
    opc_scan_list_set_channel(cfg.wlan1.scan_chlist, OPC_SCAN_BAND_5GHZ, 36);
    ASSERT(ops->apply_radio_config(&cfg) == -ETIMEDOUT,
           "timed-out helper reports ETIMEDOUT");
    sigset_t parent_mask_after_apply;
    ASSERT(sigprocmask(SIG_SETMASK, NULL, &parent_mask_after_apply) == 0 &&
           signal_masks_equal(&parent_mask_after_apply, &blocked_parent_mask),
           "bounded apply leaves the parent signal mask unchanged");
    unsetenv("OPCD_NXP_TIMEOUT_HELPER");
    unsetenv("OPCD_TEST_REAL_EXEC");
    ASSERT(access(g_cleanup_marker, F_OK) == 0,
           "SIGTERM reaches helper and runs cleanup before hard deadline");
    pid_t helper_pid = read_helper_pid();
    int helper_status = 0;
    errno = 0;
    ASSERT(helper_pid > 0 && waitpid(helper_pid, &helper_status, WNOHANG) == -1 &&
           errno == ECHILD,
           "TERM-completed helper was already reaped before apply returned");
    struct timespec settle = { .tv_sec = 0, .tv_nsec = 400 * 1000 * 1000L };
    nanosleep(&settle, NULL);
    ASSERT(access(g_survivor_marker, F_OK) != 0,
           "timed-out helper descendant cannot outlive the process group");

    /* The hard-deadline branch must contain a helper and descendant that both
     * ignore TERM, without extending the 900 ms single-radio budget. */
    unlink(g_cleanup_marker);
    unlink(g_survivor_marker);
    unlink(g_helper_pid_marker);
    setenv("OPCD_TEST_REAL_EXEC", "1", 1);
    setenv("OPCD_NXP_TIMEOUT_HELPER", "1", 1);
    setenv("OPCD_NXP_TIMEOUT_IGNORE_TERM", "1", 1);
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    ASSERT(ops->apply_radio_config(&cfg) == -ETIMEDOUT,
           "TERM-ignoring helper is hard-killed at deadline");
    clock_gettime(CLOCK_MONOTONIC, &after);
    long elapsed_ms = (after.tv_sec - before.tv_sec) * 1000L +
                      (after.tv_nsec - before.tv_nsec) / 1000000L;
    unsetenv("OPCD_NXP_TIMEOUT_IGNORE_TERM");
    unsetenv("OPCD_NXP_TIMEOUT_HELPER");
    unsetenv("OPCD_TEST_REAL_EXEC");
    ASSERT(elapsed_ms <= 1050,
           "TERM grace stays inside the existing 900 ms budget plus scheduler slack");
    helper_pid = read_helper_pid();
    errno = 0;
    ASSERT(helper_pid > 0 && waitpid(helper_pid, &helper_status, WNOHANG) == -1 &&
           errno == ECHILD,
           "hard-killed helper was reaped before apply returned");
    nanosleep(&settle, NULL);
    ASSERT(access(g_survivor_marker, F_OK) != 0,
           "SIGKILL reaches TERM-ignoring descendants at hard deadline");
    ASSERT(sigprocmask(SIG_SETMASK, &saved_parent_mask, NULL) == 0,
           "test fixture restores the parent signal mask");

    unlink(g_exec_log);
    unlink(g_cleanup_marker);
    unlink(g_survivor_marker);
    unlink(g_helper_pid_marker);
    puts("test_platform_nxp_radio: ok");
    return 0;
}
