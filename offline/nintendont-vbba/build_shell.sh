#!/usr/bin/env bash
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
# Build the SHELL (the Nintendont loader DOL) and its embedded fallback kernel for the stub.
#   NIN_MS_SHELL_SEQ=N [VBBA_OUT=DIR] build_shell.sh [--kernel-zip kernel.zip]
# Production keys only (the dev key is never in this build), writes $VBBA_OUT/shell.dol (the loader DOL; NIN_MS_SHELL_SEQ is its own OTA sequence,
# 0 = the one embedded in the stub, N = the one published as shl-N.mscf) and $VBBA_OUT/kernel.zip (its kernel).  --kernel-zip swaps in another kernel ZIP before the loader is linked (for
# example the exact q32 kernel of a tested package) and rebuilds only the loader, so the shell code and the kernel are chosen independently.
set -euo pipefail
d=$(cd "$(dirname "$0")" && pwd)
kz=""
[[ "${1:-}" == "--kernel-zip" ]] && { kz=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); [[ -f "$kz" ]] || { echo "no such kernel zip: $kz"; exit 1; }; }
export NIN_MS_SHELL_SEQ=${NIN_MS_SHELL_SEQ:-0}
# The kernel this script builds is only the fallback used when no --kernel-zip is given; a release shell is built around the release kernel
# (build_kernel.sh) with --kernel-zip, which is then embedded byte for byte.  The flags below also decide the loader build.
export NIN_MS_RELEASE=${NIN_MS_RELEASE:-1115} NIN_MS_DNS=1 NIN_MS_NET=1 NIN_MS_MAC=1 NIN_MS_LOG=2 NIN_MS_STALL=1 NIN_MS_RTCFG=1 NIN_MS_WIRED=1 \
  NIN_MS_OSDUMP=1 NIN_MS_NETEARLY=1 NIN_MS_SDLOG=1 NIN_MS_SDPROBE=1 NIN_MS_AUTOPOKE=1 NIN_MS_EXIFIX=1 NIN_MS_PCSAMPLE=1 NIN_MS_GATE2=1 NIN_MS_MACPUB=1
export NIN_MS_TCTIME=0 NIN_MS_BBACH2=1 NIN_MS_TXCAP=3 NIN_MS_TXBUF=8 NIN_MS_EXIPUB=1 NIN_MS_SINGLEUS=20 NIN_MS_ADAPT=1000 NIN_MS_SHUTDUMP=1 NIN_MS_PATCH26=$d/patches/0026b-keep-sampler.patch
export NIN_MS_RAMLOG=1 NIN_MS_VISAMPLE=1 NIN_MS_FREEZEDUMP=1 NIN_MS_FZ2=1 NIN_MS_FZ3=1 NIN_MS_FZ4=1 NIN_MS_FZ5=1 NIN_MS_FZ6=1 NIN_MS_DSPFIX=1 NIN_MS_FZ7=1 NIN_MS_STRMGUARD=1 NIN_MS_FZ9=1 NIN_MS_FZ10=1 NIN_MS_NULLGUARD=1 NIN_MS_FZ11=1
export VBBA_OUT=${VBBA_OUT:-/tmp/wii-shell-out}
bash "$d/build_wii.sh" build
if [[ -n "$kz" ]]; then
  B=${VBBA_SCRATCH:-${TMPDIR:-/tmp}/vbba-build-$USER}; B=${B%/}; t=$B/vbba
  cp "$kz" "$t/loader/data/kernel.zip"
  touch "$t/loader/data/kernel.zip"
  rm -f "$t/loader/loader.dol" "$t/loader/build/kernel_zip.o" "$t/loader/build/kernel_zip.h"
  export DEVKITPRO=$B/dkp DEVKITPPC=$B/dkp/devkitPPC PATH=$B/dkp/devkitPPC/bin:$B/dkp/devkitARM/bin:$B/dkp/tools/bin:$PATH
  make -C "$t/loader" >> "$B/vbba.log" 2>&1
  bash "$d/ota/gen_padsyms.sh" "$t/loader" >> "$B/vbba.log" 2>&1 || { echo "pad symbol pass failed (see $B/vbba.log)"; exit 1; }
  cp "$t/loader/loader.dol" "$VBBA_OUT/shell.dol"; cp "$kz" "$VBBA_OUT/kernel.zip"
  echo "shell.dol rebuilt with the kernel zip $kz -> $VBBA_OUT/shell.dol ($(stat -f %z "$VBBA_OUT/shell.dol") B)"
fi
ls -la "$VBBA_OUT/shell.dol" "$VBBA_OUT/kernel.zip"
