/*
 * Direct unit tests for opcd/json_util.c.
 *
 * The inventory and link.json tests exercise the parser indirectly. These
 * focus on the section variants — brace-depth tracking, prefix-collision
 * guards, and the string-literal skip that prevents JSON values containing
 * `{` / `}` (e.g. an SSID literal) from spoofing keys or unbalancing the
 * depth counter.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../json_util.h"

static int failures = 0;

#define ASSERT(cond, label) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; }         \
    else         { fprintf(stdout, "PASS %s\n", label); }                     \
} while (0)

int main(void)
{
    char buf[64];
    long ival = 0;

    /* --- Top-level helpers -------------------------------------------- */
    const char *flat = "{ \"name\": \"alpha\", \"count\": 42 }";

    ASSERT(opc_json_string(flat, "name", buf, sizeof buf) == 0 &&
           strcmp(buf, "alpha") == 0,
           "top-level string");
    ASSERT(opc_json_integer(flat, "count", &ival) == 0 && ival == 42,
           "top-level integer");
    ASSERT(opc_json_string(flat, "missing", buf, sizeof buf) != 0,
           "top-level missing key");
    ASSERT(opc_json_integer(flat, "missing", &ival) != 0,
           "top-level missing integer");

    /* Prefix collision: "name" must not match "nameless". */
    const char *prefix = "{ \"nameless\": \"x\", \"name\": \"target\" }";
    ASSERT(opc_json_string(prefix, "name", buf, sizeof buf) == 0 &&
           strcmp(buf, "target") == 0,
           "prefix collision (name vs nameless)");

    /* --- Section helpers ---------------------------------------------- */
    const char *nested =
        "{\n"
        "  \"info\":  { \"freq\": 5200, \"address\": \"00:11:22:33:44:55\" },\n"
        "  \"link\":  { \"address\": \"04:ba:d6:ec:0b:08\", \"signal\": -67 }\n"
        "}";

    ASSERT(opc_json_string_section(nested, "info", "address",
                                   buf, sizeof buf) == 0 &&
           strcmp(buf, "00:11:22:33:44:55") == 0,
           "section info.address");
    ASSERT(opc_json_string_section(nested, "link", "address",
                                   buf, sizeof buf) == 0 &&
           strcmp(buf, "04:ba:d6:ec:0b:08") == 0,
           "section link.address (disambiguates duplicate key)");
    ASSERT(opc_json_integer_section(nested, "info", "freq", &ival) == 0 &&
           ival == 5200,
           "section info.freq integer");
    ASSERT(opc_json_integer_section(nested, "link", "signal", &ival) == 0 &&
           ival == -67,
           "section link.signal negative integer");

    /* Missing key inside an existing section. */
    ASSERT(opc_json_string_section(nested, "info", "ssid",
                                   buf, sizeof buf) != 0,
           "section: key absent");

    /* Missing section. */
    ASSERT(opc_json_string_section(nested, "ghost", "address",
                                   buf, sizeof buf) != 0,
           "section: section absent");

    /* --- String-literal skip ------------------------------------------ */
    /* An SSID with a curly brace must not confuse brace-depth tracking;
     * the parser must still locate the `signal` key in the SAME section. */
    const char *with_brace =
        "{ \"link\": { \"essid\": \"weird{}name\", \"signal\": -55 } }";
    ASSERT(opc_json_integer_section(with_brace, "link", "signal", &ival) == 0 &&
           ival == -55,
           "string literal containing { } must not break depth tracking");

    /* And a `"signal": 0` pattern hidden inside a string value of the
     * preceding key must NOT be picked up as the signal value. */
    const char *spoof =
        "{ \"link\": { \"essid\": \"\\\"signal\\\": 99\", \"signal\": -42 } }";
    ASSERT(opc_json_integer_section(spoof, "link", "signal", &ival) == 0 &&
           ival == -42,
           "key match must not fire inside a string value");

    /* Escape sequence `\"` inside the string must be skipped intact. */
    const char *escapes =
        "{ \"info\": { \"comment\": \"contains \\\"quotes\\\"\", \"freq\": 2412 } }";
    ASSERT(opc_json_integer_section(escapes, "info", "freq", &ival) == 0 &&
           ival == 2412,
           "escaped quote inside string");

    /* --- Truncation ---------------------------------------------------- */
    char tiny[6];
    const char *long_val = "{ \"k\": \"abcdefghij\" }";
    ASSERT(opc_json_string(long_val, "k", tiny, sizeof tiny) == 0 &&
           strcmp(tiny, "abcde") == 0 &&
           tiny[5] == '\0',
           "truncation NUL-terminates");

    /* --- Malformed input ---------------------------------------------- */
    ASSERT(opc_json_string(NULL, "k", buf, sizeof buf)  == -EINVAL,
           "NULL json rejected");
    ASSERT(opc_json_string("{}", NULL, buf, sizeof buf) == -EINVAL,
           "NULL key rejected");
    ASSERT(opc_json_string("{}", "k", NULL, sizeof buf) == -EINVAL,
           "NULL out rejected");
    ASSERT(opc_json_string("{}", "k", buf, 0)           == -EINVAL,
           "zero cap rejected");

    if (failures == 0) {
        fprintf(stdout, "all json_util tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
