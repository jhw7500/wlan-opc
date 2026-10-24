/* Host unit test for the IP address list file (#194, communication spec 6.5):
 * the Figure 4-5 text grammar (iplist_text.c) and the start-up load
 * (opcd_iplist_load) — opcd's own copy read leniently with the one-time
 * conversion of the pre-#194 binary, then the customer file read strictly:
 * a valid file replaces the whole list, a file with any bad line is not
 * applied and the previous list stays. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../handler.h"
#include "../iplist_text.h"
#include "../opcd_state.h"
#include "../store.h"

static int failures = 0;
#define ASSERT(cond, label) do {                          \
    if (cond) { printf("PASS %s\n", (label)); }           \
    else      { printf("FAIL %s\n", (label)); failures++; }\
} while (0)

static char dir[64], own[128], user[128], rejected[160], legacy[128], migrated[160], record[160];

static void put_file(const char *path, const char *data, size_t len)
{
    unlink(path);
    if (opc_store_write_atomic(path, data, len, 0644) != 0) { perror(path); exit(2); }
}

static bool file_is(const char *path, const char *data, size_t len)
{
    static char buf[OPC_IPLIST_IMPORT_MAX + 2];
    ssize_t n = opc_store_read_all(path, buf, sizeof buf);
    return n == (ssize_t)len && memcmp(buf, data, len) == 0;
}

/* Spec §4.3.6 Figure 4-5, lists 1 and 2. */
static const char FIG45[] =
    "IPAddress1=172.17.101.17\n"
    "IPSubnetMask1=255.255.192.0\n"
    "IPDefaultGateway1=172.17.65.254\n"
    "WL54EssId1=\"XXXX_LV10\"\n"
    "NTPServer1=172.17.2.1\n"
    "IPAddress2=172.16.101.17\n"
    "IPSubnetMask2=255.255.192.0\n"
    "IPDefaultGateway2=172.16.65.254\n"
    "WL54EssId2=\"XXXX_LV30\"\n"
    "NTPServer2=172.16.2.1\n";
#define FIG45_IS(path) file_is((path), FIG45, sizeof FIG45 - 1)

static opcd_state_t st;
static opcd_iplist_load_t own_verdict;

static opcd_iplist_user_t start(void)
{
    memset(&st, 0, sizeof st);
    st.paths.ip_list = own;
    st.paths.ip_list_user = user;
    st.paths.ip_list_user_last = record;
    st.paths.ip_list_legacy = legacy;
    return opcd_iplist_load(&st, &own_verdict);
}

static bool list_is_fig45(void)
{
    return st.ip_list.present[0] && st.ip_list.present[1] && !st.ip_list.present[2] &&
           st.ip_list.slots[0].ip_address == 0xAC116511u &&
           st.ip_list.slots[0].subnet_mask == 0xFFFFC000u &&
           st.ip_list.slots[0].default_gateway == 0xAC1141FEu &&
           st.ip_list.slots[0].ntp_server == 0xAC110201u &&
           strcmp(st.ip_list.slots[1].essid, "XXXX_LV30") == 0 &&
           st.ip_list.slots[1].list_number == 2;
}

/* The customer replaced the file with `text`: applied as a whole. */
static void accept_text(const char *text, size_t len, const char *label)
{
    unlink(own); unlink(rejected); unlink(record);
    put_file(user, text, len);
    char l[200];
    snprintf(l, sizeof l, "applied: %s", label);
    ASSERT(start() == OPCD_IPLIST_USER_APPLIED && list_is_fig45() && FIG45_IS(own) &&
           access(rejected, F_OK) != 0 && file_is(record, text, len), l);
}
#define ACCEPT(text, label) accept_text((text), sizeof(text) - 1, (label))

/* The customer replaced the file with a bad `text`: nothing of it applies,
 * the previous list (own copy = Figure 4-5) stays, the file is kept as
 * *.rejected and rewritten with the list in effect. */
static void reject_text(const char *text, size_t len, const char *label)
{
    put_file(own, FIG45, sizeof FIG45 - 1);
    unlink(rejected); unlink(record);
    put_file(user, text, len);
    char l[200];
    snprintf(l, sizeof l, "rejected: %s", label);
    ASSERT(start() == OPCD_IPLIST_USER_REJECTED && list_is_fig45() && FIG45_IS(own) &&
           FIG45_IS(user) && FIG45_IS(record) && file_is(rejected, text, len), l);
}
#define REJECT(text, label) reject_text((text), sizeof(text) - 1, (label))

int main(void)
{
    snprintf(dir, sizeof dir, "/tmp/opcd_iplist_XXXXXX");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    snprintf(own, sizeof own, "%s/own.txt", dir);
    snprintf(user, sizeof user, "%s/iplist.txt", dir);
    snprintf(rejected, sizeof rejected, "%s/iplist.txt.rejected", dir);
    snprintf(legacy, sizeof legacy, "%s/iplist.cfg", dir);
    snprintf(migrated, sizeof migrated, "%s/iplist.cfg.migrated", dir);
    snprintf(record, sizeof record, "%s/iplist.user.last", dir);

    /* ---- the customer file replaces the whole list ---- */
    ACCEPT(FIG45, "Figure 4-5 as is");
    ACCEPT("\xEF\xBB\xBF"
           "\r\n"
           "NTPServer2=172.16.2.1\r\n"
           "IPAddress2=172.16.101.17\r\n"
           "IPSubnetMask2=255.255.192.0\r\n"
           "IPDefaultGateway2=172.16.65.254\r\n"
           "WL54EssId2=\"XXXX_LV30\"\r\n"
           "\n"
           "WL54EssId1=\"XXXX_LV10\"\r\n"
           "IPAddress1=172.17.101.17\r\n"
           "IPSubnetMask1=255.255.192.0\r\n"
           "IPDefaultGateway1=172.17.65.254\r\n"
           "NTPServer1=172.17.2.1",
           "UTF-8 BOM, CRLF, blank lines, any key order, no final line break");
    ASSERT(start() == OPCD_IPLIST_USER_KEPT && list_is_fig45() && FIG45_IS(own),
           "the next start with the same file keeps the list (the file is as recorded)");

    /* ---- Tribunal A-R1-001: an unreplaced customer file never undoes opcd's copy ---- */
    {
        static const char OLDER[] =
            "IPAddress9=10.0.9.5\nIPSubnetMask9=255.255.255.0\nIPDefaultGateway9=0.0.0.0\n"
            "WL54EssId9=\"older\"\nNTPServer9=0.0.0.0\n";
        put_file(own, FIG45, sizeof FIG45 - 1);              /* a commit that was answered OK */
        put_file(user, OLDER, sizeof OLDER - 1);             /* its customer-file copy failed: */
        put_file(record, OLDER, sizeof OLDER - 1);           /* the file is what opcd wrote before */
        ASSERT(start() == OPCD_IPLIST_USER_KEPT && list_is_fig45() && FIG45_IS(own) &&
               FIG45_IS(user) && FIG45_IS(record) && !st.ip_list.present[8],
               "unreplaced older customer file: opcd's copy stays and is written back");
        put_file(own, FIG45, sizeof FIG45 - 1);
        put_file(user, OLDER, sizeof OLDER - 1);             /* the customer replaced the file */
        put_file(record, FIG45, sizeof FIG45 - 1);
        ASSERT(start() == OPCD_IPLIST_USER_APPLIED && st.ip_list.present[8] && !st.ip_list.present[0] &&
               file_is(record, OLDER, sizeof OLDER - 1),
               "replaced customer file (differs from the record): applied and recorded");
        put_file(user, OLDER, sizeof OLDER - 1);
        unlink(record);
        put_file(own, FIG45, sizeof FIG45 - 1);
        ASSERT(start() == OPCD_IPLIST_USER_APPLIED && st.ip_list.present[8],
               "no record yet: a valid customer file is applied");
    }

    put_file(own, FIG45, sizeof FIG45 - 1);
    put_file(user, "", 0);
    ASSERT(start() == OPCD_IPLIST_USER_APPLIED && !st.ip_list.present[0] && !st.ip_list.present[1] &&
           file_is(own, "", 0), "an empty file is an empty list (decision 2026-10-24)");
    put_file(own, FIG45, sizeof FIG45 - 1);
    put_file(user, "\n\r\n\n", 4);
    ASSERT(start() == OPCD_IPLIST_USER_APPLIED && !st.ip_list.present[0] && file_is(own, "", 0),
           "a file of blank lines is an empty list");

    static const char UNSET[] =
        "IPAddress3=0.0.0.0\nIPSubnetMask3=0.0.0.0\nIPDefaultGateway3=0.0.0.0\nWL54EssId3=\"\"\nNTPServer3=0.0.0.0\n"
        "IPAddress4=10.0.0.5\nIPSubnetMask4=255.255.255.0\nIPDefaultGateway4=0.0.0.0\nWL54EssId4=\"\"\nNTPServer4=0.0.0.0\n";
    put_file(own, FIG45, sizeof FIG45 - 1);
    put_file(user, UNSET, sizeof UNSET - 1);
    ASSERT(start() == OPCD_IPLIST_USER_APPLIED && !st.ip_list.present[0] && !st.ip_list.present[2] &&
           st.ip_list.present[3] && st.ip_list.slots[3].default_gateway == 0 &&
           st.ip_list.slots[3].essid[0] == '\0',
           "numbers not in the file are empty; IP 0.0.0.0 is an empty list (#163)");

    /* ---- a bad file: not applied at all, the previous list stays ---- */
    REJECT("IPAdress1=10.0.0.5\n", "misspelled key");
    REJECT("# comment\n", "comment line");
    REJECT("IPAddress1=10.0.0.5\nIPSubnetMask1=255.255.255.0\nIPDefaultGateway1=0.0.0.0\n"
           "WL54EssId1=\"a\"\n", "list without NTPServer");
    REJECT("IPAddress1=10.0.0.5\nIPAddress1=10.0.0.6\n", "key repeated");
    REJECT("IPAddress0=10.0.0.5\n", "number 0");
    REJECT("IPAddress129=10.0.0.5\n", "number 129");
    REJECT("IPAddress01=10.0.0.5\n", "number with a leading zero");
    REJECT("IPAddress1=256.0.0.5\n", "octet over 255");
    REJECT("IPAddress1=10.0.5\n", "three octets");
    REJECT("IPAddress1=10.0.0.5 \n", "trailing space");
    REJECT("IPAddress1=10.0.0.05\n", "octet with a leading zero");
    REJECT("WL54EssId1=abc\n", "ESSID without quotes");
    REJECT("WL54EssId1=\"abcdefghijklmnopqrstuvwxyz0123456\"\n", "ESSID of 32 characters");
    REJECT("WL54EssId1=\"ab\"c\"\n", "quote inside the ESSID");
    REJECT("IPAddress1=10.0.0.5\nIPSubnetMask1=255.255.255.0\nIPDefaultGateway1=10.0.1.1\n"
           "WL54EssId1=\"a\"\nNTPServer1=0.0.0.0\n", "gateway outside the subnet (0x0013)");
    REJECT("IPAddress1=10.0.0.5\nIPSubnetMask1=255.255.255.0\nIPDefaultGateway1=0.0.0.0\n"
           "WL54EssId1=\"a b\"\nNTPServer1=0.0.0.0\n", "ESSID character outside the allowed set (0x0015)");
    REJECT("IPAddress1=10.0.0.0\nIPSubnetMask1=255.255.255.0\nIPDefaultGateway1=0.0.0.0\n"
           "WL54EssId1=\"a\"\nNTPServer1=0.0.0.0\n", "network address as the device IP (0x0011)");
    REJECT("IPAddress1=172.17.101.17\nIPSubnetMask1=255.255.192.0\nIPDefaultGateway1=172.17.65.254\n"
           "WL54EssId1=\"ok\"\nNTPServer1=172.17.2.1\nIPAddress2=10.0.0.5\n",
           "one good list and one incomplete list: nothing applies");
    {
        static const char nul[] = "IPAddress1=10.0.0.5\nIPSub\0netMask1=255.255.255.0\n";
        reject_text(nul, sizeof nul - 1, "NUL byte in a line");
        static char big[OPC_IPLIST_IMPORT_MAX + 1];
        memset(big, '\n', sizeof big);
        reject_text(big, sizeof big, "file over 64 KiB");
    }

    /* ---- the customer file missing or unusable ---- */
    put_file(own, FIG45, sizeof FIG45 - 1);
    unlink(user); unlink(record);
    ASSERT(start() == OPCD_IPLIST_USER_CREATED && list_is_fig45() && FIG45_IS(user) && FIG45_IS(record),
           "missing file: previous list kept, written back and recorded");
    {
        char target[160];
        snprintf(target, sizeof target, "%s/target.txt", dir);
        put_file(target, "", 0);
        unlink(user);
        ASSERT(symlink(target, user) == 0 && start() == OPCD_IPLIST_USER_UNUSABLE && list_is_fig45() &&
               file_is(target, "", 0), "a link is not followed: previous list kept, target untouched");
        unlink(user);
        unlink(target);
        ASSERT(mkdir(user, 0700) == 0 && start() == OPCD_IPLIST_USER_UNUSABLE && list_is_fig45(),
               "a directory is not a list file: previous list kept");
        rmdir(user);
    }

    /* ---- opcd's own copy: lenient ---- */
    {
        static const char STORED[] =
            "IPAddress1=10.0.0.5\nIPSubnetMask1=255.255.255.0\nIPDefaultGateway1=0.0.0.0\n"
            "WL54EssId1=\"ok\"\nNTPServer1=0.0.0.0\n"
            "IPAddress2=10.0.0.6\nIPSubnetMask2=255.255.255.0\n"                      /* keys missing */
            "IPAddress3=10.0.0.7\nIPSubnetMask3=255.255.255.0\nIPDefaultGateway3=10.9.9.9\n"
            "WL54EssId3=\"x\"\nNTPServer3=0.0.0.0\n"                                   /* bad gateway */
            "garbage\n"
            "IPAddress4=10.0.0.8\nIPSubnetMask4=255.255.255.0\nIPDefaultGateway4=0.0.0.0\n"
            "WL54EssId4=\"four\"\nNTPServer4=0.0.0.0";
        put_file(own, STORED, sizeof STORED - 1);
        unlink(user); unlink(legacy);
        ASSERT(start() == OPCD_IPLIST_USER_CREATED && own_verdict == OPCD_IPLIST_LOAD_TEXT &&
               st.ip_list.present[0] && !st.ip_list.present[1] && !st.ip_list.present[2] &&
               st.ip_list.present[3] && strcmp(st.ip_list.slots[3].essid, "four") == 0,
               "own copy: only the broken lists are dropped, the rest is kept");
    }
    {
        static char huge[OPC_IPLIST_TEXT_MAX + 1];
        memset(huge, '\n', sizeof huge);
        put_file(own, huge, sizeof huge);
        unlink(user);
        ASSERT(start() == OPCD_IPLIST_USER_CREATED && own_verdict == OPCD_IPLIST_LOAD_TOO_LARGE &&
               !st.ip_list.present[0], "own copy over the limit: empty list");
    }

    /* ---- upgrade: one-time conversion of the pre-#194 binary ---- */
    opcd_ip_list_t bin;
    memset(&bin, 0, sizeof bin);
    bin.slots[0].list_number = 1;
    bin.slots[0].boundary_flag = OPC_LIST_BOUNDARY_START_END;
    bin.slots[0].ip_address = 0xC0A80165u;
    bin.slots[0].subnet_mask = 0xFFFFFF00u;
    bin.slots[0].default_gateway = 0xC0A801FEu;
    snprintf(bin.slots[0].essid, sizeof bin.slots[0].essid, "site-A");
    bin.present[0] = 1;
    bin.slots[6] = bin.slots[0];
    bin.slots[6].subnet_mask = 0xFFFF00FFu;   /* not contiguous: rejected now */
    bin.present[6] = 1;
    static char want[OPC_IPLIST_TEXT_MAX];
    static char buf[OPC_IPLIST_TEXT_MAX + 64];

    unlink(own); unlink(user); unlink(migrated);
    put_file(legacy, (const char *)&bin, sizeof bin);
    ASSERT(start() == OPCD_IPLIST_USER_CREATED && own_verdict == OPCD_IPLIST_LOAD_MIGRATED &&
           st.ip_list.present[0] && !st.ip_list.present[6] &&
           strcmp(st.ip_list.slots[0].essid, "site-A") == 0,
           "upgrade: valid binary lists kept, an invalid one dropped");
    {
        ssize_t wn = opc_iplist_text_format(&st.ip_list, want, sizeof want);
        ASSERT(wn > 0 && file_is(own, want, (size_t)wn) && file_is(user, want, (size_t)wn),
               "upgrade: the list is written to opcd's copy and the customer file");
        ASSERT(access(legacy, F_OK) != 0 &&
               opc_store_read_all(migrated, buf, sizeof buf) == (ssize_t)sizeof bin &&
               memcmp(buf, &bin, sizeof bin) == 0,
               "upgrade: the binary is renamed *.migrated, bytes intact");
    }
    ASSERT(start() == OPCD_IPLIST_USER_KEPT && own_verdict == OPCD_IPLIST_LOAD_TEXT &&
           st.ip_list.present[0] && !st.ip_list.present[6],
           "upgrade: the next start reads the text, no second conversion");

    unlink(own); unlink(user); unlink(migrated);
    put_file(legacy, (const char *)&bin, sizeof bin - 1);
    ASSERT(start() == OPCD_IPLIST_USER_CREATED && own_verdict == OPCD_IPLIST_LOAD_LEGACY_DISCARD &&
           !st.ip_list.present[0] && access(own, F_OK) != 0 && access(legacy, F_OK) == 0,
           "upgrade: a binary of the wrong size is discarded");

    {
        /* A binary older than opcd's text is stale: set aside unread. */
        struct timespec past[2] = { { 1000000, 0 }, { 1000000, 0 } };
        put_file(own, FIG45, sizeof FIG45 - 1);
        put_file(legacy, (const char *)&bin, sizeof bin);
        utimensat(AT_FDCWD, legacy, past, 0);
        unlink(migrated); unlink(user);
        ASSERT(start() == OPCD_IPLIST_USER_CREATED && own_verdict == OPCD_IPLIST_LOAD_TEXT &&
               list_is_fig45() && access(legacy, F_OK) != 0 && access(migrated, F_OK) == 0,
               "upgrade: a binary older than the text is only renamed");
        /* Tribunal A-R1-002: after a downgrade the older opcd saved the list in
         * the binary; re-upgrading converts that newer binary, and the customer
         * file left from before (as opcd wrote it) does not override it. */
        put_file(own, FIG45, sizeof FIG45 - 1);
        put_file(user, FIG45, sizeof FIG45 - 1);
        put_file(record, FIG45, sizeof FIG45 - 1);
        utimensat(AT_FDCWD, own, past, 0);
        put_file(legacy, (const char *)&bin, sizeof bin);
        unlink(migrated);
        ASSERT(start() == OPCD_IPLIST_USER_KEPT && own_verdict == OPCD_IPLIST_LOAD_MIGRATED &&
               st.ip_list.present[0] && st.ip_list.slots[0].ip_address == 0xC0A80165u &&
               !st.ip_list.present[1] && access(migrated, F_OK) == 0,
               "re-upgrade: a binary newer than the text is converted and wins");
        ssize_t wn = opc_iplist_text_format(&st.ip_list, want, sizeof want);
        ASSERT(wn > 0 && file_is(own, want, (size_t)wn) && file_is(user, want, (size_t)wn),
               "re-upgrade: both text files now hold the binary's list");
    }

    {
        char nodir[160];
        snprintf(nodir, sizeof nodir, "%s/missing/own.txt", dir);
        unlink(migrated); unlink(user);
        put_file(legacy, (const char *)&bin, sizeof bin);
        memset(&st, 0, sizeof st);
        st.paths.ip_list = nodir;
        st.paths.ip_list_legacy = legacy;
        (void)opcd_iplist_load(&st, &own_verdict);
        ASSERT(own_verdict == OPCD_IPLIST_LOAD_LEGACY_UNSAVED && st.ip_list.present[0] &&
               access(legacy, F_OK) == 0 && access(migrated, F_OK) != 0,
               "upgrade: a failed text write keeps the binary for the next start");
    }

    unlink(own); unlink(legacy); unlink(migrated); unlink(user); unlink(rejected); unlink(record);
    ASSERT(start() == OPCD_IPLIST_USER_CREATED && own_verdict == OPCD_IPLIST_LOAD_NONE &&
           !st.ip_list.present[0] && file_is(user, "", 0),
           "no file at all (factory reset): empty list and an empty customer file");

    /* ---- grammar: the largest list fits the store job ---- */
    {
        opcd_ip_list_t full;
        memset(&full, 0, sizeof full);
        for (unsigned i = 0; i < OPC_IPCFG_LIST_MAX_SLOTS; i++) {
            full.slots[i].ip_address = full.slots[i].subnet_mask = 0xFFFFFFFFu;
            full.slots[i].default_gateway = full.slots[i].ntp_server = 0xFFFFFFFFu;
            memset(full.slots[i].essid, 'x', OPC_ESSID_FIELD_LEN - 1);
            full.present[i] = 1;
        }
        ssize_t n = opc_iplist_text_format(&full, want, sizeof want);
        ASSERT(n == 21604 && opc_iplist_text_format(&full, want, 21604) == -1,
               "the longest possible list is 21,604 bytes and needs every one of them");
    }

    unlink(own); unlink(user); unlink(rejected); unlink(legacy); unlink(migrated); unlink(record);
    rmdir(dir);
    if (failures) { printf("test_iplist: %d failure(s)\n", failures); return 1; }
    printf("test_iplist: all passed\n");
    return 0;
}
