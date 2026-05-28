/*
 * Unit tests for opcd_inventory_load().
 *
 * Writes a JSON file to /tmp, runs the loader, asserts on the global
 * inventory contents via opcd_inventory(). The previous-value preservation
 * behaviour (missing keys keep the prior value) is also exercised.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../inventory.h"

static int failures = 0;

#define ASSERT(cond, label) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", label); failures++; }         \
    else         { fprintf(stdout, "PASS %s\n", label); }                     \
} while (0)

/* Write `content` to a fresh temp file; caller frees the returned path. */
static char *write_temp(const char *content)
{
    static int counter;
    char *path = malloc(64);
    if (!path) return NULL;
    snprintf(path, 64, "/tmp/test_inventory_%d_%d.json",
             (int)getpid(), counter++);
    FILE *f = fopen(path, "w");
    if (!f) { free(path); return NULL; }
    fputs(content, f);
    fclose(f);
    return path;
}

int main(void)
{
    /* --- Case 1: full inventory load. -------------------------------- */
    const char *full =
        "{\n"
        "  \"vendor_code\":      \"0x00902CFB\",\n"
        "  \"product_code\":     \"0xFE03\",\n"
        "  \"product_subcode\":  \"0x0001\",\n"
        "  \"hardware_version\": \"HW-1.0.0\",\n"
        "  \"serial_number\":    \"SN-2026-0001\",\n"
        "  \"manufacture_date\": \"2026-02-28\",\n"
        "  \"shipment_date\":    \"2026-03-15\",\n"
        "  \"ieee_11r\":  1,\n"
        "  \"ieee_11ai\": 0,\n"
        "  \"ieee_11k\":  1,\n"
        "  \"ieee_11v\":  1\n"
        "}\n";
    char *p1 = write_temp(full);
    ASSERT(p1 != NULL, "temp file 1 created");
    ASSERT(opcd_inventory_load(p1) == 0, "full load returns 0");

    const opcd_inventory_t *inv = opcd_inventory();
    ASSERT(inv->vendor_code     == 0x00902CFBu, "vendor_code");
    ASSERT(inv->product_code    == 0xFE03u,     "product_code");
    ASSERT(inv->product_subcode == 0x0001u,     "product_subcode");
    ASSERT(strcmp(inv->hardware_version, "HW-1.0.0") == 0,    "hw_version");
    ASSERT(strcmp(inv->serial_number,    "SN-2026-0001") == 0, "serial");
    ASSERT(inv->manufacture_date.year  == 2026 &&
           inv->manufacture_date.month == 2    &&
           inv->manufacture_date.day   == 28,  "manufacture_date");
    ASSERT(inv->shipment_date.year  == 2026 &&
           inv->shipment_date.month == 3    &&
           inv->shipment_date.day   == 15,    "shipment_date");
    ASSERT(inv->ieee_11r  == 1, "ieee_11r");
    ASSERT(inv->ieee_11ai == 0, "ieee_11ai");
    ASSERT(inv->ieee_11k  == 1, "ieee_11k");
    ASSERT(inv->ieee_11v  == 1, "ieee_11v");
    unlink(p1);
    free(p1);

    /* --- Case 2: missing-key preservation. --------------------------- */
    const char *partial =
        "{\n"
        "  \"vendor_code\": \"0xCAFEBABE\",\n"
        "  \"ieee_11r\":    0\n"
        "}\n";
    char *p2 = write_temp(partial);
    ASSERT(p2 != NULL, "temp file 2 created");
    ASSERT(opcd_inventory_load(p2) == 0, "partial load returns 0");
    inv = opcd_inventory();
    ASSERT(inv->vendor_code == 0xCAFEBABEu, "partial: vendor_code overwritten");
    ASSERT(inv->ieee_11r    == 0,           "partial: 11r overwritten");
    /* Previously loaded fields must survive: */
    ASSERT(inv->product_code    == 0xFE03u, "partial: product_code preserved");
    ASSERT(inv->product_subcode == 0x0001u, "partial: product_subcode preserved");
    ASSERT(strcmp(inv->hardware_version, "HW-1.0.0") == 0,
           "partial: hw_version preserved");
    ASSERT(inv->ieee_11k == 1, "partial: 11k preserved");
    unlink(p2);
    free(p2);

    /* --- Case 3: missing file → returns -errno, inventory unchanged. */
    /* Capture current vendor_code to verify no mutation. */
    uint32_t before = opcd_inventory()->vendor_code;
    ASSERT(opcd_inventory_load("/tmp/__does_not_exist__.json") != 0,
           "missing file returns non-zero");
    ASSERT(opcd_inventory()->vendor_code == before,
           "missing file leaves inventory unchanged");

    /* --- Case 4: zero recognised fields → -EINVAL. ------------------- */
    const char *junk = "{ \"unrelated\": \"nothing\" }\n";
    char *p4 = write_temp(junk);
    ASSERT(p4 != NULL, "temp file 4 created");
    ASSERT(opcd_inventory_load(p4) != 0,
           "no-recognised-fields returns non-zero");
    /* And again, inventory is unchanged. */
    ASSERT(opcd_inventory()->vendor_code == before,
           "junk file leaves inventory unchanged");
    unlink(p4);
    free(p4);

    /* --- Case 5: malformed date is silently ignored. ----------------- */
    const char *bad_date =
        "{\n"
        "  \"vendor_code\":      \"0x11223344\",\n"
        "  \"manufacture_date\": \"not-a-date\"\n"
        "}\n";
    char *p5 = write_temp(bad_date);
    ASSERT(p5 != NULL, "temp file 5 created");
    /* vendor_code recovers, manufacture_date does not — counts as 1
     * recovery → load returns 0, but date field preserves previous value. */
    ASSERT(opcd_inventory_load(p5) == 0, "bad date load still succeeds");
    inv = opcd_inventory();
    ASSERT(inv->vendor_code == 0x11223344u, "bad date: vendor recovered");
    ASSERT(inv->manufacture_date.year  == 2026 &&
           inv->manufacture_date.month == 2,
           "bad date: prior manufacture_date preserved");
    unlink(p5);
    free(p5);

    if (failures == 0) {
        fprintf(stdout, "all inventory tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
