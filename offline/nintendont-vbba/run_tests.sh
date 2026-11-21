#!/usr/bin/env bash
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
# Host tests for the virtual BBA (no emulator, no Docker, nothing leaves 127.0.0.1).
#   run_tests.sh [unit|all]   unit: driver/sdnet against the vBBA + fuzz; all: + lab (real lobby server, libenet peer)
set -uo pipefail
d=$(cd "$(dirname "$0")" && pwd); root=$(cd "$d/../.." && pwd)
o=${OUT:-$root/build/nintendont-vbba/hosttest}; mkdir -p "$o"
W=(-Wall -Wextra -Werror)
SAN=(-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
CORE=("$d/core/vbba_chip.c" "$d/core/vbba_router.c")
PLAT=("$d/platform/nintendont/vbba_ndexi.c" "$d/platform/nintendont/vbba_tp_ios.c" "$d/platform/host/vbba_tp_host.c")
RIG=("$d/tests/vbba_rig.c" "$d/tests/mock_ios.c")
DRV=("$root/offline/bba/bba_mx98728.c" "$root/offline/net/sdnet.c")
rc=0
set -e
echo "== strict C99 (pedantic, no libc headers) of the portable core and the Nintendont-side files"
for f in "${CORE[@]}" "$d/core/vbba_cc.c" "$d/platform/nintendont/vbba_ndexi.c" "$d/platform/nintendont/vbba_tp_ios.c" "$d/platform/cubeboot/vbba_tp_cubeboot_stub.c" "$d/platform/cubeboot/vbba_cubeboot_frames.c"; do
  [[ -f "$f" ]] || { echo "   (skipped, not in this tree: ${f#$d/})"; continue; }   # the Cubeboot transport is not part of every tree
  cc -std=c99 -pedantic -ffreestanding "${W[@]}" -c "$f" -o "$o/$(basename "$f" .c)_strict.o"
done
echo "== drivers unchanged: $(cd "$root" && git status --porcelain -- offline/bba/bba_mx98728.c offline/bba/bba_mx98728.h offline/net/sdnet.c offline/net/sdnet.h | wc -l | tr -d ' ') local changes in offline/bba + offline/net sources (git $(cd "$root" && git log -1 --format=%h -- offline/bba offline/net))"
cc -std=gnu99 -O2 "${W[@]}" "$d/tests/vbba_test.c" "${RIG[@]}" "${CORE[@]}" "${PLAT[@]}" "${DRV[@]}" -o "$o/vbba_test"
cc -std=gnu99 "${SAN[@]}" "${W[@]}" "$d/tests/vbba_test.c" "${RIG[@]}" "${CORE[@]}" "${PLAT[@]}" "${DRV[@]}" -o "$o/vbba_test_asan"
cc -std=gnu99 "${SAN[@]}" "${W[@]}" "$d/tests/vbba_fuzz.c" "${RIG[@]}" "${CORE[@]}" "${PLAT[@]}" "${DRV[@]}" -o "$o/vbba_fuzz"
set +e
echo "== unit (O2)"; "$o/vbba_test" > "$o/unit.log" 2>&1 || rc=1; cat "$o/unit.log"
echo "== unit (ASan+UBSan)"; "$o/vbba_test_asan" > "$o/unit_asan.log" 2>&1 || rc=1; tail -1 "$o/unit_asan.log"
echo "== fuzz (ASan+UBSan)"; "$o/vbba_fuzz" ${FUZZ_N:-200000} > "$o/fuzz.log" 2>&1 || rc=1; cat "$o/fuzz.log"
echo "== EXI commands per netplay frame, 1252 driver settings vs wii_fast (the design notes; ASan+UBSan)"
cc -std=gnu99 "${SAN[@]}" "${W[@]}" "$d/tests/exi_frame.c" "${RIG[@]}" "${CORE[@]}" "${PLAT[@]}" "${DRV[@]}" -o "$o/exi_frame" \
  && "$o/exi_frame" > "$o/exi_frame.log" 2>&1 || rc=1; cat "$o/exi_frame.log"
echo "== TCP splice (core/vbba_tcp.c, VBBA_TCP=1): real driver + sdnet + sdtcp through chip + router + host stream sockets to an in-process server; loss/reorder toward the guest, short socket calls (ASan+UBSan, real time ~60 s)"
for f in "$d/core/vbba_router.c"; do cc -std=c99 -pedantic -ffreestanding "${W[@]}" -DVBBA_TCP=1 -c "$f" -o "$o/vbba_router_tcp_strict.o" || rc=1; done
cc -std=gnu99 "${SAN[@]}" "${W[@]}" -DVBBA_TCP=1 -DSDNET_ENABLE_TCP=1 "$d/tests/tcp_test.c" "${RIG[@]}" "${CORE[@]}" "${PLAT[@]}" "${DRV[@]}" "$root/offline/net/sdtcp.c" -o "$o/tcp_test" \
  && perl -e 'alarm 300; exec {$ARGV[0]} @ARGV' "$o/tcp_test" > "$o/tcp.log" 2>&1 || rc=1; cat "$o/tcp.log"
echo "== receive by poll (b18a, VBBA_TP_RXPOLL): the transport against the mock IOS (ASan+UBSan): one request at a time, poll then recvfrom, 120/s"
cc -std=c99 -pedantic -ffreestanding "${W[@]}" -DVBBA_TP_RXPOLL=1 -DVBBA_TP_IOS_TX_INFLIGHT=1 -c "$d/platform/nintendont/vbba_tp_ios.c" -o "$o/vbba_tp_ios_rxpoll_strict.o" || rc=1
cc -std=gnu99 "${SAN[@]}" "${W[@]}" -DVBBA_TP_RXPOLL=1 -DVBBA_TP_IOS_TX_INFLIGHT=1 "$d/tests/vbba_test.c" "${RIG[@]}" "${CORE[@]}" "${PLAT[@]}" "${DRV[@]}" -o "$o/vbba_test_rxpoll" \
  && "$o/vbba_test_rxpoll" > "$o/unit_rxpoll.log" 2>&1 || rc=1; cat "$o/unit_rxpoll.log"
# the Nintendont glue (kernel/vbba_glue.c from patches/0001-multishine-vbba.patch, unmodified) on the host: threads, mock kernel + IOS
# the glue tests prove the fast-thread build (-DVBBA_SINGLE=0): since the glue defaults to single-context mode (no fast thread; the main loop serves EXI), which these tests do not model
echo "== glue (patch applied to a pristine bbc0a20 archive; ASan+UBSan; real threads, wall-clock ~10 s)"
pin=${NIN_SRC:-$root/third_party/Nintendont}; PIN=bbc0a208e81d6ab0a828a431dc0b2ba6fc06aec5
if [[ -e "$pin/.git" && $(git -C "$pin" rev-parse HEAD) == "$PIN" ]]; then
  gt=$o/glue_tree; rm -rf "$gt"; mkdir -p "$gt" "$o/glue_src"
  git -C "$pin" archive "$PIN" | tar x -C "$gt"
  git init -q "$gt"        # a nested repo: git apply inside the outer repo would skip ignored paths
  if (cd "$gt" && git apply --whitespace=nowarn "$d/patches/0001-multishine-vbba.patch"); then
    cp "$gt/kernel/vbba_glue.c" "$gt/kernel/vbba_glue.h" "$o/glue_src/"
    G=$d/tests/glue
    cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
      -DVBBA_WITH_CORE -DVBBA_DIAG=0 -DVBBA_NET_MIN_S=0 -DVBBA_NET_IDLE_S=0 -DVBBA_SINGLE=0 -DVBBA_TMO_START_MS=1500 -DVBBA_TMO_OTHER_MS=600 -DVBBA_STALL_TICKS=380000u -DVBBA_TL_PERIOD_MS=200u -DVBBA_HEAP_BASE='((uintptr_t)heap_blk)' -DVBBA_HEAP_SIZE=0x1E000 -DVBBA_EXT_ADDR='((uintptr_t)status_blk)' -DVBBA_STATUS_ADDR='((uintptr_t)pub_blk)' \
      -I"$G/shim" -I"$G" -I"$d/core" -I"$d/platform/nintendont" \
      "$o/glue_src/vbba_glue.c" "$G/kmock.c" "$G/glue_test.c" "${CORE[@]}" "$d/platform/nintendont/vbba_ndexi.c" "$d/platform/nintendont/vbba_tp_ios.c" "${DRV[@]}" -lpthread -o "$o/glue_test"
    export ASAN_OPTIONS="detect_stack_use_after_return=0:${ASAN_OPTIONS:-}"   # the mock threads run on stacks that glue_test measures (0xA5 fill); ASan fake stacks would hide the use
    perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue.log" 2>&1 || rc=1; cat "$o/glue.log"
    echo "== glue with the b18a receive pattern (VBBA_TP_RXPOLL=1, one sendto in IOS): same test, mock IOS answers fcntl and poll; a recvfrom only after a ready poll, never two requests at once"
    cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
      -DVBBA_TP_RXPOLL=1 -DVBBA_TP_IOS_TX_INFLIGHT=1 -DVBBA_WITH_CORE -DVBBA_DIAG=0 -DVBBA_NET_MIN_S=0 -DVBBA_NET_IDLE_S=0 -DVBBA_SINGLE=0 -DVBBA_TMO_START_MS=1500 -DVBBA_TMO_OTHER_MS=600 -DVBBA_STALL_TICKS=380000u -DVBBA_TL_PERIOD_MS=200u -DVBBA_HEAP_BASE='((uintptr_t)heap_blk)' -DVBBA_HEAP_SIZE=0x1E000 -DVBBA_EXT_ADDR='((uintptr_t)status_blk)' -DVBBA_STATUS_ADDR='((uintptr_t)pub_blk)' \
      -I"$G/shim" -I"$G" -I"$d/core" -I"$d/platform/nintendont" \
      "$o/glue_src/vbba_glue.c" "$G/kmock.c" "$G/glue_test.c" "${CORE[@]}" "$d/platform/nintendont/vbba_ndexi.c" "$d/platform/nintendont/vbba_tp_ios.c" "${DRV[@]}" -lpthread -o "$o/glue_test_rxpoll" \
      && perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test_rxpoll" > "$o/glue_rxpoll.log" 2>&1 || rc=1; grep -E "receive by poll|UDP echoes|PASS|FAIL" "$o/glue_rxpoll.log"
    echo "== glue with the b22a engine status snapshot (VBBA_ENGSTAT=1, fake PowerPC MEM1): the eng lines carry the engine's words, the ARM-side interrupt words and EXI counters; lines < 200 characters"
    cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
      -DVBBA_ENGSTAT=1 -DVBBA_TP_IOS_TX_INFLIGHT=1 -DVBBA_WITH_CORE -DVBBA_DIAG=0 -DVBBA_NET_MIN_S=0 -DVBBA_NET_IDLE_S=0 -DVBBA_SINGLE=0 -DVBBA_TMO_START_MS=1500 -DVBBA_TMO_OTHER_MS=600 -DVBBA_STALL_TICKS=380000u -DVBBA_TL_PERIOD_MS=200u -DVBBA_HEAP_BASE='((uintptr_t)heap_blk)' -DVBBA_HEAP_SIZE=0x1E000 -DVBBA_EXT_ADDR='((uintptr_t)status_blk)' -DVBBA_STATUS_ADDR='((uintptr_t)pub_blk)' \
      -I"$G/shim" -I"$G" -I"$d/core" -I"$d/platform/nintendont" \
      "$o/glue_src/vbba_glue.c" "$G/kmock.c" "$G/glue_test.c" "${CORE[@]}" "$d/platform/nintendont/vbba_ndexi.c" "$d/platform/nintendont/vbba_tp_ios.c" "${DRV[@]}" -lpthread -o "$o/glue_test_engstat" \
      && perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test_engstat" > "$o/glue_engstat.log" 2>&1 || rc=1; grep -E "ENGSTAT|UDP echoes|PASS|FAIL" "$o/glue_engstat.log"
    echo "== glue with the b22b game/disc/card lines (VBBA_ENGSTAT=1 VBBA_SCENESTAT=1, fake PowerPC MEM1, fake DI registers and card state): game words, DI state, card state in the sets and in the shutdown set; lines < 200 characters"
    cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
      -DVBBA_ENGSTAT=1 -DVBBA_SCENESTAT=1 -DVBBA_TP_IOS_TX_INFLIGHT=1 -DVBBA_WITH_CORE -DVBBA_DIAG=0 -DVBBA_NET_MIN_S=0 -DVBBA_NET_IDLE_S=0 -DVBBA_SINGLE=0 -DVBBA_TMO_START_MS=1500 -DVBBA_TMO_OTHER_MS=600 -DVBBA_STALL_TICKS=380000u -DVBBA_TL_PERIOD_MS=200u -DVBBA_HEAP_BASE='((uintptr_t)heap_blk)' -DVBBA_HEAP_SIZE=0x1E000 -DVBBA_EXT_ADDR='((uintptr_t)status_blk)' -DVBBA_STATUS_ADDR='((uintptr_t)pub_blk)' \
      -I"$G/shim" -I"$G" -I"$d/core" -I"$d/platform/nintendont" \
      "$o/glue_src/vbba_glue.c" "$G/kmock.c" "$G/glue_test.c" "${CORE[@]}" "$d/platform/nintendont/vbba_ndexi.c" "$d/platform/nintendont/vbba_tp_ios.c" "${DRV[@]}" -lpthread -o "$o/glue_test_scenestat" \
      && perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test_scenestat" > "$o/glue_scenestat.log" 2>&1 || rc=1; grep -E "ENGSTAT|SCENESTAT|UDP echoes|PASS|FAIL" "$o/glue_scenestat.log"
    echo "== glue with the b20 sender thread (VBBA_SYNCTX=1, blocking IOS_Ioctlv, one sendto in IOS): every sendto goes through it, echoes unchanged"
    cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
      -DVBBA_SYNCTX=1 -DVBBA_TP_IOS_TX_INFLIGHT=1 -DVBBA_WITH_CORE -DVBBA_DIAG=0 -DVBBA_NET_MIN_S=0 -DVBBA_NET_IDLE_S=0 -DVBBA_SINGLE=0 -DVBBA_TMO_START_MS=1500 -DVBBA_TMO_OTHER_MS=600 -DVBBA_STALL_TICKS=380000u -DVBBA_TL_PERIOD_MS=200u -DVBBA_HEAP_BASE='((uintptr_t)heap_blk)' -DVBBA_HEAP_SIZE=0x1E000 -DVBBA_EXT_ADDR='((uintptr_t)status_blk)' -DVBBA_STATUS_ADDR='((uintptr_t)pub_blk)' \
      -I"$G/shim" -I"$G" -I"$d/core" -I"$d/platform/nintendont" \
      "$o/glue_src/vbba_glue.c" "$G/kmock.c" "$G/glue_test.c" "${CORE[@]}" "$d/platform/nintendont/vbba_ndexi.c" "$d/platform/nintendont/vbba_tp_ios.c" "${DRV[@]}" -lpthread -o "$o/glue_test_synctx" \
      && perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test_synctx" > "$o/glue_synctx.log" 2>&1 || rc=1; grep -E "synctx|UDP echoes|PASS|FAIL" "$o/glue_synctx.log"
    echo "== glue with the b19 buffer placement (VBBA_IOSBUF=1, one sendto in IOS): the datagram vector of every sendto lies in the socket heap block"
    cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
      -DVBBA_IOSBUF=1 -DVBBA_TP_IOS_TX_INFLIGHT=1 -DVBBA_WITH_CORE -DVBBA_DIAG=0 -DVBBA_NET_MIN_S=0 -DVBBA_NET_IDLE_S=0 -DVBBA_SINGLE=0 -DVBBA_TMO_START_MS=1500 -DVBBA_TMO_OTHER_MS=600 -DVBBA_STALL_TICKS=380000u -DVBBA_TL_PERIOD_MS=200u -DVBBA_HEAP_BASE='((uintptr_t)heap_blk)' -DVBBA_HEAP_SIZE=0x1E000 -DVBBA_EXT_ADDR='((uintptr_t)status_blk)' -DVBBA_STATUS_ADDR='((uintptr_t)pub_blk)' \
      -I"$G/shim" -I"$G" -I"$d/core" -I"$d/platform/nintendont" \
      "$o/glue_src/vbba_glue.c" "$G/kmock.c" "$G/glue_test.c" "${CORE[@]}" "$d/platform/nintendont/vbba_ndexi.c" "$d/platform/nintendont/vbba_tp_ios.c" "${DRV[@]}" -lpthread -o "$o/glue_test_iosbuf" \
      && perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test_iosbuf" > "$o/glue_iosbuf.log" 2>&1 || rc=1; grep -E "iosbuf|UDP echoes|PASS|FAIL" "$o/glue_iosbuf.log"
    echo "== glue with the Connection check (the NIN_MS_CCHECK block of build_wii.sh applied to the glue, core/vbba_cc.c, the mock echoes MSEC datagrams): PPC mailbox -> 10 s check through the fast thread's router/IOS path -> verdict; cancel"
    awk '/<<.PYCC.$/{f=1;next} /^PYCC$/{f=0} f' "$d/build_wii.sh" > "$o/cc_patch.py"
    printf 'VBBA_OBJS := vbba_chip.o vbba_router.o vbba_ndexi.o vbba_tp_ios.o\n$(VBBA_OBJS) vbba_glue.o: CFLAGS += $(VBBA_FLAGS)\n' > "$o/cc_Makefile"
    { echo 'static unsigned vgPsH[4], vgCfgKind;   /* the host glue (patch 0001 only) has no PASSHIST counters / ncd interface kind: the real build has both */'; cat "$o/glue_src/vbba_glue.c"; } > "$o/glue_src/vbba_glue_cc.c"
    python3 "$o/cc_patch.py" "$o/glue_src/vbba_glue_cc.c" "$o/cc_Makefile" && grep -c "vg_cc_step" "$o/glue_src/vbba_glue_cc.c" | sed -e 's/^/   vg_cc_step call sites + definition: /' || rc=1
    cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
      -DGLUE_CC=1 -DVBBA_WITH_CORE -DVBBA_DIAG=0 -DVBBA_NET_MIN_S=0 -DVBBA_NET_IDLE_S=0 -DVBBA_SINGLE=0 -DVBBA_TMO_START_MS=1500 -DVBBA_TMO_OTHER_MS=600 -DVBBA_STALL_TICKS=380000u -DVBBA_TL_PERIOD_MS=200u -DVBBA_HEAP_BASE='((uintptr_t)heap_blk)' -DVBBA_HEAP_SIZE=0x1E000 -DVBBA_EXT_ADDR='((uintptr_t)status_blk)' -DVBBA_STATUS_ADDR='((uintptr_t)pub_blk)' \
      -I"$G/shim" -I"$G" -I"$d/core" -I"$d/platform/nintendont" \
      "$o/glue_src/vbba_glue_cc.c" "$G/kmock.c" "$G/glue_test.c" "${CORE[@]}" "$d/core/vbba_cc.c" "$d/platform/nintendont/vbba_ndexi.c" "$d/platform/nintendont/vbba_tp_ios.c" "${DRV[@]}" -lpthread -o "$o/glue_test_cc" \
      && perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test_cc" > "$o/glue_cc.log" 2>&1 || rc=1; grep -E "CCHECK" "$o/glue_cc.log"
    echo "== glue, own-open path (no stock IOS handles)"
    GLUE_OWN=1 perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_own.log" 2>&1 || rc=1; tail -3 "$o/glue_own.log"
    echo "== glue, start-up variants: variant 1 first (checked in every run); variant 1 hangs so variant 0 runs with kd out -15 + ncd -6 x3; address only via SO_GETHOSTID"
    GLUE_KDOUT=-15 GLUE_HOSTID=1 perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_hostid.log" 2>&1 || rc=1; grep -E "start-up order|PASS|FAIL" "$o/glue_hostid.log"
    GLUE_HANG=1 GLUE_KDOUT=-15 GLUE_NCD6=1 perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_ncd6.log" 2>&1 || rc=1; grep -E "HANG|start-up order|PASS|FAIL" "$o/glue_ncd6.log"
    echo "== glue, the Wii of runs 6/7 scaled down: first kd call slower than the timeout, the next one queues behind it (kd start-up ~20 s there); slow IOS sendto (latency counters)"
    GLUE_KDSLOW=1 GLUE_KDOUT=-15 perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_kdslow.log" 2>&1 || rc=1; grep -E "HANG|start-up order|DHCP bound|PASS|FAIL" "$o/glue_kdslow.log"
    GLUE_LATETX=1 perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_latetx.log" 2>&1 || rc=1; grep -E "LATETX|PASS|FAIL" "$o/glue_latetx.log"
    GLUE_SLOWTX=1 perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_slowtx.log" 2>&1 || rc=1; grep -E "path counters|PASS|FAIL" "$o/glue_slowtx.log"
    echo "== loader sweep (the auto app: preset rotates per launch; loader/source/vbba_sweep.c from the patch, compiled unmodified)"
    cc -std=c99 -pedantic -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$gt/loader/source" "$d/tests/sweep/sweep_test.c" "$gt/loader/source/vbba_sweep.c" -o "$o/sweep_test" \
      && "$o/sweep_test" > "$o/sweep.log" 2>&1 || rc=1; cat "$o/sweep.log"
    echo "== glue, /vbba.cfg duty presets (the ARM duty-cycle experiment): the values in force, a full run with each (echoes, DHCP, hold, shutdown)"
    for c in duty=1 duty=2 duty=3 duty=4 duty=5 duty=9 "# test
spin=0 idle=200,prio=20 main=300
"; do
      GLUE_CFG="$c" perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_cfg.log" 2>&1 || rc=1; grep -E "vbba.cfg|PASS|FAIL" "$o/glue_cfg.log" | sed -e 's/^/   /' | head -3
    done
    echo "== glue, preset baked into the app by the loader: used when there is no /vbba.cfg, overridden by it, an old loader without the mailbox"
    for bc in "4|" "3|" "5|" "4|duty=2" "2|idle=300" "nomagic|"; do
      bk=${bc%%|*}; cf=${bc#*|}
      if [[ -n "$cf" ]]; then env GLUE_BAKED="$bk" GLUE_CFG="$cf" perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_baked.log" 2>&1 || rc=1
      else env GLUE_BAKED="$bk" perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_baked.log" 2>&1 || rc=1; fi
      grep -E "baked|PASS|FAIL" "$o/glue_baked.log" | sed -e 's/^/   /' | head -3
    done
    echo "== glue, the auto app: the sweep word the loader leaves (position 2 of 6 = d4; no state file + write failed; /vbba.cfg decides) shows in the status block and the log"
    for ac in "4|206" "3|20306" "x|40206"; do
      GLUE_AUTO="${ac#*|}"; ab=${ac%%|*}
      if [[ "$ab" == x ]]; then env GLUE_AUTO="$GLUE_AUTO" GLUE_CFG="duty=2" perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_auto.log" 2>&1 || rc=1
      else env GLUE_AUTO="$GLUE_AUTO" GLUE_BAKED="$ab" perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_auto.log" 2>&1 || rc=1; fi
      grep -E "baked|PASS|FAIL" "$o/glue_auto.log" | sed -e 's/^/   /' | head -3
    done
    echo "== glue, hung/late IOS call (kd start-up never returns in time), udelay returning at once, fast thread stuck (watchdog takeover)"
    GLUE_HANG=1 perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_hang.log" 2>&1 || rc=1; grep -E "HANG|PASS|FAIL" "$o/glue_hang.log"
    GLUE_NOSLEEP=1 perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_nosleep.log" 2>&1 || rc=1; grep -E "NOSLEEP|PASS|FAIL" "$o/glue_nosleep.log"
    GLUE_STALL=1 perl -e 'alarm 120; exec {$ARGV[0]} @ARGV' "$o/glue_test" > "$o/glue_stall.log" 2>&1 || rc=1; grep -E "STALL|PASS|FAIL" "$o/glue_stall.log"
  else echo "patch does not apply to the pinned clone"; rc=1; fi
else echo "pinned Nintendont clone missing or not at $PIN: glue test SKIPPED (rc 1)"; rc=1; fi
echo "== heap/queue layout of the glue (queue entries >= slots + 16, see the design notes Round 5f)"
python3 "$d/tests/check_glue_heap.py" || rc=1
echo "== log line lengths of the glue (dbgprintf's 256-byte stack buffer has no bound; every line < 200 characters worst case)"
python3 "$d/tests/check_log_lines.py" || rc=1
echo "== breadcrumb words (core/vbba_crumbs.h encoding) and the additive-only instrumentation of the b0 glue (apps i1, i2)"
cc -std=c99 -pedantic "${W[@]}" -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$d/tests/crumbs_test.c" -o "$o/crumbs_test" && "$o/crumbs_test" || rc=1
cc -std=c99 -pedantic "${W[@]}" -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$d/tests/wl_test.c" -o "$o/wl_test" && "$o/wl_test" || rc=1
echo "== Connection check (core/vbba_cc.c): simulated network (loss, reorder, jitter, burst gaps, duplicates, no echo, no network, busy, cancel), mailbox protocol, verdict thresholds (ASan+UBSan)"
cc -std=gnu99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "${W[@]}" "$d/tests/cc_test.c" "$d/core/vbba_cc.c" -o "$o/cc_test" && "$o/cc_test" | tail -2 || rc=1
echo "== stall watchdog decisions (header extracted from patch 0015)"
awk '/^\+\+\+ b\/kernel\/vbba_stall.h/{f=1;next} /^diff --git/{f=0} f&&/^\+/{print substr($0,2)}' "$d/patches/0015-multishine-stall-watch.patch" > "$o/stall_hdr.h"
cc -std=c99 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$o" -o "$o/stall_watch_test" "$d/tests/stall_watch_test.c" && "$o/stall_watch_test" || rc=1
python3.14 "$d/tests/rtcfg_patch_test.py" || rc=1
python3.14 "$d/tests/netearly_patch_test.py" || rc=1
python3.14 "$d/tests/sdlog_patch_test.py" || rc=1
python3.14 "$d/tests/sdprobe_patch_test.py" || rc=1
python3.14 "$d/tests/autopoke_patch_test.py" || rc=1
awk '/^\+\+\+ b\/kernel\/vbba_stall.h/{f=1;next} /^diff --git/{f=0} f&&/^\+/{print substr($0,2)}' "$d/patches/0021-multishine-sd-probe-nosave.patch" > "$o/stall_hdr21.h"
awk -v new="$o/stall_hdr21.h" '/^#define MSS_IS_FAT/{while ((getline l < new) > 0) print l; next} {print}' "$o/stall_hdr.h" > "$o/stall_hdr_new.h"
cc -std=c99 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$o" -o "$o/stall_probe_test" "$d/tests/stall_probe_test.c" && "$o/stall_probe_test" || rc=1
echo "== (patch 0017): vg_ncd_cfg() and vg_ifmac_again() text against mocks (ASan+UBSan)"
python3 "$d/tests/ncd_cfg_test.py" || rc=1
python3 "$d/tests/ifmac_again_test.py" || rc=1
echo "== (patch 0018): vg_diag_os() against a fake PowerPC memory image (ASan+UBSan)"
python3 "$d/tests/os_dump_test.py" || rc=1
echo "== (patch 0023): the guest EXIDMA stub as machine code (keeps an outstanding card callback, atomic mask update) + patch text"
python3 "$d/tests/exidma_stub_test.py" || rc=1
python3 "$d/tests/exifix_patch_test.py" || rc=1
echo "== (patch 0024): the interrupt sampler in FakeInterrupt.S as machine code"
python3 "$d/tests/fakeint_sample_test.py" || rc=1
python3 "$d/tests/pcsample_patch_test.py" || rc=1
echo "== (patch 0025): the handshake gate"
python3 "$d/tests/gate_patch_test.py" || rc=1
echo "== release profile (patch 0026)"
python3 "$d/tests/quiet_patch_test.py" || rc=1
echo "== kernel OTA (patch 0028): the shell verifies a signed kernel container (the C core the loader runs), rollback state machine"
python3.14 "$d/ota/tests/test_ota.py" 2>&1 | tail -4 || rc=1
echo "== disc lock (stock v1.02 only): sampled fingerprint, cache, alterations (image tests skip without the retail image)"
python3.14 "$d/ota/tests/test_disc.py" 2>&1 | tail -4 || rc=1
echo "== shell look: launch progress mapping, generated background (mark aspect vs the site SVG, position), text layout rules"
python3.14 "$d/ota/tests/test_theme.py" 2>&1 | tail -4 || rc=1
echo "== settings screen: forced values of the hidden rows leave the shipped config unchanged, no gaps in the row table (needs the pinned Nintendont sources)"
python3.14 "$d/ota/tests/test_cfg.py" 2>&1 | tail -4 || rc=1
echo "== Wii network config core (ms_netcfg_core): one connection row parse/validate/serialize/select/restore on a synthetic 0x1B5C block, no secret in the layout line (ASan+UBSan, strict C89)"
python3.14 "$d/ota/tests/test_netcfg.py" 2>&1 | tail -4 || rc=1
echo "== pinned TLS fetch (ms_tlsget.c over offline/net/tls) against the repo's own TLS listener on loopback: pin, wrong pin, server down, silent server, too big"
if [[ -f "$d/ota/tests/test_tls.py" ]]; then python3.14 "$d/ota/tests/test_tls.py" 2>&1 | tail -4 || rc=1; else echo "   (skipped: needs the server's TLS listener, which is not in this tree)"; fi
echo "== stub (the immutable first layer): builds, embeds the shell verbatim, pins the release keys, trampoline checks; the chain-load in headless Dolphin"
python3.14 "$d/stub/tests/test_stub_build.py" 2>&1 | tail -4 || rc=1
bash "$d/stub/tests/chain_test.sh" 2>&1 | tail -2 || rc=1
if [[ "${1:-unit}" == all ]]; then
  "$d/tests/run_lab.sh" "$o" || rc=1
fi
echo "vbba host tests: $([[ $rc == 0 ]] && echo PASS || echo FAIL)"
exit $rc
