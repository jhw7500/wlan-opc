/*
 * opcd — OPC-side UDP/IP control daemon (scaffold, Phase 0).
 *
 * Phase 2 will fill in:
 *   - epoll loop + signalfd
 *   - UDP socket bound to /usr/local/opc/etc/opc.conf::udp_port (default 50607)
 *   - Login/Logout session with 5-minute idle auto-logout
 *   - 10 Request/Query handlers and atomic-rename persistent store
 *   - 7-kind Indication producer (event + period)
 */

#include <stdio.h>

int main(void)
{
    fprintf(stderr, "opcd: scaffold only — Phase 2 not implemented yet\n");
    return 0;
}
