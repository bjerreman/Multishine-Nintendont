#!/bin/bash
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
# Emulator test of the shell's in-place hand-over caller (ota/ms_chain.c, built with MS_CHAIN_TEST: GameCube, no MEM2, no ES / STM): a program that plays the early shell calls ms_chain_run with the same
# chain_payload.dol as chain_test.sh; headless Dolphin (Docker, one container, no host window) must see the payload run (sections copied over the caller, bss cleared, entry started), the loop-guard word
# 'MSCH' at 0x81309F00 and the Homebrew Channel argument block (magic at 0x80001808, the command line at 0x81308000).  Skipped (exit 0) without devkitPPC, Docker or the Dolphin image.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; root="$(cd "$here/../../../.." && pwd)"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}; export DEVKITPPC=${DEVKITPPC:-$DEVKITPRO/devkitPPC}
cc="$DEVKITPPC/bin/powerpc-eabi-gcc"
if [[ ! -x "$cc" ]] || ! command -v docker >/dev/null || ! docker image inspect multishine-dolphin-arm64:dev >/dev/null 2>&1; then echo "chain caller test: skipped (devkitPPC, Docker or the Dolphin image is missing)"; exit 0; fi
while [[ -n "$(docker ps -q)" ]]; do sleep 20; done
t="$(mktemp -d)"; [[ -n "${KEEP:-}" ]] || trap 'rm -rf "$t"' EXIT; echo "work dir $t" >&2
"$cc" -DGEKKO -mogc -mcpu=750 -meabi -mhard-float -O1 -I"$DEVKITPRO/libogc/include" -c "$here/chain_payload.c" -o "$t/p.o"
"$cc" -mogc -mcpu=750 -meabi -mhard-float -o "$t/p.elf" "$t/p.o" -L"$DEVKITPRO/libogc/lib/cube" -logc -lm
"$DEVKITPRO/tools/bin/elf2dol" "$t/p.elf" "$t/payload.dol"
mkdir -p "$t/loader/source"
python3.14 "$root/offline/nintendont-vbba/ota/stage_loader.py" "$root" "$t" >/dev/null
src="$t/loader/source"
cp "$root/offline/nintendont-vbba/ota/ms_tramp2.S" "$root/offline/nintendont-vbba/stub/shell_dol.S" "$here/chain_caller_main.c" "$src/"
CF="-std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter -DGEKKO -mogc -mcpu=750 -meabi -mhard-float -DMS_CHAIN_TEST=1 -I$src -I$DEVKITPRO/libogc/include"
objs=()
for f in chain_caller_main.c ms_chain.c ms_ota_core.c mw2_crypto.c ms_sha256.c; do "$cc" $CF -c "$src/$f" -o "$t/${f%.c}.o"; objs+=("$t/${f%.c}.o"); done
"$cc" -DGEKKO -mogc -mcpu=750 -meabi -mhard-float -mregnames -D_LANGUAGE_ASSEMBLY -c "$src/ms_tramp2.S" -o "$t/tramp.o"
"$cc" -DGEKKO -mogc -mcpu=750 -meabi -mhard-float -I"$DEVKITPRO/libogc/include" "-DSHELL_DOL_PATH=\"$t/payload.dol\"" -c "$src/shell_dol.S" -o "$t/shell_dol.o"
"$cc" -mogc -mcpu=750 -meabi -mhard-float -o "$t/caller.elf" "${objs[@]}" "$t/tramp.o" "$t/shell_dol.o" -L"$DEVKITPRO/libogc/lib/cube" -logc -lm
"$DEVKITPRO/tools/bin/elf2dol" "$t/caller.elf" "$t/caller.dol"
mkdir -p "$t/in" "$t/out"; cp "$t/caller.dol" "$t/in/boot.dol"
docker run --rm --platform linux/arm64 --network none --shm-size=1g --memory=3g -v "$t/in:/in:ro" -v "$t/out:/out" multishine-dolphin-arm64:dev \
  --iso /in/boot.dol --out /out --frames 300 --max-wall 150 --dump none \
  --watch 80002000:magic --watch 80002004:data --watch 80002008:bss --watch 8000200C:ro --watch 80002010:n --watch 80002014:back --watch 81309F00:guard --watch 81309F40:trmp --watch 81309F44:status --watch 81309F48:tries --watch 80B05014:sq0 --watch 80B05038:sq1 --watch 80B05060:sq2 --watch 80001808:argv >"$t/run.log" 2>&1 || { tail -5 "$t/run.log"; echo "chain caller test: the emulator run failed"; exit 1; }
tsv="$t/out/ram.tsv"
grep -q "magic=c0dec0de" "$tsv" || { echo "chain caller test FAILED: the payload never ran"; exit 1; }
grep -q "data=11223344" "$tsv" || { echo "chain caller test FAILED: .data was not copied"; exit 1; }
grep -q "ro=0badf00d" "$tsv" || { echo "chain caller test FAILED: .rodata was not copied"; exit 1; }
if grep -q "bss=" "$tsv"; then echo "chain caller test FAILED: the bss was not cleared"; exit 1; fi
if grep -q "back=bad0bad0" "$tsv"; then echo "chain caller test FAILED: ms_chain_run returned (refused)"; exit 1; fi
grep -q "guard=4d534348" "$tsv" || { echo "chain caller test FAILED: the loop-guard word was not set"; exit 1; }
grep -q "trmp=54524d50" "$tsv" || { echo "chain caller test FAILED: the tramp left no report"; exit 1; }
grep -q "status=00000001" "$tsv" || { echo "chain caller test FAILED: the tramp verify did not report ok"; exit 1; }
grep -q "tries=00000001" "$tsv" || { echo "chain caller test FAILED: the copy needed more than one attempt"; exit 1; }
grep -q "sq0=e200e294" "$tsv" && grep -q "sq1=962c9615" "$tsv" && grep -q "sq2=b2aab210" "$tsv" || { echo "chain caller test FAILED: the stage squares (yellow / green / cyan) were not painted"; exit 1; }
grep -q "argv=" "$tsv" || { echo "chain caller test FAILED: the argument block was not set"; exit 1; }
n1=$(grep -o "n=[0-9a-f]*" "$tsv" | sed -n 2p); n2=$(grep -o "n=[0-9a-f]*" "$tsv" | tail -1)
[[ -n "$n1" && "$n1" != "$n2" ]] || { echo "chain caller test FAILED: the payload does not keep running"; exit 1; }
echo "chain caller test: ok (ms_chain_run handed over to the payload in Dolphin: sections over the caller, bss cleared, guard word set, argument block set, entry running)"
