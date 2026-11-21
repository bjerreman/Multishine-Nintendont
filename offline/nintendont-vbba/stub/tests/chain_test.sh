#!/bin/bash
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
# Emulator test of the stub's chain-load (tramp.S): a GameCube build of the stub (MS_STUB_TEST: no SD, buffers in MEM1) embeds chain_payload.dol as its "shell"; headless Dolphin (Docker,
# one container, no host window) boots the STUB, which must verify and copy the payload's sections over itself, zero the payload's bss and start it.  The payload writes marker words that
# the harness' RAM watcher records.  Needs devkitPPC and Docker; skipped (exit 0) without them.
#   chain_test.sh        -> "chain test: ok" or the failing check
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}; export DEVKITPPC=${DEVKITPPC:-$DEVKITPRO/devkitPPC}
cc="$DEVKITPPC/bin/powerpc-eabi-gcc"
[[ -z "${MS_NO_EMULATOR:-}" ]] || { echo "chain test: skipped (MS_NO_EMULATOR: the headless Dolphin image is not part of this repository)"; exit 0; }
if [[ ! -x "$cc" ]] || ! command -v docker >/dev/null || ! docker image inspect multishine-dolphin-arm64:dev >/dev/null 2>&1; then echo "chain test: skipped (devkitPPC, Docker or the Dolphin image is missing)"; exit 0; fi
while [[ -n "$(docker ps -q)" ]]; do sleep 20; done          # one emulator container at a time
t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
"$cc" -DGEKKO -mogc -mcpu=750 -meabi -mhard-float -O1 -I"$DEVKITPRO/libogc/include" -c "$here/chain_payload.c" -o "$t/p.o"
"$cc" -mogc -mcpu=750 -meabi -mhard-float -o "$t/p.elf" "$t/p.o" -L"$DEVKITPRO/libogc/lib/cube" -logc -lm
"$DEVKITPRO/tools/bin/elf2dol" "$t/p.elf" "$t/payload.dol"
MS_STUB_TEST=1 bash "$here/../build_stub.sh" "$t/payload.dol" "$t/stub" >/dev/null
mkdir -p "$t/out"
docker run --rm --platform linux/arm64 --network none --shm-size=1g --memory=3g -v "$t/stub:/in:ro" -v "$t/out:/out" multishine-dolphin-arm64:dev \
  --iso /in/boot.dol --out /out --frames 300 --max-wall 150 --dump none \
  --watch 80002000:magic --watch 80002004:data --watch 80002008:bss --watch 8000200C:ro --watch 80002010:n >"$t/run.log" 2>&1 || { tail -5 "$t/run.log"; echo "chain test: the emulator run failed"; exit 1; }
tsv="$t/out/ram.tsv"
grep -q "magic=c0dec0de" "$tsv" || { echo "chain test FAILED: the payload never ran (no marker)"; exit 1; }
grep -q "data=11223344" "$tsv" || { echo "chain test FAILED: the payload's .data was not copied"; exit 1; }
grep -q "ro=0badf00d" "$tsv" || { echo "chain test FAILED: the payload's .rodata was not copied"; exit 1; }
if grep -q "bss=" "$tsv"; then echo "chain test FAILED: the payload's bss was not cleared ($(grep -o 'bss=[0-9a-f]*' "$tsv" | head -1))"; exit 1; fi   # it landed on the stub's own data: stale words if not zeroed
n1=$(grep -o "n=[0-9a-f]*" "$tsv" | sed -n 2p); n2=$(grep -o "n=[0-9a-f]*" "$tsv" | tail -1)
[[ -n "$n1" && "$n1" != "$n2" ]] || { echo "chain test FAILED: the payload does not keep running"; exit 1; }
echo "chain test: ok (the stub chain-loaded the embedded shell in Dolphin: sections copied over the stub, bss cleared, entry started)"
