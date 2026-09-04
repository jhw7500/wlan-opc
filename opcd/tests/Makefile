CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -I.. -I../../protocol
CFLAGS  += -pthread   # store_async worker thread (compile)

# Out-of-tree: test objects + binaries land under build/<arch>/opcd/tests/.
# Reuses the opcd objects already compiled into build/<arch>/opcd/ (so the
# tests link against the host build without dragging in platform_nxp.c) and
# the protocol library from build/<arch>/protocol/. Defaults to native.
BUILDROOT ?= $(abspath ../../build/native)
OPCDOBJ   := $(BUILDROOT)/opcd
PROTODIR  := $(BUILDROOT)/protocol
TESTDIR   := $(BUILDROOT)/opcd/tests

LDFLAGS += -L$(PROTODIR)
# Link flag too — cross-toolchains may treat the CFLAGS -pthread as
# compile-only. Applied to every test link line (all include $(LDFLAGS)).
LDFLAGS += -pthread
LDLIBS  += -lopcproto

OPCD_OBJS_INVENTORY := $(OPCDOBJ)/inventory.o $(OPCDOBJ)/json_util.o
OPCD_OBJS_NTP       := $(OPCDOBJ)/ntp_parse.o
OPCD_OBJS_JSON      := $(OPCDOBJ)/json_util.o
OPCD_OBJS_SNAPSHOT  := $(OPCDOBJ)/snapshot.o
# test_handler drives opcd_dispatch end-to-end, so it links the handler and
# everything it depends on, with the host-only stub platform backend.
OPCD_OBJS_HANDLER   := $(OPCDOBJ)/handler.o $(OPCDOBJ)/store.o $(OPCDOBJ)/store_async.o \
                       $(OPCDOBJ)/indication.o $(OPCDOBJ)/fault_probe.o \
                       $(OPCDOBJ)/inventory.o $(OPCDOBJ)/json_util.o $(OPCDOBJ)/snapshot.o \
                       $(OPCDOBJ)/chan_encode.o $(OPCDOBJ)/opcd_log.o $(OPCDOBJ)/platform_stub.o
OPCD_OBJS_STORE_ASYNC := $(OPCDOBJ)/store.o $(OPCDOBJ)/store_async.o
OPCD_OBJS_FAULT     := $(OPCDOBJ)/fault_probe.o
# Pure parser — no platform backend, no protocol library.
OPCD_OBJS_NL80211   := $(OPCDOBJ)/nl80211_parse.o
# Pure channel-field encoder — no platform backend, no protocol library.
OPCD_OBJS_CHAN      := $(OPCDOBJ)/chan_encode.o
# Pure opc.conf freq-source parser — no platform backend, no protocol library.
OPCD_OBJS_FREQSRC   := $(OPCDOBJ)/freq_source.o
OPCD_OBJS_IPIFACE   := $(OPCDOBJ)/ip_iface.o
OPCD_OBJS_TOPOLOGY  := $(OPCDOBJ)/topology.o
# Pure opc.conf roam-notify-port parser — no platform backend, no protocol library.
OPCD_OBJS_ROAMCONF  := $(OPCDOBJ)/roam_notify_conf.o
# roam-notify datagram parser — links roam_datagram.o + its deps (json_util for
# field extraction, chan_encode for the channel field). No platform backend.
OPCD_OBJS_ROAMDGRAM := $(OPCDOBJ)/roam_datagram.o $(OPCDOBJ)/json_util.o $(OPCDOBJ)/chan_encode.o
# 감사 로그 순수 헬퍼(ip4str/이름표/ack peek) — no platform backend, no protocol library.
OPCD_OBJS_OLOG      := $(OPCDOBJ)/opcd_log.o

TESTS := test_inventory test_ntp_parse test_json_util test_snapshot test_handler \
         test_store_async test_fault_probe test_nl80211_parse test_chan_encode \
         test_freq_source test_ip_iface test_topology test_roam_notify_conf test_roam_datagram test_opcd_log
BINS  := $(addprefix $(TESTDIR)/,$(TESTS))

.PHONY: all check clean
all: $(BINS)

$(TESTDIR)/test_inventory: $(TESTDIR)/test_inventory.o $(OPCD_OBJS_INVENTORY) $(PROTODIR)/libopcproto.a
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_inventory.o $(OPCD_OBJS_INVENTORY) $(LDFLAGS) $(LDLIBS)

$(TESTDIR)/test_ntp_parse: $(TESTDIR)/test_ntp_parse.o $(OPCD_OBJS_NTP)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_ntp_parse.o $(OPCD_OBJS_NTP)

$(TESTDIR)/test_json_util: $(TESTDIR)/test_json_util.o $(OPCD_OBJS_JSON)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_json_util.o $(OPCD_OBJS_JSON)

$(TESTDIR)/test_snapshot: $(TESTDIR)/test_snapshot.o $(OPCD_OBJS_SNAPSHOT) $(PROTODIR)/libopcproto.a
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_snapshot.o $(OPCD_OBJS_SNAPSHOT) $(LDFLAGS) $(LDLIBS)

$(TESTDIR)/test_handler: $(TESTDIR)/test_handler.o $(OPCD_OBJS_HANDLER) $(PROTODIR)/libopcproto.a
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_handler.o $(OPCD_OBJS_HANDLER) $(LDFLAGS) $(LDLIBS)

$(TESTDIR)/test_store_async: $(TESTDIR)/test_store_async.o $(OPCD_OBJS_STORE_ASYNC)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_store_async.o $(OPCD_OBJS_STORE_ASYNC) $(LDFLAGS)

$(TESTDIR)/test_fault_probe: $(TESTDIR)/test_fault_probe.o $(OPCD_OBJS_FAULT)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_fault_probe.o $(OPCD_OBJS_FAULT)

# Pure parser test — links only nl80211_parse.o (no platform backend, no
# libopcproto). Reuses the host opcd object built by `make native`.
$(TESTDIR)/test_nl80211_parse: $(TESTDIR)/test_nl80211_parse.o $(OPCD_OBJS_NL80211)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_nl80211_parse.o $(OPCD_OBJS_NL80211)

# Pure encoder test — links only chan_encode.o (no platform backend, no
# libopcproto). Reuses the host opcd object built by `make native`.
$(TESTDIR)/test_chan_encode: $(TESTDIR)/test_chan_encode.o $(OPCD_OBJS_CHAN)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_chan_encode.o $(OPCD_OBJS_CHAN)

# Pure opc.conf parser test — links only freq_source.o (no platform backend, no
# libopcproto). Reuses the host opcd object built by `make native`.
$(TESTDIR)/test_freq_source: $(TESTDIR)/test_freq_source.o $(OPCD_OBJS_FREQSRC)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_freq_source.o $(OPCD_OBJS_FREQSRC)

# Pure opc.conf parser test — links only ip_iface.o (no platform backend, no
# protocol library), mirroring test_freq_source.
$(TESTDIR)/test_ip_iface: $(TESTDIR)/test_ip_iface.o $(OPCD_OBJS_IPIFACE)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_ip_iface.o $(OPCD_OBJS_IPIFACE)

# Management-topology resolver test (#122) — links only topology.o.
$(TESTDIR)/test_topology: $(TESTDIR)/test_topology.o $(OPCD_OBJS_TOPOLOGY)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_topology.o $(OPCD_OBJS_TOPOLOGY)

# Pure opc.conf parser test — links only roam_notify_conf.o (no platform
# backend, no libopcproto). Reuses the host opcd object built by `make native`.
$(TESTDIR)/test_roam_notify_conf: $(TESTDIR)/test_roam_notify_conf.o $(OPCD_OBJS_ROAMCONF)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_roam_notify_conf.o $(OPCD_OBJS_ROAMCONF)

# roam-notify datagram parser test — links roam_datagram.o + json_util.o +
# chan_encode.o (no platform backend, no libopcproto).
$(TESTDIR)/test_roam_datagram: $(TESTDIR)/test_roam_datagram.o $(OPCD_OBJS_ROAMDGRAM)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_roam_datagram.o $(OPCD_OBJS_ROAMDGRAM)

# 감사 로그 헬퍼 테스트 — 순수 함수만 검증(no libopcproto). opcd_logf 는
# opcd_log_init() 전 no-op 계약이라 테스트에서 호스트 syslog 를 오염시키지 않는다.
$(TESTDIR)/test_opcd_log: $(TESTDIR)/test_opcd_log.o $(OPCD_OBJS_OLOG)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_opcd_log.o $(OPCD_OBJS_OLOG)

$(TESTDIR)/%.o: %.c | $(TESTDIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Header dependency tracking (#40) — each .d sits next to its test .o. The
# reused $(OPCDOBJ)/*.o are covered by opcd/Makefile's own .d includes (the
# top-level `check` always runs `make native` first).
-include $(BINS:%=%.d)

$(TESTDIR):
	mkdir -p $(TESTDIR)

check: $(BINS)
	@for t in $(BINS); do echo "== $$t =="; $$t || exit 1; done
	@echo "all opcd unit tests passed"

clean:
	rm -rf $(TESTDIR)
