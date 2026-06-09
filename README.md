# wlan-opc

OPC-side (wireless-board) UDP/IP control plane for the VHL ↔ wireless-board
common-control protocol (spec Rev1.00, 2026-05-25), plus a companion VHL-side
CLI simulator.

Designed to ship inside the `wlan-package` Debian package as
`/usr/local/opc/{bin/opcd, bin/vhlctl, etc/..., opcd.service}` on
NXP88W9098 + i.MX8MM ARM64 targets.

## Layout

- `protocol/` — wire codec, command/indication IDs, error causes (`libopcproto.a`)
- `opcd/` — OPC-side daemon, systemd unit (Phase 2)
- `vhlctl/` — VHL-side CLI simulator (Phase 3)

## Build

Per-architecture out-of-tree build — native (host) and arm64 (target) binaries
coexist under `build/native/` and `build/arm64/`, so neither clobbers the other:

```
make            # = arm64 cross-build (target deploy)  -> build/arm64/
make native     # host build (sanity checks)           -> build/native/
make both       # both, side by side
make check      # native build + host unit tests
make clean      # remove build/
```

`PLATFORM` (stub|nxp) is orthogonal to arch and only affects opcd (vhlctl is
platform-independent); cross-deploy uses `make PLATFORM=nxp`.

## Install layout (target)

```
/usr/local/opc/
├── bin/opcd
├── bin/vhlctl
├── etc/{opc.conf, password, iplist.cfg, radio.conf, temp/}
└── opcd.service   ← symlinked from /etc/systemd/system/opcd.service
```

## Status

- Phase 0 — scaffold (this commit)
- Phase 1 — protocol codec
- Phase 2 — opcd daemon
- Phase 3 — vhlctl CLI
- Phase 4 — package integration
- Phase 5 — manual run-through

See the parent `wlan-package/docs/seed.yaml` for the full specification, the
acceptance criteria, and `wlan-package/docs/proto-todo.md` for spec ambiguities
tracked back to their call sites.
