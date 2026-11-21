#!/usr/bin/env bash
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
# Build the release KERNEL (the Nintendont ARM kernel with the virtual BBA, its TCP splice and the Wi-Fi settings) as kernel.zip.
#   [VBBA_OUT=DIR] [VBBA_SCRATCH=DIR] build_kernel.sh      -> $VBBA_OUT/kernel.zip; then tools/release/make_kernel_ota.py --seq N ...
# The flag set below is the complete release set (every switch build_wii.sh knows and the value the release uses); nothing else is read.
set -euo pipefail
d=$(cd "$(dirname "$0")" && pwd)
export NIN_MS_RELEASE=${NIN_MS_RELEASE:-1115}
export NIN_MS_DNS=1 NIN_MS_NET=1 NIN_MS_MAC=1 NIN_MS_LOG=2 NIN_MS_STALL=1 NIN_MS_RTCFG=1 NIN_MS_WIRED=1 NIN_MS_OSDUMP=1 NIN_MS_NETEARLY=1 NIN_MS_SDLOG=1 \
  NIN_MS_SDPROBE=1 NIN_MS_AUTOPOKE=1 NIN_MS_EXIFIX=1 NIN_MS_PCSAMPLE=1 NIN_MS_GATE2=1 NIN_MS_MACPUB=1 NIN_MS_QUIET=1 NIN_MS_PATCH26=$d/patches/0026b-keep-sampler.patch
export NIN_MS_TCTIME=0 NIN_MS_BBACH2=1 NIN_MS_TXCAP=3 NIN_MS_TXBUF=8 NIN_MS_EXIPUB=1 NIN_MS_SINGLEUS=20 NIN_MS_ADAPT=1000 NIN_MS_SHUTDUMP=1
export NIN_MS_RAMLOG=1 NIN_MS_VISAMPLE=1 NIN_MS_FREEZEDUMP=1 NIN_MS_FZ2=1 NIN_MS_FZ3=1 NIN_MS_FZ4=1 NIN_MS_FZ5=1 NIN_MS_FZ6=1 NIN_MS_DSPFIX=1 NIN_MS_FZ7=1 \
  NIN_MS_STRMGUARD=1 NIN_MS_FZ8=1 NIN_MS_FZ9=1 NIN_MS_PATCHSPACE=1 NIN_MS_FZ10=1 NIN_MS_NULLGUARD=1 NIN_MS_BOOTCNT=1 NIN_MS_HEARTBEAT=1 NIN_MS_CACHEFIX=1
export NIN_MS_PASSHIST=1 NIN_MS_ARMLOG=1 NIN_MS_DIPUMP=1 NIN_MS_GAMELOG=1 NIN_MS_PVBBA=1 NIN_MS_NETAUTO=1 NIN_MS_WLSIG=1 NIN_MS_WIFIFIX=0 \
  NIN_MS_ROAM=1 NIN_MS_WC24=1 NIN_MS_CCHECK=1 NIN_MS_TCP=1
export NIN_MS_PVSNAP= NIN_MS_PVCHK= NIN_MS_IOSMARK= NIN_MS_ONETHREAD=
unset NIN_MS_HB NIN_MS_FZ11 NIN_MS_NETSEG
export MS_SERVER_PLACEHOLDER_OK=1    # the loader that build_wii.sh links on the way is not used (only kernel.zip is), so it needs no server address
export VBBA_OUT=${VBBA_OUT:-/tmp/wii-kernel-out} VBBA_SCRATCH=${VBBA_SCRATCH:-${TMPDIR:-/tmp}/wii-kernel-build-${USER:-user}}
bash "$d/build_wii.sh" build
ls -la "$VBBA_OUT/kernel.zip"
