/*
 * vhlctl — VHL-side UDP/IP CLI simulator (scaffold, Phase 0).
 *
 * Phase 3 will fill in:
 *   - argparse + subcommand dispatch
 *     login | logout | basic-info | device-info
 *     set-password | set-ip-list | change-ip
 *     set-radio | set-indication | reset | listen
 *   - request/response round-trip helpers built on libopcproto.a
 */

#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    fprintf(stderr, "vhlctl: scaffold only — Phase 3 not implemented yet\n");
    return 0;
}
