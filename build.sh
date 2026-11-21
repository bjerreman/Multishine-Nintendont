#!/usr/bin/env bash
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
# Build the Wii loader: the SHELL (Nintendont loader DOL with the embedded kernel), the STUB (the boot.dol a player copies once),
# and TEST-signed update containers (shl-N.mscf, krn-N.mscf) with the published RFC 8032 test key.
#   ./build.sh [OUT]            default OUT=build/wii
# Needs: ./prepare.sh (pinned Nintendont), the pinned Wii toolchain in $DKP_R35 (devkitPPC r35 + libogc 1.8.23 + devkitARM r52,
# README.md; default toolchains/dkp-r35), devkitPPC + libogc in $DEVKITPRO for the stub (default /opt/devkitpro),
# python3.14.  GNU make cannot build in a path with spaces: the scratch tree is $VBBA_SCRATCH (default $TMPDIR/msw-build-$USER).
# Release builds bake the update server (MS_SERVER_IP, MS_SERVER_HOST) and are signed outside this repository (README, "Updates").
set -euo pipefail
cd "$(dirname "$0")"
root=$PWD
out=${1:-$root/build/wii}; [[ $out = /* ]] || out=$root/$out
[[ -f third_party/Nintendont/kernel/Makefile ]] || { echo "third_party/Nintendont is empty: run ./prepare.sh"; exit 2; }
export DKP_R35=${DKP_R35:-$root/toolchains/dkp-r35}
export VBBA_OUT=$out VBBA_SCRATCH=${VBBA_SCRATCH:-${TMPDIR:-/tmp}/msw-build-${USER:-user}}
export NIN_MS_SHELL_SEQ=${NIN_MS_SHELL_SEQ:-1}
[[ -n "${MS_SERVER_IP:-}" ]] || export MS_SERVER_PLACEHOLDER_OK=1     # no server given: the documentation placeholder (never a release)
mkdir -p "$out/stub" "$out/ota" "$out/kernel"
# KERNEL: the Nintendont ARM kernel with the virtual BBA (build_kernel.sh holds the release flag set)
VBBA_OUT=$out/kernel VBBA_SCRATCH=${VBBA_SCRATCH}-krn bash offline/nintendont-vbba/build_kernel.sh
# SHELL: the Nintendont loader with the Multishine screens, around exactly that kernel
bash offline/nintendont-vbba/build_shell.sh --kernel-zip "$out/kernel/kernel.zip"
bash offline/nintendont-vbba/stub/build_stub.sh "$out/shell.dol" "$out/stub" --devkey
python3.14 tools/release/make_shell_ota.py --dol "$out/shell.dol" --seq "$NIN_MS_SHELL_SEQ" --dev --out-dir "$out/ota"
python3.14 tools/release/make_kernel_ota.py --kernel-zip "$out/kernel.zip" --seq "${NIN_MS_KERNEL_SEQ:-1}" --dev --out-dir "$out/ota"
ls -l "$out/shell.dol" "$out/kernel.zip" "$out/stub/boot.dol" "$out"/ota/*.mscf
