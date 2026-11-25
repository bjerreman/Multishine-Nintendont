#!/usr/bin/env bash
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
# Wii build of Nintendont (FIX94 bbc0a20, GPL-2.0) with the Multishine virtual BBA (patches/0001-multishine-vbba.patch
# plus the vBBA core objects linked into the kernel) and our package build/offline/nintendont/package/vbba/.
#   offline/nintendont-vbba/build_wii.sh [build|package|all]     (default all)
# Toolchain: the one README.md describes (devkitPPC r35 + libogc 1.8.23 + devkitARM r52, all under
# $DKP_R35, nothing downloaded by this script).  Same space-free scratch-dir technique as
# the project's other tooling (GNU make cannot build in a path with spaces).  The pinned clone is only read.
# The package is written to build/offline/nintendont/package/vbba/ only (never touches the other package folders).
set -euo pipefail
d=$(cd "$(dirname "$0")" && pwd); root=$(cd "$d/../.." && pwd)
pin=${NIN_SRC:-$root/third_party/Nintendont}; PIN=bbc0a208e81d6ab0a828a431dc0b2ba6fc06aec5
step=${VBBA_STEP:-1}; split=${VBBA_SPLIT:-}; tweak=${VBBA_TWEAK:-b17d}; out=${VBBA_OUT:-$root/build/vbba}; tc=${DKP_R35:-$root/toolchains/dkp-r35}
tweak=${tweak//b17d/plainprobe idle1ms yield1ms inflight1 main1000}   # token b17d = the baseline (the b17b tweaks + main1000, confirmed by the b17d console run)
B=${VBBA_SCRATCH:-${TMPDIR:-/tmp}/vbba-build-$USER}; B=${B%/}
[[ "$B" != *" "* ]] || { echo "VBBA_SCRATCH must not contain spaces: $B"; exit 1; }
[[ -x "$tc/devkitPPC/bin/powerpc-eabi-gcc" && -f "$tc/libogc/lib/wii/libogc.a" && -x "$tc/devkitARM/bin/arm-none-eabi-gcc" ]] || { echo "missing toolchain under $tc (README.md)"; exit 1; }
[[ $(git -C "$pin" rev-parse HEAD) == "$PIN" ]] || { echo "pinned clone is not at $PIN"; exit 1; }
mkdir -p "$B" "$out"
ln -sfn "$tc" "$B/dkp"; ln -sfn "$root" "$B/ms"            # space-free names for make
export DEVKITPRO=$B/dkp DEVKITPPC=$B/dkp/devkitPPC DEVKITARM=$B/dkp/devkitARM
export PATH=$DEVKITPPC/bin:$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH
t=$B/vbba; log=$B/vbba.log; stamp=$B/vbba.stamp
pk=${VBBA_PKG:-$root/build/offline/nintendont/package/vbba}
patch=$d/patches/0001-multishine-vbba.patch
patch5=$d/patches/0005-multishine-diag.patch   # EXI event ring, SD busy tracking, card state for the DIAG lines
patch6=$d/patches/0006-multishine-diag-ppc.patch   # diagnostic only (reads the PowerPC breadcrumb, VI retrace, scene frames and interrupt words into the DIAG lines)
patch7=$d/patches/0007-multishine-dhcp-dns.patch   # (NIN_MS_DNS=1): DHCP option 6 (resolver 1.1.1.1 by default, /vbba.cfg dns=a.b.c.d or dns=none); the router code itself lives in core/ and sends option 6 only when its cfg.dns is set
patch8=$d/patches/0008-multishine-diag-net.patch   # (NIN_MS_NET=1, needs NIN_MS_DNS=1): logging only, guest-net counters in the 10 s and tl@ sets, first distinct guest tx headers
patch9=$d/patches/0009-multishine-diag-online.patch   # (NIN_MS_ONLINE_INFO=<engine build's image.json>, needs 's flags): logging only, the guest's online state read at build-fixed addresses
patch10=$d/patches/0010-multishine-log-reliability.patch   # (NIN_MS_LOG=1): fresh ndebug.log per run (+ prev copy by the loader), run id line, SD write probe with an on-screen warning, LOGOK / LOGEND markers
patch11=$d/patches/0011-multishine-log-probe-fix.patch   # (NIN_MS_LOG=2, includes 0010): the SD probe no longer fails on the loader's missing CTRL_SYNC; 8.3 probe name, ndebug.old, FRESULTs on screen and in RUNID
patch12=$d/patches/0012-multishine-chip-mac-follows-interface.patch   # (NIN_MS_MAC=1): the ip/top interface-MAC query is on and the chip MAC follows it (core vbba_chip_set_mac leaves the PAR registers alone once the guest has reset the chip)
patch15=$d/patches/0015-multishine-stall-watch.patch   # (NIN_MS_STALL=1): main-loop stage breadcrumbs, a watchdog thread that blinks the slot LED (slow = ok, fast = main loop stuck outside FatFs, solid = stuck in FatFs/SD), STALL/STALLMAX log lines; diagnostic only, no behaviour change
patch16=$d/patches/0016-multishine-runtime-switches.patch   # (NIN_MS_RTCFG=1, needs NIN_MS_STALL=1): /vbba.cfg netdelay=SECONDS (net start not before N s after the vBBA started) and nobba=1 (vBBA never started), for one-app diagnostics; absent keys = behaviour
patch17=$d/patches/0017-multishine-ncd-config-wired.patch   # /30 (NIN_MS_WIRED=1, needs NIN_MS_MAC=1): ncd network-config dump, ip/top MAC asked again from the main thread (logged with the time and the config kind), /vbba.cfg net=wired|wifi (opt-in)
patch19=$d/patches/0019-multishine-netearly.patch   # (NIN_MS_NETEARLY=1, needs NIN_MS_OSDUMP=1): /vbba.cfg netearly=SECONDS (1..30): the network IOS start-up is released N s after the vBBA started without the SD-idle wait (netdelay with its own log line)
patch20=$d/patches/0020-multishine-sd-log-diag.patch   # (NIN_MS_SDLOG=1): the dbgprintf Hollywood 0x0D800070 bit 0 test no longer silences the log for good (counted, noted as LOGBIT), SD sector writes retried and SD command errors counted (SDERR line in the 10 s set); behaviour otherwise unchanged
patch21=$d/patches/0021-multishine-sd-probe-nosave.patch   # (NIN_MS_SDPROBE=1, needs NIN_MS_SDLOG=1): /vbba.cfg nosave=1 (GCNCard_Save does not touch the SD, the save counts as done) and sdprobe=1 (the main loop reads SD sector 0 once a second, timed, SDPROBE line every 10 s, slot LED: solid = request pending over 2 s, fast blink = failed or slow)
patch22=$d/patches/0022-multishine-autopoke.patch   # (NIN_MS_AUTOPOKE=1, needs NIN_MS_SDPROBE=1): /vbba.cfg autopoke=SECONDS (default 4, 0 = off): when the guest is silent that long and its newest EXI command was a channel 0 DMA, the transfer-complete interrupt is thrown again (the guest main thread was found asleep in the OS idle loop, waiting for it); the EXI channel table address is taken from the patched EXIDMA; the freeze dump prints the guest TC callback words
patch23=$d/patches/0023-multishine-exidma-keep-cb.patch   # (NIN_MS_EXIFIX=1, needs NIN_MS_AUTOPOKE=1): the guest-side EXIDMA stub keeps an outstanding transfer-complete callback when a DMA without callback (the BBA driver's) comes in, does its OS interrupt-mask read-modify-write with interrupts off; ARM side counts TC interrupts sent with a guest command already waiting; autopoke default 0
patch24=$d/patches/0024-multishine-pc-sample.patch   # (NIN_MS_PCSAMPLE=1, needs NIN_MS_EXIFIX=1): the guest's OS interrupt dispatcher stores where it interrupted the PowerPC (srr0 srr1 lr r1 + real PI INTSR/INTMR + OS mask word) into an uncached ring the ARM prints in the freeze dump
patch25=$d/patches/0025-multishine-gate-handshake.patch   # (NIN_MS_GATE2=1, needs NIN_MS_PCSAMPLE=1): gate=1 releases the net start 3 s after the guest's BBA init handshake (challenge response + rx enable), no SD-idle or register-quiet condition; log lines for the gate transitions
patch26=${NIN_MS_PATCH26:-$d/patches/0026-multishine-release-quiet.patch}   # release profile (NIN_MS_QUIET=1, build_kernel.sh): VBBA_QUIET=1 VBBA_DIAG=0: no periodic DIAG/tl lines, no stall watchdog, no sampler in the guest interrupt path (0024 reverted in the two guest-path files), no experiment switches (/vbba.cfg knows dns= only; the net start gate stays at guest BBA init + 3 s, cap 60 s), no IOS breadcrumb lines, a short shutdown block
patch27=$d/patches/0027-multishine-mac-publish.patch   # (NIN_MS_MACPUB=1): the MAC of the interface in use (ip/top answer) is published to the PowerPC in 4 uncached words (ARM 0x13026680) so Settings > Network > Adapter shows it without a Reconnect; RUNID line also prints the Network Profile in force
patch29=$d/patches/0029-multishine-wifi-roam-off.patch   # (NIN_MS_ROAM=1, needs NIN_MS_WIRED=1 for the interface kind): Wi-Fi roaming scan off (WLC roam_off on /dev/wl0) 2 s after the address is up, re-checked every 10 s; Wi-Fi only (ncd kind 1); ran once on a Wi-Fi Wii (seq 9,: set off, read back OFF);
patch30=$d/patches/0030-multishine-wc24-suspend.patch   # (NIN_MS_WC24=1, needs NIN_MS_ROAM=1: cut from the tree after patch 0029): WiiConnect24 scheduler paused (kd/request ioctl 1) on the helper thread 4 s after the address is up, resumed (ioctl 3) at game exit / shutdown; summary lines in game.log (VBBAWifiGameLog); UNTESTED on a console
patch18=$d/patches/0018-multishine-os-thread-dump-irqpoke.patch   # (NIN_MS_OSDUMP=1, needs NIN_MS_RTCFG=1): DIAG dump of the PowerPC OS thread list (state, priority, wait queue, saved pc/lr/sp) with every stall DIAG set; /vbba.cfg irqpoke=SECONDS experiment (off by default: one spurious EXI TC interrupt after that long a guest silence)
patch4=$d/patches/0004-multishine-sd-counter.patch   # SD command counter for the activity-based network release
patch3=$d/patches/0003-multishine-meta-profile.patch   # HBC meta.xml only into our own launch dir (the Network Profile option is the original one again since )
rel=${NIN_MS_RELEASE:-0}   # the Multishine release shown in the HBC version text (build_kernel.sh and build_shell.sh set it)
[[ -n "$rel" ]] || { echo "no release number: set NIN_MS_RELEASE"; exit 1; }
if [[ "${NIN_MS_QUIET:-}" == 1 ]]; then   # release candidate: the version text is "Wii <version>+<build id>"; the id = SHA-1 prefix over the whole patch series (the same source = the same id)
  rcid=$(cat "$d"/patches/0*.patch | shasum | cut -c1-7); rel="Wii ${NIN_MS_RC_VERSION:-1.0-alpha}+$rcid"
  [[ -n "${NIN_MS_RC_ID:-}" ]] && { rcid=$NIN_MS_RC_ID; rel="Wii ${NIN_MS_RC_VERSION:-1.0-alpha}+$rcid"; }   # same, for the release-candidate id
fi
patch2=$d/patches/0002-multishine-paths.patch   # own config file /multishine/nincfg.bin, own card folder /multishine/saves, 1019-block default

build() {
  rm -rf "$t"; mkdir -p "$t"; : > "$log"; : > "$stamp"
  git -C "$pin" archive "$PIN" | tar x -C "$t"
  (cd "$t" && git apply --whitespace=nowarn "$patch"); echo "applied $(basename "$patch")" | tee -a "$log"
  (cd "$t" && git apply --whitespace=nowarn "$patch2"); echo "applied $(basename "$patch2")" | tee -a "$log"
  (cd "$t" && git apply --whitespace=nowarn "$patch3"); echo "applied $(basename "$patch3")" | tee -a "$log"
  (cd "$t" && git apply --whitespace=nowarn "$patch4"); echo "applied $(basename "$patch4")" | tee -a "$log"
  (cd "$t" && git apply --whitespace=nowarn "$patch5"); echo "applied $(basename "$patch5")" | tee -a "$log"
  (cd "$t" && git apply --whitespace=nowarn "$patch6"); echo "applied $(basename "$patch6")" | tee -a "$log"
  if [[ "${NIN_MS_DNS:-}" == 1 ]]; then (cd "$t" && git apply --whitespace=nowarn "$patch7"); echo "applied $(basename "$patch7")" | tee -a "$log"; fi
  if [[ "${NIN_MS_NET:-}" == 1 ]]; then (cd "$t" && git apply --whitespace=nowarn "$patch8"); echo "applied $(basename "$patch8")" | tee -a "$log"; fi
  if [[ "${NIN_MS_MAC:-}" == 1 ]]; then (cd "$t" && git apply --whitespace=nowarn "$patch12"); echo "applied $(basename "$patch12")" | tee -a "$log"; fi
  if [[ "${NIN_MS_LOG:-}" == 1 || "${NIN_MS_LOG:-}" == 2 ]]; then (cd "$t" && git apply --whitespace=nowarn "$patch10"); echo "applied $(basename "$patch10")" | tee -a "$log"; fi
  if [[ "${NIN_MS_LOG:-}" == 2 ]]; then (cd "$t" && git apply --whitespace=nowarn "$patch11"); echo "applied $(basename "$patch11")" | tee -a "$log"; fi
  if [[ -n "${NIN_MS_ONLINE_INFO:-}" ]]; then   # the addresses of the engine's status blocks, from that build's image.json (they change with every engine build; valid for that build only)
    (cd "$t" && git apply --whitespace=nowarn "$patch9"); echo "applied $(basename "$patch9")" | tee -a "$log"
    python3.14 - "$NIN_MS_ONLINE_INFO" "$t/kernel/vbba_online.h" "${NIN_MS_ONLINE_REL:?set NIN_MS_ONLINE_REL (the engine release these addresses belong to)}" <<'PY' | tee -a "$log"
import json, sys
j = json.load(open(sys.argv[1])); out = sys.argv[2]; rel = sys.argv[3]
a = {k: int(j[k], 16) for k in ("status_addr", "od_status_addr", "on_status_addr", "onc_status_addr")}
open(out, "w").write("""/* generated by build_wii.sh from %s (engine build %s, release %s) */
#ifndef VBBA_ONLINE_H
#define VBBA_ONLINE_H
#define VBBA_ONL 1
#define VBBA_ONL_S3 0x%08Xu
#define VBBA_ONL_OD 0x%08Xu
#define VBBA_ONL_ON 0x%08Xu
#define VBBA_ONL_ONC 0x%08Xu
#define VBBA_ONL_REL "%s"
#define VBBA_ONL_BUILD "%s"
#endif
""" % (sys.argv[1].split("/")[-2] + "/image.json", j["build_id"], rel, a["status_addr"], a["od_status_addr"], a["on_status_addr"], a["onc_status_addr"], rel, j["build_id"]))
print("online addresses for engine build %s (release %s): s3 %08X od %08X on %08X onc %08X" % (j["build_id"], rel, a["status_addr"], a["od_status_addr"], a["on_status_addr"], a["onc_status_addr"]))
PY
  fi
  if [[ "${NIN_MS_STALL:-}" == 1 ]]; then (cd "$t" && git apply --whitespace=nowarn "$patch15"); echo "applied $(basename "$patch15")" | tee -a "$log"; fi
  if [[ "${NIN_MS_RTCFG:-}" == 1 ]]; then (cd "$t" && git apply --whitespace=nowarn "$patch16"); echo "applied $(basename "$patch16")" | tee -a "$log"; fi
  if [[ "${NIN_MS_WIRED:-}" == 1 ]]; then [[ "${NIN_MS_MAC:-}" == 1 ]] || { echo "NIN_MS_WIRED needs NIN_MS_MAC"; exit 1; }; (cd "$t" && git apply --whitespace=nowarn "$patch17"); echo "applied $(basename "$patch17")" | tee -a "$log"; fi
  if [[ "${NIN_MS_OSDUMP:-}" == 1 ]]; then [[ "${NIN_MS_RTCFG:-}" == 1 ]] || { echo "NIN_MS_OSDUMP needs NIN_MS_RTCFG"; exit 1; }; (cd "$t" && git apply --whitespace=nowarn "$patch18"); echo "applied $(basename "$patch18")" | tee -a "$log"; fi
  if [[ "${NIN_MS_NETEARLY:-}" == 1 ]]; then [[ "${NIN_MS_OSDUMP:-}" == 1 ]] || { echo "NIN_MS_NETEARLY needs NIN_MS_OSDUMP"; exit 1; }; (cd "$t" && git apply --whitespace=nowarn "$patch19"); echo "applied $(basename "$patch19")" | tee -a "$log"; fi
  if [[ "${NIN_MS_SDLOG:-}" == 1 ]]; then (cd "$t" && git apply --whitespace=nowarn "$patch20"); echo "applied $(basename "$patch20")" | tee -a "$log"; fi
  if [[ "${NIN_MS_SDPROBE:-}" == 1 ]]; then [[ "${NIN_MS_SDLOG:-}" == 1 ]] || { echo "NIN_MS_SDPROBE needs NIN_MS_SDLOG"; exit 1; }; (cd "$t" && git apply --whitespace=nowarn "$patch21"); echo "applied $(basename "$patch21")" | tee -a "$log"; fi
  if [[ "${NIN_MS_AUTOPOKE:-}" == 1 ]]; then [[ "${NIN_MS_SDPROBE:-}" == 1 ]] || { echo "NIN_MS_AUTOPOKE needs NIN_MS_SDPROBE"; exit 1; }; (cd "$t" && git apply --whitespace=nowarn "$patch22"); echo "applied $(basename "$patch22")" | tee -a "$log"; fi
  if [[ "${NIN_MS_EXIFIX:-}" == 1 ]]; then [[ "${NIN_MS_AUTOPOKE:-}" == 1 ]] || { echo "NIN_MS_EXIFIX needs NIN_MS_AUTOPOKE"; exit 1; }; (cd "$t" && git apply --whitespace=nowarn "$patch23"); echo "applied $(basename "$patch23")" | tee -a "$log"; fi
  if [[ "${NIN_MS_PCSAMPLE:-}" == 1 ]]; then [[ "${NIN_MS_EXIFIX:-}" == 1 ]] || { echo "NIN_MS_PCSAMPLE needs NIN_MS_EXIFIX"; exit 1; }; (cd "$t" && git apply --whitespace=nowarn "$patch24"); echo "applied $(basename "$patch24")" | tee -a "$log"; fi
  if [[ "${NIN_MS_GATE2:-}" == 1 ]]; then [[ "${NIN_MS_PCSAMPLE:-}" == 1 ]] || { echo "NIN_MS_GATE2 needs NIN_MS_PCSAMPLE"; exit 1; }; (cd "$t" && git apply --whitespace=nowarn "$patch25"); echo "applied $(basename "$patch25")" | tee -a "$log"; fi
  if [[ "${NIN_MS_QUIET:-}" == 1 ]]; then [[ "${NIN_MS_GATE2:-}" == 1 && "${NIN_MS_PCSAMPLE:-}" == 1 && -z "${NIN_MS_ONLINE_INFO:-}" ]] || { echo "NIN_MS_QUIET needs NIN_MS_GATE2=1 and NIN_MS_PCSAMPLE=1 without NIN_MS_ONLINE_INFO (build_kernel.sh)"; exit 1; }; (cd "$t" && git apply --whitespace=nowarn "$patch26"); echo "applied $(basename "$patch26")" | tee -a "$log"; fi
  if [[ "${NIN_MS_MACPUB:-}" == 1 ]]; then (cd "$t" && git apply --whitespace=nowarn "$patch27"); echo "applied $(basename "$patch27")" | tee -a "$log"; fi
  patch28=$d/patches/0028-multishine-kernel-ota.patch   # (always on): the loader starts a verified SD:/multishine/kernel.bin instead of the embedded kernel and its Update key fetches signed kernels from OUR server; the stock GitHub updater is replaced
  (cd "$t" && git apply --whitespace=nowarn "$patch28"); echo "applied $(basename "$patch28")" | tee -a "$log"
  patch31=$d/patches/0031-multishine-boot-timing.patch   # (always on, loader only): 'T boot <step> <ms>' lines in the shell's SD log (ota.log) for every boot step incl. the SD and USB mounts
  if [[ "${NIN_MS_BOOTTIME:-}" == 1 ]]; then (cd "$t" && git apply --whitespace=nowarn "$patch31"); echo "applied $(basename "$patch31")" | tee -a "$log"; fi   # off by default in this tree (the Wii loader 173 shell does not carry it)
  patch32=$d/patches/0032-multishine-skip-usb-mount.patch   # (always on, loader only): the 10 s USB mount wait is skipped when the saved SD game is proven good (ms_usb_skip in ota/ms_disc_gate.c; SD:/multishine/usb.on forces the mount)
  if [[ "${NIN_MS_USBSKIP:-}" == 1 ]]; then (cd "$t" && git apply --whitespace=nowarn "$patch32"); echo "applied $(basename "$patch32")" | tee -a "$log"; fi   # off by default in this tree (needs ms_usb_skip in ota/ms_disc_gate.c, which the Wii does not have)
  [[ "${NIN_MS_OTA_DEVKEY:-}" != 1 || "${NIN_MS_QUIET:-}" != 1 ]] || { echo "NIN_MS_OTA_DEVKEY (the published dev test key) is for lab builds: refused with NIN_MS_QUIET"; exit 1; }
  python3.14 "$d/ota/stage_loader.py" "$root" "$t" ${NIN_MS_OTA_DEVKEY:+--devkey} | tee -a "$log"
  python3.14 "$d/ms_meta.py" header "$t" "$rel"; echo "meta texts + release $rel" | tee -a "$log"
  if [[ -n "${NIN_MS_NETDELAY:-}" ]]; then   # delay test build: no network IOS call until N seconds after the vBBA started
    [[ $(grep -c "^VBBA_FLAGS := -std=gnu99" "$t/kernel/Makefile") == 1 ]]
    perl -pi -e "s/^(VBBA_FLAGS := -std=gnu99)/\$1 -DVBBA_NET_DELAY_S=$NIN_MS_NETDELAY/" "$t/kernel/Makefile"
    grep -q "^VBBA_FLAGS := -std=gnu99.*-DVBBA_NET_DELAY_S=$NIN_MS_NETDELAY" "$t/kernel/Makefile" && echo "NETDELAY test build: $NIN_MS_NETDELAY s" | tee -a "$log"
  fi
  if [[ -n "${NIN_MS_SINGLEUS:-}" ]]; then   # test build: main-loop sleep per pass in single-context mode (default 300 us) -- every EXI command of the guest waits for one pass
    [[ $(grep -c "^VBBA_FLAGS := -std=gnu99" "$t/kernel/Makefile") == 1 ]]
    perl -pi -e "s/^(VBBA_FLAGS := -std=gnu99)/\$1 -DVBBA_SINGLE_US=$NIN_MS_SINGLEUS/" "$t/kernel/Makefile"
    grep -q "^VBBA_FLAGS := -std=gnu99.*-DVBBA_SINGLE_US=$NIN_MS_SINGLEUS" "$t/kernel/Makefile" && echo "SINGLEUS test build: $NIN_MS_SINGLEUS us" | tee -a "$log"
  fi
  if [[ "${NIN_MS_EXIPUB:-}" == 1 ]]; then   # test build: the ARM publishes "a transfer-complete interrupt is held back or not yet handled" (EXI_IRQ) in the guest-visible word 0x13026690 (PPC 0xD3026690); the engine skips its BBA slice while it is set (findings 38-41)
    [[ $(grep -c "EXI_IRQ = true;" "$t/kernel/EXI.c") == 3 && $(grep -c "^	EXI_IRQ = false;" "$t/kernel/EXI.c") == 1 ]] || { echo "EXI.c: unexpected EXI_IRQ assignments"; exit 1; }
    perl -pi -e 's/EXI_IRQ = true;/EXI_IRQ = true; exiPubSet(1);/g; s/^\tEXI_IRQ = false;/\tEXI_IRQ = false; exiPubSet(0);/mg' "$t/kernel/EXI.c"
    perl -0pi -e 's/(bool EXI_IRQ = false;)/$1\nstatic void exiPubSet(u32 v) { write32(0x13026694, 0x45585042); write32(0x13026690, v); sync_after_write((void*)0x13026690, 32); }   \/* Multishine: guest-visible copy of EXI_IRQ *\//' "$t/kernel/EXI.c"
    perl -0pi -e 's/(static void exiPubSet\(u32 v\)[^\n]*\n)/$1static u32 exiCardN;   \/* Multishine: card-side (not device 2) commands counted, published at 0x13026698 so the guest can see that a card transfer is going on *\/\nstatic void exiCardCnt(u32 command) { u32 d = ((command >> 24) == 0x10) ? ((command >> 8) & 0xFF) : EXIDeviceSelect[0]; if (d != 2) { exiCardN++; write32(0x13026698, exiCardN); sync_after_write((void*)0x13026690, 32); } }\n/; s/(\t\tEXIDiagRec\( command \);)/$1\n\t\texiCardCnt( command );/' "$t/kernel/EXI.c"
    grep -q "exiCardCnt( command )" "$t/kernel/EXI.c" || { echo "EXI.c: EXIDiagRec call not found"; exit 1; }
    grep -q "exiPubSet(1)" "$t/kernel/EXI.c" && grep -q "static void exiPubSet" "$t/kernel/EXI.c" && echo "EXIPUB test build" | tee -a "$log"
  fi
  if [[ -n "${NIN_MS_TXCAP:-}" ]]; then   # test build: NIN_MS_TXCAP=N sendto requests in flight in IOS and 16 send buffers (1128 Wii log: 766 of 1753 game packets dropped by the 8-buffer / 1-in-flight queue; finding 45)
    [[ $(grep -c "^VBBA_FLAGS := -std=gnu99" "$t/kernel/Makefile") == 1 ]]
    perl -pi -e "s/^(VBBA_FLAGS := -std=gnu99)/\$1 -DVBBA_TP_IOS_TX=${NIN_MS_TXBUF:-16} -DVBBA_TP_IOS_TX_INFLIGHT=$NIN_MS_TXCAP/" "$t/kernel/Makefile"
    grep -q "^VBBA_FLAGS := -std=gnu99.*-DVBBA_TP_IOS_TX=${NIN_MS_TXBUF:-16} -DVBBA_TP_IOS_TX_INFLIGHT=$NIN_MS_TXCAP" "$t/kernel/Makefile" && echo "TXCAP test build: $NIN_MS_TXCAP in flight, 16 buffers" | tee -a "$log"
  fi
  if [[ "${NIN_MS_RXPUB:-}" == 1 ]]; then   # test build: every main-loop pass the ARM publishes 0x5258<<16 | the chip's RWP at 0x13026670 (PPC 0xD3026670): the engine answers idle receive polls from it without EXI traffic (finding 47)
    [[ $(grep -c "^	vg_jc_drain();" "$t/kernel/vbba_glue.c") == 1 ]] || { echo "vbba_glue.c: vg_jc_drain call not unique"; exit 1; }
    perl -0pi -e 's/(\tvg_jc_drain\(\);[^\n]*\n)/$1\t{ static u32 lastpub; u32 v = 0x52580000u | ((u32)vbba_chip_rwp(&V->chip) & 0xFFFu); if (V && v != lastpub) { lastpub = v; write32(0x13026670, v); sync_after_write((void*)0x13026670, 32); } }   \/* Multishine: chip RWP for the guest *\/\n/' "$t/kernel/vbba_glue.c"
    grep -q "chip RWP for the guest" "$t/kernel/vbba_glue.c" && echo "RXPUB test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_BSSHEAP:-}" == 1 ]]; then   # freeze-hunt c2: the vBBA heap and its status block live in kernel .bss, nothing is written to 0x131xxxxx (VBBA_OFF 128); the SQ line then reads zeros (finding 49)
    [[ $(grep -c "^VBBA_FLAGS := -std=gnu99" "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_OFF=128/' "$t/kernel/Makefile"
    grep -q "^VBBA_FLAGS := -std=gnu99.*-DVBBA_OFF=128" "$t/kernel/Makefile" && echo "BSSHEAP test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_BOOTCNT:-}" == 1 ]]; then   # test build: every kernel start appends one line "boot <HW_TIMER ticks>" to /bootloop.log on the SD (the bootloop engine reboots the game every 35 s, each reboot starts the kernel again): lines = boots, the last line against the log's idle dump = boots before a hang (finding 50)
    grep -q 'dbgprintf("RUNID: game' "$t/kernel/main.c" || { echo "main.c: RUNID game line not found"; exit 1; }
    perl -pi -e 's/^(\tdbgprintf\("RUNID: game[^\n]*\n)/$1\t{ FIL bl; if (f_open_char(&bl, "\/bootloop.log", FA_WRITE|FA_OPEN_ALWAYS) == FR_OK) { char bb[16]; UINT bw; u32 bv = read32(HW_TIMER), bk; f_lseek(&bl, f_size(&bl)); bb[0] = 0x42; for (bk = 0; bk < 8; bk++) { u32 bd = (bv >> (28 - 4 * bk)) \& 15; bb[1 + bk] = (char)(bd < 10 ? 0x30 + bd : 0x57 + bd); } bb[9] = 13; bb[10] = 10; f_write(&bl, bb, 11, \&bw); f_close(&bl); } }   \/* Multishine: boot log *\/\n/' "$t/kernel/main.c"
    grep -q "bootloop.log" "$t/kernel/main.c" && echo "BOOTCNT test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_VISAMPLE:-}" == 1 ]]; then   # test build (finding 63): each PowerPC interrupt sample also stores the VI display interrupt register 0 (0xCC002030, read by the PowerPC: safe) at +0x1C; SHUTDUMP prints it
    grep -c "stw		r5,	0x6738(r6)" "$t/kernel/asm/FakeInterrupt.S" | grep -q '^1$' || { echo "FakeInterrupt.S: sample anchor not found (needs NIN_MS_PCSAMPLE=1 without the 0026 FakeInterrupt hunk)"; exit 1; }
    perl -pi -e 's/^(\tstw\t\tr5,\t0x6738\(r6\)\n)/$1\tlis\t\tr5,\t0xCC00\n\tlwz\t\tr5,\t0x2030(r5)\n\tstw\t\tr5,\t0x673C(r6)\n/' "$t/kernel/asm/FakeInterrupt.S"
    grep -q "0x673C" "$t/kernel/asm/FakeInterrupt.S" && echo "VISAMPLE test build" | tee -a "$log"
  fi
  if [[ -n "${NIN_MS_ADAPT:-}" ]]; then   # test build (findings 58,  22): the main loop sleeps NIN_MS_SINGLEUS (20 us) only while the guest is in an EXI burst (a command served within the last 300 us) and NIN_MS_ADAPT us (1000) otherwise, so the IOS network threads get clear gaps between the bursts (b17d: sendto 27 -> 5 ms at a long sleep) while EXI keeps its 10 us per operation
    grep -c "^u32 VBBAMainDelay(u32 us)$" "$t/kernel/vbba_glue.c" | grep -q '^1$' || { echo "vbba_glue.c: VBBAMainDelay anchor not unique"; exit 1; }
    python3 - "$t/kernel/vbba_glue.c" "$NIN_MS_ADAPT" "${NIN_MS_HEARTBEAT:-0}" <<'PYE'
import sys
p, us, hb = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
a = "u32 VBBAMainDelay(u32 us)\n{\n#if VBBA_SINGLE\n\treturn (vgStarted && VBBA_SINGLE_US > us) ? (u32)VBBA_SINGLE_US : us;"
assert s.count(a) == 1
b = ("extern u32 exiServed[4][3];\nstatic u32 vgAdN, vgAdT;\nu32 VBBAMainDelay(u32 us)\n{\n#if VBBA_SINGLE\n"
     "\t{ u32 n = 0, i, j, t = read32(HW_TIMER), d;\n"
     "\t  if (!vgStarted) return us;\n"
     "\t  { extern void vgFreezeWatch(void); vgFreezeWatch(); }\n"
     + ("\t  { static u32 hbT, hbL; if ((u32)(t - hbT) > 949000u) { hbT = t; hbL ^= 1u; if (hbL) set32(HW_GPIO_OUT, GPIO_SLOT_LED); else clear32(HW_GPIO_OUT, GPIO_SLOT_LED); } }\n" if hb == "1" else "") +
     "\t  d = (VBBA_SINGLE_US > us) ? (u32)VBBA_SINGLE_US : us;\n"
     "\t  for (i = 0; i < 4; i++) for (j = 0; j < 3; j++) n += exiServed[i][j];\n"
     "\t  if (n != vgAdN) { vgAdN = n; vgAdT = t; }\n"
     "\t  return ((u32)(t - vgAdT) > 570u) ? " + us + "u : d; }")
open(p, 'w').write(s.replace(a, b))
PYE
    grep -q "vgAdN" "$t/kernel/vbba_glue.c" && echo "ADAPT test build: ${NIN_MS_ADAPT} us when idle" | tee -a "$log"
  fi
  if [[ "${NIN_MS_PASSHIST:-}" == 1 ]]; then   #  (needs NIN_MS_ADAPT): ARM main-loop pass accounting in the published status block's spare words 121..125
    # (struct vg_status pad[5]): 121 IsWiiU (1 on a Wii U: the engine's game record reports platform 3), 122 passes >= 5 ms, 123 passes >= 20 ms, 124 longest pass in ms since boot, 125 passes.  A pass is the time
    # between two VBBAMainDelay calls (one per main-loop pass: the previous sleep + EXI / DI / HID / SD work), HW_TIMER ticks at 1.898 MHz.  Four adds per pass; nothing else changes.  The engine's readout IO line and
    # on_backend.c gr_platform read the words; a kernel without this leaves them 0.
    [[ -n "${NIN_MS_ADAPT:-}" ]] || { echo "NIN_MS_PASSHIST needs NIN_MS_ADAPT"; exit 1; }
    python3 - "$t/kernel/vbba_glue.c" <<'PYE'
import sys
p = sys.argv[1]
s = open(p).read()
a = "\t  if (n != vgAdN) { vgAdN = n; vgAdT = t; }\n"
assert s.count(a) == 1
b = a + ("\t  if (vgPsT) { u32 g = t - vgPsT; if (g >= 9490u) vgPsH[0]++; if (g >= 37960u) vgPsH[1]++; g /= 1898u; if (g > vgPsH[2]) vgPsH[2] = g; }\n"
         "\t  vgPsT = t; vgPsH[3]++;\n")
s = s.replace(a, b)
a2 = "static struct vg_status *vst;\n"
assert s.count(a2) == 1
s = s.replace(a2, a2 + "static u32 vgPsT, vgPsH[4];   /* PASSHIST: passes >= 5 ms, >= 20 ms, longest ms, passes (written by VBBAMainDelay, read by vg_status_update) */\n")
a3 = "\tvst->tx_inflight_now = inflight;\n"
assert s.count(a3) == 1
s = s.replace(a3, a3 + "\tvst->pad[0] = IsWiiU() ? 1u : 0u; vst->pad[1] = vgPsH[0]; vst->pad[2] = vgPsH[1]; vst->pad[3] = vgPsH[2]; vst->pad[4] = vgPsH[3];   /* PASSHIST: words 121..125 */\n")
s = "#define MS_PASSHIST_DUMP 1   /* the soft power-off / freeze dump prints the four counters (vgShutdumpNet) */\n" + s
open(p, 'w').write(s)
PYE
    grep -q "vgPsH" "$t/kernel/vbba_glue.c" && echo "PASSHIST test build: main-loop pass counters in status words 121..125" | tee -a "$log"
  fi
  if [[ "${NIN_MS_SHUTDUMP:-}" == 1 ]]; then   # test build (finding 56): the soft power-off prints the PowerPC interrupt-sample ring (the legacy VI / PI registers are NOT readable from the ARM: q3 hung), so a freeze can be read from a quiet kernel with no cost during play
    grep -c 'dbgprintf("Got Shutdown button call\\n");' "$t/kernel/common.c" | grep -q '^1$' || { echo "common.c: Shutdown anchor not unique"; exit 1; }
    python3 - "$t/kernel/common.c" <<'PYE'
import sys
p = sys.argv[1]
s = open(p).read()
a = 'dbgprintf("Got Shutdown button call\\n");'
b = a + r"""
	{ u32 si;
	dbgprintf("SHUTDUMP: tick %u hw70 %08X (reading the legacy VI / PI registers from the ARM hung the shutdown in 1138-q3: not read)\r\n", read32(HW_TIMER), read32(0x0D800070));
	dbgprintf("SHUTDUMP: sample counter %u\r\n", read32(0x13026700));
	{ u32 ht = read32(0x004D7378) & 0x01FFFFFFu; dbgprintf("SHUTDUMP: game retraceCount %u | OS mask words C4 %08X C8 %08X | last interrupt %04X srr0 %08X | handler table %08X VI handler %08X\r\n", read32(0x004D7420), read32(0x000000C4), read32(0x000000C8), read32(0x004D7380) >> 16, read32(0x004D737C), read32(0x004D7378), (ht ? read32(ht + 24 * 4) : 0)); }
	for (si = 0; si < 4; si++) { u32 o = 0x13026720 + 0x20 * si; dbgprintf("SHUTDUMP: sample %u srr0 %08X srr1 %08X lr %08X r1 %08X intsr %08X intmr %08X osmask %08X vi_di0 %08X\r\n", si, read32(o), read32(o + 4), read32(o + 8), read32(o + 12), read32(o + 16), read32(o + 20), read32(o + 24), read32(o + 28)); } }"""
assert s.count(a) == 1
open(p, 'w').write(s.replace(a, b))
PYE
    python3 - "$t/kernel/vbba_glue.c" "$t/kernel/common.c" <<'PYE2'
import sys
g, c = sys.argv[1], sys.argv[2]
s = open(g).read()
s += r"""

/* test build (SHUTDUMP): the first distinct headers the guest transmitted (ethertype, protocol, destination address and port, source port), so the soft power-off log says where the Wii sent its datagrams */
void vgShutdumpNet(void)
{
	u32 i;
	if (!V) return;
	for (i = 0; i < V->router.txhdr_n; i++) {
		const struct vbba_txhdr *o = &V->router.txhdr[i];
		dbgprintf("SHUTDUMP: tx hdr %u eth %04X proto %u dst %u.%u.%u.%u:%u from port %u n %u\r\n", i, o->eth, o->proto, o->dip >> 24, (o->dip >> 16) & 255u, (o->dip >> 8) & 255u, o->dip & 255u, o->dport, o->sport, o->n);
	}
	for (i = 0; i < V->router.rxsrc_n; i++) dbgprintf("SHUTDUMP: rx src %u from %u.%u.%u.%u:%u n %u\r\n", i, V->router.rxsrc[i].ip >> 24, (V->router.rxsrc[i].ip >> 16) & 255u, (V->router.rxsrc[i].ip >> 8) & 255u, V->router.rxsrc[i].ip & 255u, (u32)V->router.rxsrc[i].port, V->router.rxsrc[i].n);
	dbgprintf("SHUTDUMP: router maps opened %u evicted %u failed %u full %u peak %u of %u | last evicted port %u quiet %u ms\r\n", V->router.st.maps_opened, V->router.st.maps_evicted, V->router.st.maps_failed, V->router.st.maps_full, V->router.st.maps_peak, (u32)VBBA_MAPS, V->router.st.evict_port, V->router.st.evict_idle_ms);
	for (i = 0; i < VBBA_MAPS; i++) if (V->router.map[i].used) dbgprintf("SHUTDUMP: map %u guest port %u handle %d bound port %u\r\n", i, V->router.map[i].gport, V->router.map[i].h, V->router.tp && V->router.tp->bound_port ? V->router.tp->bound_port(V->router.tp->ctx, V->router.map[i].h) : 0);
	dbgprintf("SHUTDUMP: router rx delivered %u unsolicited %u (newest from %u.%u.%u.%u:%u) no-lease %u err %u trunc %u\r\n", V->router.st.rx_dgrams, V->router.st.rx_unsolicited, V->router.st.unsol_ip >> 24, (V->router.st.unsol_ip >> 16) & 255u, (V->router.st.unsol_ip >> 8) & 255u, V->router.st.unsol_ip & 255u, (u32)V->router.st.unsol_port, V->router.st.rx_no_lease, V->router.st.rx_err, V->router.st.rx_trunc);
#ifdef MS_PASSHIST_DUMP
	dbgprintf("SHUTDUMP: PASSHIST passes>=5ms %u passes>=20ms %u longest_ms %u passes %u\r\n", vgPsH[0], vgPsH[1], vgPsH[2], vgPsH[3]);
#endif
}
"""
open(g, 'w').write(s)
t = open(c).read()
a = '\tdbgprintf("SHUTDUMP: sample counter %u\\r\\n", read32(0x13026700));'
assert t.count(a) == 1
t = t.replace(a, a + '\n\t{ extern void vgShutdumpNet(void); vgShutdumpNet(); }')
open(c, 'w').write(t)
PYE2
    grep -q SHUTDUMP "$t/kernel/common.c" && echo "SHUTDUMP test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_TCP:-}" == 1 ]]; then   # TCP through the vBBA (core/vbba_tcp.c): the router splices the guest's TCP connections (Slippi matchmaker TLS WebSocket) onto IOS stream sockets (platform/nintendont/vbba_tp_ios.c); new IOS encodings here, exempt from the request watchdog; SHUTDUMP tcp lines
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_TCP=1/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99 -DVBBA_TCP=1' "$t/kernel/Makefile" || { echo "TCP: Makefile flag not applied"; exit 1; }
    python3 - "$t/kernel/vbba_glue.c" <<'PYTCP'
import sys
p = sys.argv[1]; s = open(p).read()
a = "\tcase VBBA_IOS_CLOSE:            /* so_close 0x03 */"
assert s.count(a) == 1, "TCP encoder anchor"
enc = (
 "\tcase VBBA_IOS_TCPSOCKET:        /* so_socket 0x0F: AF_INET 2, SOCK_STREAM 1, 0 */\n"
 "\t\ts->params[0] = 2; s->params[1] = 1; s->params[2] = 0;\n"
 "\t\tsync_after_write(s, sizeof(*s));\n"
 "\t\trc = IOS_IoctlAsync(ipFd, 0x0F, s->params, 12, NULL, 0, vgQueue, &s->msg);\n"
 "\t\tbreak;\n"
 "\tcase VBBA_IOS_CONNECT:          /* so_connect 0x04 (libogc net_connect): {fd, has_addr 1, sockaddr {len 8, AF_INET, port, addr}} = 36 bytes, the bind layout */\n"
 "\t\ts->params[0] = (u32)r->fd; s->params[1] = 1;\n"
 "\t\ts->params[2] = (8u << 24) | (2u << 16) | r->port;\n"
 "\t\ts->params[3] = r->ip;\n"
 "\t\tsync_after_write(s, sizeof(*s));\n"
 "\t\trc = IOS_IoctlAsync(ipFd, 0x04, s->params, 36, NULL, 0, vgQueue, &s->msg);\n"
 "\t\tbreak;\n"
 "\tcase VBBA_IOS_SEND:             /* so_sendto 0x0D without a destination (libogc net_send): vec0 data, vec1 {fd, flags 0, has_dest 0, ...} */\n"
 "\t\ts->params[0] = (u32)r->fd; s->params[1] = 0; s->params[2] = 0;\n"
 "\t\ts->vec[0].data = r->buf; s->vec[0].len = r->len;\n"
 "\t\ts->vec[1].data = s->params; s->vec[1].len = 40;\n"
 "\t\tsync_after_write(r->buf, round32(r->len));\n"
 "\t\tsync_after_write(s, sizeof(*s));\n"
 "\t\trc = IOS_IoctlvAsync(ipFd, 0x0D, 2, 0, s->vec, vgQueue, &s->msg);\n"
 "\t\tbreak;\n"
 "\tcase VBBA_IOS_RECV:             /* so_recvfrom 0x0C without an address (libogc net_recv): vec0 {fd, flags}, out vec1 buffer, out vec2 empty */\n"
 "\t\ts->params[0] = (u32)r->fd; s->params[1] = 0;\n"
 "\t\ts->vec[0].data = s->params;     s->vec[0].len = 8;\n"
 "\t\ts->vec[1].data = r->buf;        s->vec[1].len = r->len;\n"
 "\t\ts->vec[2].data = NULL;          s->vec[2].len = 0;\n"
 "\t\tsync_after_write(s, sizeof(*s));\n"
 "\t\trc = IOS_IoctlvAsync(ipFd, 0x0C, 1, 2, s->vec, vgQueue, &s->msg);\n"
 "\t\tbreak;\n"
 "\tcase VBBA_IOS_TCPPOLL:          /* so_poll 0x0B, one socket, timeout 0, events = r->len; the completion hands back revents */\n"
 "\t\ts->params[0] = 0; s->params[1] = 0;\n"
 "\t\ts->params[8] = (u32)r->fd; s->params[9] = r->len; s->params[10] = 0;\n"
 "\t\tsync_after_write(s, sizeof(*s));\n"
 "\t\trc = IOS_IoctlAsync(ipFd, 0x0B, s->params, 8, &s->params[8], 12, vgQueue, &s->msg);\n"
 "\t\tbreak;\n"
 "\tcase VBBA_IOS_SHUTDOWN:         /* so_shutdown 0x0E (libogc net_shutdown): {fd, how} */\n"
 "\t\ts->params[0] = (u32)r->fd; s->params[1] = r->len;\n"
 "\t\tsync_after_write(s, sizeof(*s));\n"
 "\t\trc = IOS_IoctlAsync(ipFd, 0x0E, s->params, 8, NULL, 0, vgQueue, &s->msg);\n"
 "\t\tbreak;\n")
s = s.replace(a, enc + a)
b = "\t\t} else if (r->op == VBBA_IOS_RECVFROM && r->result > 0) {"
assert s.count(b) == 1, "TCP completion anchor"
s = s.replace(b, "\t\t} else if (r->op == VBBA_IOS_TCPPOLL) {     /* revents (or the error) */\n\t\t\tvg_inval(s->params, 64);\n\t\t\tif (r->result >= 0) r->result = (s32)(s->params[10] & 0xFFFFu);\n\t\t} else if (r->op == VBBA_IOS_RECV && r->result > 0) {\n\t\t\tvg_inval(r->buf, (u32)r->result > r->len ? r->len : (u32)r->result);\n" + b)
c = "\t\tif (r->op == VBBA_IOS_RECVFROM && !VBBA_TP_RXPOLL) continue;"
assert s.count(c) == 1, "TCP watchdog anchor"
s = s.replace(c, c + "\n\t\tif (r->op >= VBBA_IOS_TCPSOCKET && r->op <= VBBA_IOS_SHUTDOWN) continue;      /* a slow stream request must never take the network down; the transport has its own deadlines */")
d = '\tdbgprintf("SHUTDUMP: router maps opened %u evicted %u failed %u full %u peak %u of %u | last evicted port %u quiet %u ms\\r\\n", V->router.st.maps_opened, V->router.st.maps_evicted, V->router.st.maps_failed, V->router.st.maps_full, V->router.st.maps_peak, (u32)VBBA_MAPS, V->router.st.evict_port, V->router.st.evict_idle_ms);'
if s.count(d) == 1:
    s = s.replace(d, d + '\n\t{ const struct vbba_tcp_stats *q = &V->router.tcp.st; const struct vbba_tp_ios_tcp_stats *w = &V->tp.tst;\n'
      '\t  dbgprintf("SHUTDUMP: tcp syn %u opened %u open_fail %u noslot %u denied %u closed %u rst_tx %u rst_rx %u orphan %u retrans %u ooo %u bad %u\\r\\n", q->syn, q->opened, q->open_fail, q->noslot, q->denied, q->closed, q->rst_tx, q->rst_rx, q->orphan_rst, q->retrans, q->ooo, q->bad);\n'
      '\t  dbgprintf("SHUTDUMP: tcp bytes up %u down %u seg rx %u tx %u sock_err %u eof %u shut %u abort linger %u tries %u connect %u synack_rtx %u\\r\\n", q->bytes_up, q->bytes_down, q->seg_rx, q->seg_tx, q->sock_err, q->sock_eof, q->shutdowns, q->abort_linger, q->abort_tries, q->abort_connect, q->synack_rtx);\n'
      '\t  dbgprintf("SHUTDUMP: tcp ios opens %u connected %u fail %u cpoll %u rx %u B/%u tx %u B/%u short %u neg %u polls %u eof %u closes %u rc conn %d tx %d rev %d\\r\\n", w->opens, w->connected, w->open_fail, w->conn_poll, w->rx_bytes, w->rx_n, w->tx_bytes, w->tx_n, w->tx_short, w->tx_neg, w->rx_poll, w->eof, w->closes, w->last_conn_rc, w->last_tx_rc, w->last_poll_rev); }')
    print("TCP: SHUTDUMP lines added")
open(p, 'w').write(s)
PYTCP
    grep -q "case VBBA_IOS_TCPSOCKET" "$t/kernel/vbba_glue.c" && echo "TCP build: TCP splice through IOS stream sockets (core/vbba_tcp.c)" | tee -a "$log"
  fi
  if [[ "${NIN_MS_ARMLOG:-}" == 1 ]]; then   # E-ARM1 (needs NIN_MS_PASSHIST and NIN_MS_SHUTDUMP): WHY was a main-loop pass long?  Every pass >= 5 ms (the PASSHIST gap, which includes the previous sleep) is stored in a 32-entry ring with the time spent inside DIFinishAsync (the blocking disc-read wait, which does not service the vBBA), how many such waits, the ISO offset/length/mode of the read waited on, and the time spent in GCNCard_Save and how much the main-loop sleep overshot what was asked (other IOS threads ran); the rest of the pass is "other".  Printed at the soft power-off / freeze dump as SHUTDUMP: ARM lines.  Nothing is written during play (no SD access, no dbgprintf); per pass the cost is a few adds.
    [[ "${NIN_MS_PASSHIST:-}" == 1 && "${NIN_MS_SHUTDUMP:-}" == 1 ]] || { echo "NIN_MS_ARMLOG needs NIN_MS_PASSHIST and NIN_MS_SHUTDUMP"; exit 1; }
    python3 - "$t/kernel/DI.c" "$t/kernel/main.c" "$t/kernel/vbba_glue.c" "$t/kernel/common.c" <<'PYE4'
import sys
di, mn, gl, cm = sys.argv[1:5]
s = open(di).read()
# 1. the last issued disc read (the one a following DIFinishAsync waits for) and the time the blocking wait took
a = "static void DiscReadAsync(u32 Buffer, u32 Offset, u32 Length, u32 Mode)\n{\n\tDIFinishAsync(); //if something is still running\n"
assert s.count(a) == 1
s = s.replace(a, "extern u32 amLastOff, amLastLen, amLastMode;\n" + a + "\tamLastOff = Offset; amLastLen = Length; amLastMode = Mode;   /* ARMLOG: set after the wait, so a later DIFinishAsync reports the read it really waited on */\n")
a = "void DIFinishAsync()\n{\n\twhile(DiscCheckAsync() == false)\n\t{\n\t\tudelay(200); //wait for driver\n\t\tCheckOSReport();\n\t\tBTUpdateRegisters();\n\t}\n}"
assert s.count(a) == 1
b = ("extern void amNoteDi(u32 ticks);\nvoid DIFinishAsync()\n{\n\tu32 amT0 = 0;\n\twhile(DiscCheckAsync() == false)\n\t{\n\t\tif(amT0 == 0) amT0 = read32(HW_TIMER) | 1u;\n"
     "\t\tudelay(200); //wait for driver\n\t\tCheckOSReport();\n\t\tBTUpdateRegisters();\n\t}\n\tif(amT0) amNoteDi(read32(HW_TIMER) - (amT0 & ~1u));   /* ARMLOG: ticks spent in the blocking wait (tick resolution 0.527 us; the | 1 only marks 'started') */\n}")
s = s.replace(a, b)
open(di, 'w').write(s)
# 2. the card save and the main-loop sleep in the main loop
m = open(mn).read()
a = "\t\tudelay(VBBAMainDelay(20));"
assert m.count(a) == 1
m = m.replace(a, "\t\t{ extern void amNoteSleep(u32 ticks, u32 us); u32 amD = VBBAMainDelay(20), amT = read32(HW_TIMER); udelay(amD); amNoteSleep(read32(HW_TIMER) - amT, amD); }   /* ARMLOG: the sleep, charged to the next pass like PASSHIST does */")
a = "\t\t\t\tGCNCard_Save();\n"
assert m.count(a) == 1
m = m.replace(a, "\t\t\t\t{ extern void amNoteSave(u32 ticks); u32 amS0 = read32(HW_TIMER); GCNCard_Save(); amNoteSave(read32(HW_TIMER) - amS0); }   /* ARMLOG */\n")
open(mn, 'w').write(m)
# 3. the ring, the per-pass hook (inside the PASSHIST block of VBBAMainDelay), the printer
g = open(gl).read()
a = "\t  vgPsT = t; vgPsH[3]++;\n"
assert g.count(a) == 1
g = g.replace(a, "\t  if (vgPsT) { extern void amPass(u32 gapTicks, u32 now); amPass(t - vgPsT, t); }   /* ARMLOG: close the pass that just ended */\n" + a)
g += r"""

/* E-ARM1 (NIN_MS_ARMLOG): cause of every main-loop pass >= 5 ms.  Per pass: ticks inside DIFinishAsync (n waits, the read waited on), inside GCNCard_Save, and the main-loop sleep overshoot; the rest is "other". */
u32 amLastOff, amLastLen, amLastMode;
static struct { u32 diT, diN, offw, lenw, modew, svT, slT; } amCur;
static struct { u32 t, gap, diT, diN, off, len, mode, svT, slT; } amRing[32];
static u32 amN, amC[4], amMsDi, amMsSv, amMsSl, amMsOt;   /* amC: passes >= 5 ms, >= 20 ms, >= 100 ms, >= 500 ms */
void amNoteDi(u32 ticks) { amCur.diT += ticks; amCur.diN++; amCur.offw = amLastOff; amCur.lenw = amLastLen; amCur.modew = amLastMode; }
void amNoteSave(u32 ticks) { amCur.svT += ticks; }
void amNoteSleep(u32 ticks, u32 us) { u32 want = us * 1898u / 1000u; amCur.slT += (ticks > want) ? ticks - want : 0; }   /* overshoot only: time the sleep took beyond what was asked (other IOS threads ran) */
void amPass(u32 gap, u32 now)
{
	if (gap >= 9490u) {   /* 5 ms */
		u32 i = amN++ & 31u;
		amRing[i].t = now; amRing[i].gap = gap; amRing[i].diT = amCur.diT; amRing[i].diN = amCur.diN;
		amRing[i].off = amCur.offw; amRing[i].len = amCur.lenw; amRing[i].mode = amCur.modew; amRing[i].svT = amCur.svT; amRing[i].slT = amCur.slT;
		amC[0]++; if (gap >= 37960u) amC[1]++; if (gap >= 189800u) amC[2]++; if (gap >= 949000u) amC[3]++;
		if (gap >= 37960u) { u32 d = amCur.diT / 1898u, v = amCur.svT / 1898u, l = amCur.slT / 1898u, g = gap / 1898u; amMsDi += d; amMsSv += v; amMsSl += l; amMsOt += (g > d + v + l) ? g - d - v - l : 0; }
	}
	amCur.diT = amCur.diN = amCur.offw = amCur.lenw = amCur.modew = amCur.svT = amCur.slT = 0;
}
void vgArmDump(void)
{
	u32 i, n = amN > 32u ? 32u : amN, now = read32(HW_TIMER);
	dbgprintf("SHUTDUMP: ARM passes >= 5 ms %u, >= 20 ms %u, >= 100 ms %u, >= 500 ms %u | ms in passes >= 20 ms: disc-read wait %u, card save %u, sleep overshoot %u, other %u | ring %u of %u (newest first, age = ms before this dump)\r\n", amC[0], amC[1], amC[2], amC[3], amMsDi, amMsSv, amMsSl, amMsOt, n, amN);
	for (i = 0; i < n; i++) {
		u32 k = (amN - 1u - i) & 31u;
		dbgprintf("SHUTDUMP: ARM pass age %u ms gap %u ms | disc wait %u ms (%u waits) read off %08X len %08X mode %u | card save %u ms | sleep overshoot %u ms\r\n", (now - amRing[k].t) / 1898u, amRing[k].gap / 1898u, amRing[k].diT / 1898u, amRing[k].diN, amRing[k].off, amRing[k].len, amRing[k].mode, amRing[k].svT / 1898u, amRing[k].slT / 1898u);
	}
}
"""
open(gl, 'w').write(g)
# 4. print with the other SHUTDUMP lines (FREEZEDUMP, applied after this block, copies the call into its dump too)
c = open(cm).read()
a = "\n\t{ extern void vgShutdumpNet(void); vgShutdumpNet(); }"
assert c.count(a) == 1
c = c.replace(a, a + "\n\t{ extern void vgArmDump(void); vgArmDump(); }")
open(cm, 'w').write(c)
PYE4
    grep -q "vgArmDump" "$t/kernel/common.c" && echo "ARMLOG test build: pass-cause ring (disc-read wait / card save / other), printed in the SHUTDUMP lines" | tee -a "$log"
  fi
  if [[ "${NIN_MS_DIPUMP:-}" == 1 ]]; then   # FIX CANDIDATE for E-ARM1 (UNTESTED on a console, lab build only): DIFinishAsync, the blocking wait for the previous SD disc read, did not serve EXI or the vBBA, so the guest's BBA commands and the network waited out the whole read.  The spin now runs the same per-pass service the main loop runs with a read in flight (VBBAUpdate, EXIServiceStep under its EXI lock, SOCKUpdateRegisters), guarded against re-entry, and only while the vBBA/BBA emulation is on.  No SD or FatFs access is added (those paths are gated on DI_IRQ, which is true during the wait, exactly as in a normal main-loop pass).  Compare the ARM pass counters (NIN_MS_ARMLOG, NIN_MS_PASSHIST) with and without it.
    python3 - "$t/kernel/DI.c" <<'PYE5'
import sys
p = sys.argv[1]
s = open(p).read()
a = "\t\tCheckOSReport();\n\t\tBTUpdateRegisters();\n\t}\n"
assert s.count(a) == 1, s.count(a)
b = ("\t\tCheckOSReport();\n\t\tBTUpdateRegisters();\n"
     "\t\t{ extern vu32 bbaEmuWanted, vbbaWanted; extern void VBBAUpdate(void); extern int EXIServiceStep(void); extern void SOCKUpdateRegisters(void); static u8 dipIn;   /* DIPUMP */\n"
     "\t\t  if (bbaEmuWanted && !dipIn) { dipIn = 1; if (vbbaWanted) VBBAUpdate(); (void)EXIServiceStep(); SOCKUpdateRegisters(); dipIn = 0; } }\n"
     "\t}\n")
s = s.replace(a, b)
open(p, 'w').write(s)
PYE5
    grep -q "DIPUMP" "$t/kernel/DI.c" && echo "DIPUMP test build (fix candidate): DIFinishAsync serves EXI / vBBA / sockets while it waits" | tee -a "$log"
  fi
  if [[ "${NIN_MS_FREEZEDUMP:-}" == 1 ]]; then   # test build (finding 63): the dump goes to /shutdump.log (its own file: ndebug.log dies early in some runs) and the kernel writes it by itself when the game's retrace counter has not moved for 6 s (again at 36 s and 66 s)
    python3 - "$t/kernel/vbba_glue.c" "$t/kernel/common.c" "$t/kernel/vsprintf.c" <<'PYE3'
import sys
g, c, v = sys.argv[1], sys.argv[2], sys.argv[3]
t = open(c).read()
i = t.index("\n\t{ u32 si;")
j = t.index("} }\n", t.index("for (si = 0; si < 4; si++)", i)) + 4
body = t[i:j]
t = t[:i] + "\n\t{ extern void vgDumpAll(void); vgDumpAll(); }\n" + t[j:]
open(c, 'w').write(t)
s = open(g).read()
s = s.replace('dbgprintf("SHUTDUMP', 'sdumpf("SHUTDUMP')
body = body.replace('dbgprintf("SHUTDUMP', 'sdumpf("SHUTDUMP')
s += """

extern int sdumpf(const char *fmt, ...);
void vgDumpAll(void)""" + body.rstrip("\n") + """

static u32 vgFzCnt, vgFzT, vgFzN, vgFzNext;
void vgFreezeWatch(void)                        /* once a second: the game's retrace counter (0x804D7420) has not moved for 6 s -> dump (again at 36 s and 66 s) */
{
	u32 t = read32(HW_TIMER), c;
	if (vgFzNext && (s32)(t - vgFzNext) < 0) return;
	vgFzNext = t + 1898000u;
	c = read32(0x004D7420);
	if (!c) return;
	if (c != vgFzCnt) { vgFzCnt = c; vgFzT = t; vgFzN = 0; return; }
	if (vgFzN < 3u && (u32)(t - vgFzT) > (6u + 30u * vgFzN) * 1898000u) { vgFzN++; sdumpf("SHUTDUMP: auto dump %u: the game's retrace counter has not moved for %u s (%u)\\r\\n", vgFzN, (t - vgFzT) / 1898000u, c); vgDumpAll(); }
}
"""
s = s.replace("void vgShutdumpNet(void)", "extern int sdumpf(const char *fmt, ...);\nvoid vgShutdumpNet(void)", 1) if "extern int sdumpf" not in s.split("void vgShutdumpNet(void)")[0] else s
open(g, 'w').write(s)
w = open(v).read()
w += """

/* test build: one formatted line appended to /shutdump.log, the file opened and closed per call (ndebug.log can be dead) */
int sdumpf(const char *fmt, ...)
{
	va_list args;
	char buffer[0x100];
	FIL f;
	UINT w;
	va_start(args, fmt);
	_vsprintf(buffer, fmt, args);
	va_end(args);
	if (!SDisInit) return -1;
	if (f_open_char(&f, "/shutdump.log", FA_OPEN_ALWAYS | FA_WRITE) != FR_OK) return -1;
	f_lseek(&f, f.obj.objsize);
	f_write(&f, buffer, strlen(buffer), &w);
	f_close(&f);
	return 0;
}
"""
open(v, 'w').write(w)
PYE3
    grep -q "vgFreezeWatch" "$t/kernel/vbba_glue.c" && echo "FREEZEDUMP test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_RAMLOG:-}" == 1 ]]; then   # test build (finding 64): from "Jumping to" on the log lines go to a RAM buffer and not to the SD card (the game reads its ISO through the same card driver: a write plus sync per line races with it); /ramlog.log gets the buffer at a freeze dump and at LOGEND
    python3 - "$t/kernel/vsprintf.c" "$t/kernel/Patch.c" "$t/kernel/common.c" "$t/kernel/vbba_glue.c" <<'PYE4'
import sys
v, pa, c, g = sys.argv[1:5]
s = open(v).read()
a = "\tu32 read;\t\n\tu32 msSaved = msCrumb.stage;   /* */"
assert s.count(a) == 1, "vsprintf anchor"
s = s.replace(a, "\tif (dbgRamOn) {   /* RAMLOG test build: after the game started the lines stay in RAM */\n\t\tu32 n = strlen(buffer);\n\t\tif (dbgRamN + n < sizeof dbgRamBuf) { memcpy(dbgRamBuf + dbgRamN, buffer, n); dbgRamN += n; } else dbgRamLost++;\n\t\treturn 0;\n\t}\n" + a)
b = "int dbgprintf( const char *fmt, ...)\n{"
assert s.count(b) == 1
s = s.replace(b, "u32 dbgRamOn, dbgRamN, dbgRamLost;\nchar dbgRamBuf[16384];\n" + b)
s += """

/* RAMLOG: write the RAM log to /ramlog.log (replaced each time) */
void dbgRamFlush(void)
{
	FIL f;
	UINT w;
	if (!SDisInit || !dbgRamN) return;
	if (f_open_char(&f, "/ramlog.log", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return;
	f_write(&f, dbgRamBuf, dbgRamN, &w);
	f_close(&f);
}
"""
open(v, 'w').write(s)
t = open(pa).read()
a = '\tdbgprintf("Jumping to 0x%08X\\n", GameEntry);'
assert t.count(a) == 1, "patch anchor"
t = t.replace(a, a + "\n\t{ extern u32 dbgRamOn; dbgRamOn = 1; }")
open(pa, 'w').write(t)
u = open(c).read()
a = '\tdbgprintf("LOGEND:'
assert u.count(a) == 1, "common anchor"
u = u.replace(a, "\t{ extern void dbgRamFlush(void); dbgRamFlush(); }\n" + a)
open(c, 'w').write(u)
w = open(g).read()
a = "void vgDumpAll(void){" if "void vgDumpAll(void){" in w else "void vgDumpAll(void)"
assert a in w
w = w.replace("void vgDumpAll(void)", "extern void dbgRamFlush(void);\nvoid vgDumpAll(void)", 1)
w = w.replace("vgFzN++; sdumpf(", "vgFzN++; dbgRamFlush(); sdumpf(", 1)
w = w.replace("void vgDumpAll(void)", """void vgDumpVi(void)
{
	sdumpf("SHUTDUMP: game VI state retraceCount %u flushFlag %08X changeMode %08X shdwChangeMode %08X changed %08X %08X shdwChanged %08X %08X FBSet %08X CurrTiming %08X CurrTvMode %08X | scene %08X loading %08X\\r\\n", read32(0x004D7420), read32(0x004D7424), read32(0x004D7450), read32(0x004D7440), read32(0x004D7448), read32(0x004D744C), read32(0x004D7458), read32(0x004D745C), read32(0x004D7468), read32(0x004D7460), read32(0x004D7464), read32(0x00479D30), read32(0x00479D64));
}
void vgDumpAll(void)""", 1)
w = w.replace("\t{ u32 si;", "\t{ u32 si; { extern void vgDumpVi(void); vgDumpVi(); }", 1)
open(g, 'w').write(w)
PYE4
    grep -q "dbgRamOn" "$t/kernel/vsprintf.c" && echo "RAMLOG test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_FZ2:-}" == 1 ]]; then   # test build (static analysis): (1) the freeze dumps read the shared ring WITHOUT sync_before_read, so dumps 2 and 3 (and perhaps 1) may show a stale ARM cache line: now invalidated first; (2) every dispatch of the guest OS interrupt dispatcher (not only those in the range check) counts into 0xD30267A0 and stores the VI beam/DI0..3/CR words, the time base, the DSP CSR word, the AI control word and the context pointer (PowerPC reads of these registers are safe, the dispatcher reads DSP/AI itself); the dump prints them with the delta of the dispatch count since the previous dump (interrupts still being taken during the freeze or not) and the EXI / IPC words
    [[ "${NIN_MS_RAMLOG:-}" == 1 && "${NIN_MS_VISAMPLE:-}" == 1 && "${NIN_MS_FREEZEDUMP:-}" == 1 ]] || { echo "NIN_MS_FZ2 needs RAMLOG, VISAMPLE and FREEZEDUMP"; exit 1; }
    python3 - "$t/kernel/asm/FakeInterrupt.S" "$t/kernel/vbba_glue.c" <<'PYF'
import sys
fi, g = sys.argv[1], sys.argv[2]
s = open(fi).read()
a = "SampleDone:\n"
assert s.count(a) == 1, "FakeInterrupt SampleDone"
blk = """SampleDone:
	lis		r8,	0xD302
	lwz		r9,	0x67A0(r8)
	addi		r9,	r9,	1
	stw		r9,	0x67A0(r8)		#every dispatch, whatever the context address
	li		r9,	1
	stw		r9,	0x67CC(r8)		#stage 1: entered (stages: 1 entry, 2 VI words read, 3 about to read the DSP CSR, 4 DSP CSR read, 5 AI read, 6 left FakeInterrupt)
	lis		r10,	0xCC00
	lwz		r9,	0x202C(r10)		#VI beam position words
	stw		r9,	0x67A4(r8)
	lwz		r9,	0x2030(r10)		#DI0..DI3
	stw		r9,	0x67A8(r8)
	lwz		r9,	0x2034(r10)
	stw		r9,	0x67AC(r8)
	lwz		r9,	0x2038(r10)
	stw		r9,	0x67B0(r8)
	lwz		r9,	0x203C(r10)
	stw		r9,	0x67B4(r8)
	lwz		r9,	0x2000(r10)		#VTR / DCR (enable bit)
	stw		r9,	0x67B8(r8)
	li		r9,	2
	stw		r9,	0x67CC(r8)
	mftbl		r9
	stw		r9,	0x67BC(r8)
	li		r9,	3
	stw		r9,	0x67CC(r8)
	lwz		r9,	0x5008(r10)		#DSP CSR is the low half
	stw		r9,	0x67C0(r8)
	li		r9,	4
	stw		r9,	0x67CC(r8)
	lwz		r9,	0x6C00(r10)		#AI control
	stw		r9,	0x67C4(r8)
	stw		r30,	0x67C8(r8)		#context pointer of this dispatch
	li		r9,	5
	stw		r9,	0x67CC(r8)
"""
s = s.replace(a, blk)
e = "end:\n\tlis\t\tr3,\t0xCC00\n"
assert s.count(e) == 1, "FakeInterrupt end"
s = s.replace(e, "end:\n\tlis\t\tr8,\t0xD302\n\tli\t\tr9,\t6\n\tstw\t\tr9,\t0x67CC(r8)\n\tlis\t\tr3,\t0xCC00\n")
open(fi, 'w').write(s)
t = open(g).read()
b = "void vgDumpAll(void)\n"
assert t.count(b) == 1, "vgDumpAll"
fn = r"""static u32 vgFz2Prev;
void vgDumpX(void)
{
	u32 a;
	sync_before_read((void *)0x13026500, 0x100);
	sync_before_read((void *)0x13026700, 0x100);
	a = read32(0x130267A0);
	sdumpf("FZ2: dispatches %u (+%u since the last dump) in-range counter %u | stage %u (1 entry, 2 VI read, 3 before DSP CSR read, 4 after, 5 after AI read, 6 left FakeInterrupt)\r\n", a, a - vgFz2Prev, read32(0x13026700), read32(0x130267CC));
	vgFz2Prev = a;
	sdumpf("FZ2: VI beam %08X DI0 %08X DI1 %08X DI2 %08X DI3 %08X VTR/DCR %08X | tbl %08X | DSP %08X AI %08X | ctx %08X\r\n", read32(0x130267A4), read32(0x130267A8), read32(0x130267AC), read32(0x130267B0), read32(0x130267B4), read32(0x130267B8), read32(0x130267BC), read32(0x130267C0), read32(0x130267C4), read32(0x130267C8));
	sdumpf("FZ2: ppc irq flag %08X mask %08X | exi marker %08X held %08X exi_int %08X count %08X\r\n", read32(HW_PPCIRQFLAG), read32(HW_PPCIRQMASK), read32(0x13026694), read32(0x13026690), read32(0x13026560), read32(0x13026698));
}
"""
t = t.replace(b, fn + b)
c = "\t{ u32 si; { extern void vgDumpVi(void); vgDumpVi(); }"
assert t.count(c) == 1, "vgDumpVi call"
t = t.replace(c, c + " { extern void vgDumpX(void); vgDumpX(); }")
open(g, 'w').write(t)
PYF
    grep -q "FZ2:" "$t/kernel/vbba_glue.c" && echo "FZ2 test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_FZ3:-}" == 1 ]]; then   # test build (q18): q17 showed the PowerPC takes NO interrupt after the freeze (dispatch count +0 at 36 s and 66 s), so it spins with MSR.EE = 0 or waits for something: the freeze dump also prints the engine's status block (its breadcrumb BC value 'spin': 0x1x EXI op started, 0x2xx issued and waiting in EXISync, 0x1xx done; ip; hook calls with the delta since the previous dump) and the EXI mailbox words / lock / served counts, all reads only
    [[ "${NIN_MS_FZ2:-}" == 1 ]] || { echo "NIN_MS_FZ3 needs NIN_MS_FZ2"; exit 1; }
    python3 - "$t/kernel/vbba_glue.c" <<'PYG'
import sys
g = sys.argv[1]
t = open(g).read()
a = 'read32(0x13026694), read32(0x13026690), read32(0x13026560), read32(0x13026698));\n'
assert t.count(a) == 1, "FZ2 anchor"
b = a + r"""	{
		static u32 prevCalls;
		extern u32 exiServed[4][3];
		static u32 fz3S3, fz3Scans;
		u32 s3 = fz3S3, c0, c1, got, ch, k;
		if (!s3 && fz3Scans < 3u) {   /* the quiet profile has no vg_dg_find_s3: same scan, engine status block magic 0x53335354 in 0x80400000..0x80600000 */
			u32 a, i;
			fz3Scans++;
			for (a = 0x80400000u; a < 0x80600000u && !s3; a += 0x20u) {
				const volatile u32 *p = (const volatile u32 *)(a & 0x01FFFFFFu);
				sync_before_read((void *)p, 0x20);
				for (i = 0; i < 8u; i++) {
					if (p[i] == 0x53335354u) {
						const volatile u32 *q = (const volatile u32 *)((a + 4u * i) & 0x01FFFFFFu);
						sync_before_read((void *)((u32)q & ~31u), 0x120);
						if (q[1] < 0x100u && q[39] < 0x1000u && q[3] != 0xFFFFFFFFu) { s3 = a + 4u * i; break; }
					}
				}
			}
			fz3S3 = s3;
		}
		if (s3) {
			const volatile u32 *s = (const volatile u32 *)(s3 & 0x01FFFFFFu);
			sync_before_read((void *)((u32)s & ~31u), 0x120);
			sdumpf("FZ3: s3 @%08X state %X calls %u (+%u) b0 %u ip %08X spin(BC) %X\r\n", s3, s[1], s[3], s[3] - prevCalls, s[5], s[4], s[39]);
			prevCalls = s[3];
		} else sdumpf("FZ3: s3 block not found\r\n");
		sync_before_read((void *)EXI_BASE, 0x20);
		c0 = read32(EXI_CMD_0); c1 = read32(EXI_CMD_1);
		got = (u32)EXILockTry(); if (got) EXIUnlock();
		sdumpf("FZ3: exi posted cmd0 %08X cmd1 %08X lock %s\r\n", c0, c1, got ? "free" : "HELD");
		for (ch = 0; ch < 3u; ch++) { k = ch; sdumpf("FZ3: exi served ch%u card %u bba %u other %u\r\n", ch, exiServed[k][0], exiServed[k][1], exiServed[k][2]); }
	}
"""
t = t.replace(a, b)
open(g, 'w').write(t)
PYG
    grep -q "FZ3:" "$t/kernel/vbba_glue.c" && echo "FZ3 test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_FZ4:-}" == 1 ]]; then   # test build (q19): prints the engine's uncached breadcrumb words 0xD30267D0.. (engine build with bc_wii, 1147 and later): BC value, PowerPC time base when it was written, caller of the last irq_off, irq_off count, 'inside an irq_off window' flag; the time base of the last dispatch (FZ2) gives the age of the freeze in PowerPC ticks
    [[ "${NIN_MS_FZ3:-}" == 1 ]] || { echo "NIN_MS_FZ4 needs NIN_MS_FZ3"; exit 1; }
    python3 - "$t/kernel/vbba_glue.c" <<'PYH'
import sys
g = sys.argv[1]
t = open(g).read()
a = '\t\t} else sdumpf("FZ3: s3 block not found\\r\\n");\n'
assert t.count(a) == 1, "FZ3 anchor"
t = t.replace(a, a + '\t\tsdumpf("FZ4: BC %08X at tbl %08X | irq_off lr %08X count %u inside %u (engine words 0xD30267D0..E0, all zero = engine without bc_wii)\\r\\n", read32(0x130267D0), read32(0x130267D4), read32(0x130267D8), read32(0x130267DC), read32(0x130267E0));\n')
open(g, 'w').write(t)
PYH
    grep -q "FZ4:" "$t/kernel/vbba_glue.c" && echo "FZ4 test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_FZ11:-}" == 1 ]]; then   # test build (DESYNC LOCATOR,): the dump also prints the engine's desync locator (status words 48/49/51 of s3_status plus the checksum-compare words).  The engine (a Wii-marked build, s12_gc.c bc_wii_on) publishes the address of its status block in the uncached word 0xD30267D8 (the never-written 'irq_off lr'); the FZ3 scan does not find the block in current builds.  Reads only, the same path as FZ3: pointer range and magic checked, cache invalidate, volatile reads of MEM1
    [[ "${NIN_MS_FZ4:-}" == 1 ]] || { echo "NIN_MS_FZ11 needs NIN_MS_FZ4"; exit 1; }
    python3 - "$t/kernel/vbba_glue.c" <<'PYL'
import sys
g = sys.argv[1]
t = open(g).read()
i = t.index('\t\tsdumpf("FZ4: BC')
j = t.index("\n", i) + 1
blk = r"""	{	/* FZ11: the desync locator */
		u32 p = read32(0x130267D8);
		if (p >= 0x80000000u && p < 0x81800000u && !(p & 3u)) {
			const volatile u32 *d = (const volatile u32 *)(p & 0x01FFFFFFu);
			sync_before_read((void *)((u32)d & ~31u), 0x120);
			if (d[0] == 0x53335354u) {
				sdumpf("FZ11: s3 @%08X local frame %u last compared %u cmp %u bad %u first_bad %u | rlatest %u lnewest %u peer tag %u\r\n", p, d[6], d[37], d[21], d[22], d[23], d[18], d[19], d[36]);
				sdumpf("FZ11: first mismatch frame %u ours %08X peer %08X (hash halves %04X / %04X, float sums %04X / %04X)\r\n", d[48], d[49], d[51], d[49] >> 16, d[51] >> 16, d[49] & 0xFFFFu, d[51] & 0xFFFFu);
			} else sdumpf("FZ11: pointer %08X is not the engine status block (magic %08X)\r\n", p, d[0]);
		} else sdumpf("FZ11: no status pointer (%08X): engine without the desync locator\r\n", p);
	}
"""
t = t[:j] + blk + t[j:]
open(g, 'w').write(t)
PYL
    grep -q "FZ11:" "$t/kernel/vbba_glue.c" && echo "FZ11 test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_FZ5:-}" == 1 ]]; then   # test build (q21): q20 showed the hang is after FakeInterrupt, inside Melee's own __OSDispatchInterrupt (stage 6, __OSLastInterrupt not updated): seven marker stubs are patched into that function (Melee 1.02 addresses, each replaced instruction is checked first): site 80347908 (before the DSP CSR read), 8034790C (after), 80347AB0 (OS mask load), 80347AC8 (priority table), 80347B00 (handler table lookup), 80347B38 (about to call the handler), 80347B54 (handler returned); each stub stores its own return address (site + 4) in the uncached word 0xD30267E8 and runs the replaced instruction; FakeInterrupt clears the word at every entry; the dump prints it
    [[ "${NIN_MS_FZ4:-}" == 1 ]] || { echo "NIN_MS_FZ5 needs NIN_MS_FZ4"; exit 1; }
    python3 - "$t/kernel/Patch.c" "$t/kernel/asm/FakeInterrupt.S" "$t/kernel/vbba_glue.c" <<'PYI'
import sys
pa, fi, g = sys.argv[1:4]
t = open(pa).read()
a = "\t\t\t\t\tPatchBL( PatchCopy(FakeInterrupt, FakeInterrupt_size), (u32)Buffer + i + 4 );\n"
assert t.count(a) == 1, "Patch.c FakeInterrupt anchor"
blk = r"""	{	/* Multishine FZ5: marker stubs inside Melee's __OSDispatchInterrupt (function start = this match - 0x1C) */
		static const u32 dm[5] = { 0x7CA802A6, 0x3CC0D302, 0x90A667E8, 0x60000000, 0x4E800020 };   /* mflr r5; lis r6,0xD302; stw r5,0x67E8(r6); <replaced instruction>; blr */
		static const u32 offs[7] = { 0xCC, 0xD0, 0x274, 0x28C, 0x2C4, 0x2FC, 0x318 };
		static const u32 want[7] = { 0xA083000A, 0x54830738, 0x808300C4, 0x3C608040, 0x806DBCD8, 0x7FA3EB78, 0x7FC3F378 };
		u32 fb = (u32)Buffer + i - 0x1C, k;
		for (k = 0; k < 7; k++) {
			if (read32(fb + offs[k]) != want[k]) { dbgprintf("FZ5: site +%03X differs (%08X), no marker\r\n", offs[k], read32(fb + offs[k])); continue; }
			u32 st = PatchCopy((const u8 *)dm, sizeof dm);
			*(volatile u32 *)(st + 12) = want[k];
			PatchBL(st, fb + offs[k]);
		}
	}
"""
t = t.replace(a, a + blk)
open(pa, 'w').write(t)
s = open(fi).read()
b = "\tstw\t\tr9,\t0x67CC(r8)\t\t#stage 1: entered"
assert s.count(b) == 1
i = s.index(b); j = s.index("\n", i) + 1
s = s[:j] + "\tli\t\tr9,\t0\n\tstw\t\tr9,\t0x67E8(r8)\t\t#marker word cleared at every entry\n" + s[j:]
open(fi, 'w').write(s)
t = open(g).read()
c = 'read32(0x13026700), read32(0x130267CC));'
assert t.count(c) == 1
t = t.replace(c, 'read32(0x13026700), read32(0x130267CC));\n\tsdumpf("FZ5: dispatcher marker (site+4) %08X\\r\\n", read32(0x130267E8));')
open(g, 'w').write(t)
PYI
    grep -q "FZ5:" "$t/kernel/vbba_glue.c" && echo "FZ5 test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_FZ6:-}" == 1 ]]; then   # test build (q22): q21 froze with the last marker 80347B3C = the PowerPC passed the record of the interrupt and OSDisableScheduler and is inside the interrupt HANDLER (or OSEnableScheduler / __OSReschedule) with EE = 0.  Melee's __DSPHandler (0x8033625C, the DSP_DSP handler, CSR bit 0x80 = what the dumps show) busy-waits 'while (DSPCheckMailFromDSP == 0);' and later 'while (DSPCheckMailToDSP != 0);' inside the interrupt: a DSP that never sends / takes its mail hangs the PowerPC for good.  This build records the interrupt number and handler address at the handler call, two markers in __DSPHandler (803362A4 = in the wait-for-mail loop, 803362B0 = mail arrived) and the two DSP mailbox status halfwords (read as halfwords only: a 32-bit read would consume the mail) at every dispatch entry
    [[ "${NIN_MS_FZ5:-}" == 1 ]] || { echo "NIN_MS_FZ6 needs NIN_MS_FZ5"; exit 1; }
    python3 - "$t/kernel/Patch.c" "$t/kernel/asm/FakeInterrupt.S" "$t/kernel/vbba_glue.c" <<'PYJ'
import sys
pa, fi, g = sys.argv[1:4]
t = open(pa).read()
# stub 2 for the handler-call site: also stores r29 (interrupt) and r31 (handler)
a = "\t\t\tu32 st = PatchCopy((const u8 *)dm, sizeof dm);\n\t\t\t*(volatile u32 *)(st + 12) = want[k];\n\t\t\tPatchBL(st, fb + offs[k]);\n"
assert t.count(a) == 1, "FZ5 loop anchor"
b = r"""			if (offs[k] == 0x2FC) {
				static const u32 dm2[7] = { 0x7CA802A6, 0x3CC0D302, 0x90A667E8, 0x93A667EC, 0x93E667F0, 0x60000000, 0x4E800020 };   /* ... stw r29,0x67EC(r6); stw r31,0x67F0(r6); <replaced>; blr */
				u32 st2 = PatchCopy((const u8 *)dm2, sizeof dm2);
				*(volatile u32 *)(st2 + 20) = want[k];
				PatchBL(st2, fb + offs[k]);
				continue;
			}
"""
t = t.replace(a, b + a)
# the __DSPHandler markers: after the FZ5 loop block (the block ends with "\t\t}\n\t}\n")
c = "\t\t\tPatchBL(st, fb + offs[k]);\n\t\t}\n"
assert t.count(c) == 1, "FZ5 loop end"
d = r"""		{	/* FZ6: two markers in Melee's __DSPHandler (0x8033625C = dispatcher 0x8034783C - 0x115E0) */
			static const u32 doffs[2] = { 0x44, 0x50 };
			static const u32 dwant[2] = { 0x28030000, 0x80ADBB8C };
			u32 hb = fb - 0x115E0u, q;
			for (q = 0; q < 2; q++) {
				if (read32(hb + doffs[q]) != dwant[q]) { dbgprintf("FZ6: __DSPHandler +%02X differs (%08X), no marker\r\n", doffs[q], read32(hb + doffs[q])); continue; }
				u32 st3 = PatchCopy((const u8 *)dm, sizeof dm);
				*(volatile u32 *)(st3 + 12) = dwant[q];
				PatchBL(st3, hb + doffs[q]);
			}
		}
"""
t = t.replace(c, c + d)
open(pa, 'w').write(t)
s = open(fi).read()
e = "\tlwz\t\tr9,\t0x5008(r10)\t\t#DSP CSR is the low half\n\tstw\t\tr9,\t0x67C0(r8)\n"
assert s.count(e) == 1
s = s.replace(e, e + "\tlhz\t\tr9,\t0x5004(r10)\t\t#DSP mailbox DSP->CPU, high half only (reading the low half would take the mail)\n\tstw\t\tr9,\t0x67F4(r8)\n\tlhz\t\tr9,\t0x5000(r10)\t\t#CPU->DSP high half\n\tstw\t\tr9,\t0x67F8(r8)\n")
open(fi, 'w').write(s)
u = open(g).read()
f = 'sdumpf("FZ5: dispatcher marker (site+4) %08X\\r\\n", read32(0x130267E8));'
assert u.count(f) == 1
u = u.replace(f, f + '\n\tsdumpf("FZ6: interrupt %u handler %08X | mailbox status DSP->CPU %04X CPU->DSP %04X at the last entry\\r\\n", read32(0x130267EC), read32(0x130267F0), read32(0x130267F4), read32(0x130267F8));')
open(g, 'w').write(u)
PYJ
    grep -q "FZ6:" "$t/kernel/vbba_glue.c" && echo "FZ6 test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_DSPFIX:-}" == 1 ]]; then   # test build (q23, REPAIR experiment): the DSP mail waits of Melee's __DSPHandler / __DSP_exec_task are unbounded busy loops INSIDE the interrupt ('while (DSPCheckMailFromDSP == 0);' and 'while (DSPCheckMailToDSP != 0);'); q22 showed the PowerPC stuck in the handler after the first wait.  DSPCheckMailToDSP (0x80335FC8) and DSPCheckMailFromDSP (0x80335FD8) get a branch to stubs that give up after 500000 consecutive busy polls (ToDSP returns 0, FromDSP returns 1) and count them in uncached words 0xD30266C0 (ToDSP: +0 run, +4 give-ups, +8 longest run) and 0xD30266D0 (FromDSP); the freeze / shutdown dump prints them (needs NIN_MS_FZ6)
    [[ "${NIN_MS_FZ6:-}" == 1 ]] || { echo "NIN_MS_DSPFIX needs NIN_MS_FZ6"; exit 1; }
    cat > "$B/dspfix.S" <<'PYASM'
	.text
	.globl stubTo
stubTo:                           # replaces DSPCheckMailToDSP (jumped to from its first instruction): returns 1 while the CPU->DSP mailbox is full, gives up (returns 0) after 500000 consecutive busy polls
	mflr	6
	lis	3,0xCC00
	lhz	5,0x5000(3)
	extrwi	3,5,1,16
	lis	4,0xD302
	stw	6,0x66FC(4)
	cmpwi	3,0
	beq	1f
	lwz	5,0x66C0(4)
	addi	5,5,1
	stw	5,0x66C0(4)
	lwz	12,0x66C8(4)               # longest run seen
	cmplw	5,12
	ble	2f
	stw	5,0x66C8(4)
2:	lis	12,7
	ori	12,12,0xA120
	cmplw	5,12
	ble	3f
	lwz	5,0x66C4(4)                # give-ups
	addi	5,5,1
	stw	5,0x66C4(4)
	li	5,0
	stw	5,0x66C0(4)
	li	3,0
3:	blr
1:	li	5,0
	stw	5,0x66C0(4)
	blr
	.globl stubFrom
stubFrom:                         # replaces DSPCheckMailFromDSP: returns 0 while no mail, gives up (returns 1) after 500000 consecutive empty polls
	lis	3,0xCC00
	lhz	5,0x5004(3)
	extrwi	3,5,1,16
	lis	4,0xD302
	cmpwi	3,0
	bne	1f
	lwz	5,0x66D0(4)
	addi	5,5,1
	stw	5,0x66D0(4)
	lwz	12,0x66D8(4)
	cmplw	5,12
	ble	2f
	stw	5,0x66D8(4)
2:	lis	12,7
	ori	12,12,0xA120
	cmplw	5,12
	ble	3f
	lwz	5,0x66D4(4)
	addi	5,5,1
	stw	5,0x66D4(4)
	li	5,0
	stw	5,0x66D0(4)
	li	3,1
3:	blr
1:	li	5,0
	stw	5,0x66D0(4)
	blr
PYASM
    "$DEVKITPPC/bin/powerpc-eabi-as" -mgekko -o "$B/dspfix.o" "$B/dspfix.S" && "$DEVKITPPC/bin/powerpc-eabi-objcopy" -O binary -j .text "$B/dspfix.o" "$B/dspfix.bin" || { echo "dspfix stub assembly failed"; exit 1; }
    python3 - "$t/kernel/Patch.c" "$t/kernel/main.c" "$t/kernel/vbba_glue.c" "$B/dspfix.bin" <<'PYK'
import sys, struct
pa, mc, g, binp = sys.argv[1:5]
w = open(binp, 'rb').read()
assert len(w) % 4 == 0 and len(w) == 0xE0, len(w)
arr = ", ".join("0x%08X" % x for x in struct.unpack(">%dI" % (len(w) // 4), w))
t = open(pa).read()
a = "\t\t\t\t\tPatchBL( PatchCopy(FakeInterrupt, FakeInterrupt_size), (u32)Buffer + i + 4 );\n"
assert t.count(a) == 1, "Patch.c anchor"
blk = """	{	/* Multishine DSPFIX: bounded DSP mail waits (stubTo at +0, stubFrom at +0x74) */
		static const u32 dspfix[%d] = { %s };
		u32 fb = (u32)Buffer + i - 0x1C, sTo = fb - 0x11874u, sFrom = fb - 0x11864u;
		if (read32(sTo) == 0x3C60CC00 && read32(sTo + 4) == 0xA0035000 && read32(sFrom) == 0x3C60CC00 && read32(sFrom + 4) == 0xA0035004) {
			u32 st = PatchCopy((const u8 *)dspfix, sizeof dspfix);
			PatchB(st, sTo);
			PatchB(st + 0x74, sFrom);
			dbgprintf("DSPFIX: DSPCheckMailToDSP / FromDSP replaced (stubs at %%08X)\\r\\n", st);
		} else dbgprintf("DSPFIX: sites differ (%%08X %%08X), not patched\\r\\n", read32(sTo), read32(sFrom));
	}
""" % (len(w) // 4, arr)
t = t.replace(a, a + blk)
open(pa, 'w').write(t)
m = open(mc).read()
b = "\tsync_after_write((void*)0x13026700, 0xA0);\n"
assert m.count(b) == 1, "main.c anchor"
m = m.replace(b, b + "\tmemset32((void*)0x130266C0, 0, 0x40);   /* DSPFIX counters */\n\tsync_after_write((void*)0x130266C0, 0x40);\n")
open(mc, 'w').write(m)
u = open(g).read()
f = 'read32(0x130267F4), read32(0x130267F8));'
assert u.count(f) == 1
u = u.replace(f, f + '\n\tsync_before_read((void *)0x130266C0, 0x40);\n\tsdumpf("DSPFIX: ToDSP run %u giveups %u longest %u | FromDSP run %u giveups %u longest %u\\r\\n", read32(0x130266C0), read32(0x130266C4), read32(0x130266C8), read32(0x130266D0), read32(0x130266D4), read32(0x130266D8));')
open(g, 'w').write(u)
PYK
    grep -q "DSPFIX:" "$t/kernel/vbba_glue.c" && echo "DSPFIX test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_NODSP:-}" == 1 ]]; then   # test build (q24): Nintendont rewrites Melee's DSP side for its disc-audio streaming: ndebug.log shows 'Patch:[__DSPHandler B] applied (0x0033625C)' (a streaming mixer called from inside the DSP interrupt handler) and 'Patch:[DSP v2] patched' (the AX DSP microcode is modified).  Melee does not use AI streaming, and the Cube (no Nintendont) never sees either patch, while the freeze is inside __DSPHandler on the Wii only.  This switch skips both (the DSP microcode patch and the handler hook)
    python3 - "$t/kernel/Patch.c" <<'PYN'
import sys
p = sys.argv[1]
t = open(p).read()
a = "static void DoDSPPatch( char *ptr, u32 Version )\n{\n"
assert t.count(a) == 1, "DoDSPPatch"
t = t.replace(a, a + "\tdbgprintf(\"NODSP: DSP microcode patch v%u skipped\\r\\n\", Version);\n\treturn;\n")
b = "\tu32 DSPHandlerNeeded = 1;\n"
assert t.count(b) == 1, "DSPHandlerNeeded"
t = t.replace(b, "\tu32 DSPHandlerNeeded = 0;   /* NODSP */\n")
open(p, 'w').write(t)
PYN
    grep -q "NODSP" "$t/kernel/Patch.c" && echo "NODSP test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_FZ7:-}" == 1 ]]; then   # test build (q25): q23/q24 showed the PowerPC leaves the first mail wait of Melee's __DSPHandler (marker 803362B0) and the ToDSP / FromDSP polls are not where it hangs (longest run 1).  What follows is the mail switch with the task callbacks (init_cb / res_cb / done_cb of the AX DSP task, 9 call sites) and the common tail (OSClearContext, OSSetCurrentContext).  This build stores the mail value (0xD30267FC), the callback address at each callback call (0xD30267E4, marker = the call site + 4: 80336324 80336348 803363A0 80336434 803364D4 80336548 80336590 803365EC 80336658) and markers at the tail (80336660 / 80336670)
    [[ "${NIN_MS_DSPFIX:-}" == 1 ]] || { echo "NIN_MS_FZ7 needs NIN_MS_DSPFIX"; exit 1; }
    cat > "$B/fz7.S" <<'PYASM'
	.text
	.globl stubMail
stubMail:                         # at 0x803362B0 of __DSPHandler (after DSPReadMailFromDSP, r3 = the mail): marker + mail
	mflr	7
	lis	6,0xD302
	stw	7,0x67E8(6)
	stw	3,0x67FC(6)
	lwz	0,8(5)                    # the replaced instruction
	blr
	.globl stubCb
stubCb:                           # at each 'mtlr r12' before a task callback blrl: marker + callback address, then mtlr r12 and return
	mflr	5
	lis	6,0xD302
	stw	5,0x67E8(6)
	stw	12,0x67E4(6)
	mtlr	12
	mtctr	5
	bctr
PYASM
    "$DEVKITPPC/bin/powerpc-eabi-as" -mgekko -o "$B/fz7.o" "$B/fz7.S" && "$DEVKITPPC/bin/powerpc-eabi-objcopy" -O binary -j .text "$B/fz7.o" "$B/fz7.bin" || { echo "fz7 stub assembly failed"; exit 1; }
    python3 - "$t/kernel/Patch.c" "$t/kernel/asm/FakeInterrupt.S" "$t/kernel/vbba_glue.c" "$B/fz7.bin" <<'PYK'
import sys, struct
pa, fi, g, binp = sys.argv[1:5]
w = open(binp, 'rb').read()
assert len(w) == 52, len(w)
arr = ", ".join("0x%08X" % x for x in struct.unpack(">13I", w))
t = open(pa).read()
a = "\t\t\t\t\tPatchBL( PatchCopy(FakeInterrupt, FakeInterrupt_size), (u32)Buffer + i + 4 );\n"
assert t.count(a) == 1, "Patch.c anchor"
blk = """	{	/* Multishine FZ7: mail capture + callback markers + tail markers in Melee's __DSPHandler (0x8033625C = dispatcher 0x8034783C - 0x115E0) */
		static const u32 fz7[13] = { %s };   /* stubMail at +0, stubCb at +0x18 */
		static const u32 dm[5] = { 0x7CA802A6, 0x3CC0D302, 0x90A667E8, 0x60000000, 0x4E800020 };
		static const u32 cbs[9] = { 0xC4, 0xE8, 0x140, 0x1D4, 0x274, 0x2E8, 0x330, 0x38C, 0x3F4 };
		static const u32 tails[2] = { 0x400, 0x410 };
		static const u32 twant[2] = { 0x38610010, 0x800102EC };
		u32 hb = (u32)Buffer + i - 0x1C - 0x115E0u, q;
		if (read32(hb + 0x54) == 0x80050008) {
			u32 st = PatchCopy((const u8 *)fz7, sizeof fz7);
			PatchBL(st, hb + 0x54);
			for (q = 0; q < 9; q++) {
				if (read32(hb + cbs[q]) != 0x7D8803A6) { dbgprintf("FZ7: callback site +%%03X differs (%%08X)\\r\\n", cbs[q], read32(hb + cbs[q])); continue; }
				PatchBL(st + 0x18, hb + cbs[q]);
			}
			for (q = 0; q < 2; q++) {
				u32 s2;
				if (read32(hb + tails[q]) != twant[q]) { dbgprintf("FZ7: tail site +%%03X differs (%%08X)\\r\\n", tails[q], read32(hb + tails[q])); continue; }
				s2 = PatchCopy((const u8 *)dm, sizeof dm);
				*(volatile u32 *)(s2 + 12) = twant[q];
				PatchBL(s2, hb + tails[q]);
			}
		} else dbgprintf("FZ7: __DSPHandler differs (%%08X), no markers\\r\\n", read32(hb + 0x54));
	}
""" % arr
t = t.replace(a, a + blk)
open(pa, 'w').write(t)
s = open(fi).read()
b = "\tstw\t\tr9,\t0x67E8(r8)\t\t#marker word cleared at every entry\n"
assert s.count(b) == 1, "FakeInterrupt clear anchor"
s = s.replace(b, b + "\tstw\t\tr9,\t0x67E4(r8)\n\tstw\t\tr9,\t0x67FC(r8)\n")
open(fi, 'w').write(s)
u = open(g).read()
f = 'read32(0x130266D8));'
assert u.count(f) == 1
u = u.replace(f, f + '\n\tsdumpf("FZ7: mail %08X callback %08X\\r\\n", read32(0x130267FC), read32(0x130267E4));')
open(g, 'w').write(u)
PYK
    grep -q "FZ7:" "$t/kernel/vbba_glue.c" && echo "FZ7 test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_STRMGUARD:-}" == 1 ]]; then   # FIX candidate (q26, found by static analysis of kernel/asm/__DSPHandler.S, the disc-audio streaming mixer Nintendont hooks into Melee's __DSPHandler at +0xF8 = the 0xDCD10002 mail path, i.e. every DSP frame while a DVD audio stream plays): its three cache loops are 'srwi r7,<bytes>,5; mtctr r7; <dcbi/dcbf>; bdnz'.  A count of 0 makes bdnz loop 2^32 times with r5 walking through the whole address space (dcbi/dcbf on unmapped addresses = exception) inside the interrupt with EE = 0.  r7 = 0 happens (1) when the AI DMA length register reads 0 (the DMA just ended, the next one is not started yet: r8 = 0, loops 1 and 3), and (2) at the buffer wrap when fewer than 32 bytes of the DMA chunk remain (loop 'DCInvalidateRange3', r7 = (r8 - r5) >> 5 = 0).  Guards: skip the whole mix when r8 < 32 (count in 0xD30266E0) and skip loop 3 when its count is 0 (count in 0xD30266E4)
    python3 - "$t/kernel/asm/__DSPHandler.S" <<'PYS'
import sys
p = sys.argv[1]
s = open(p).read()
a = "\tlwz\t\tr4,\tAI_ADP_LOC@l(r12)\n"
assert s.count(a) == 1, "AI_ADP_LOC load"
s = s.replace(a, a + "\tcmplwi\tr8,\t0x20\n\tbge\t\tdmaok\n\tlis\t\tr12,\t0xD302\n\tlwz\t\tr0,\t0x66E0(r12)\n\taddi\tr0,\tr0,\t1\n\tstw\t\tr0,\t0x66E0(r12)\n\tb\t\texit\ndmaok:\n")
b = "\tsub\t\tr7,\tr8,\tr5\n\tsrwi\tr7,\tr7,\t5\n\tmtctr\tr7\n"
assert s.count(b) == 1, "loop 3"
s = s.replace(b, "\tsub\t\tr7,\tr8,\tr5\n\tsrwi\tr7,\tr7,\t5\n\tcmplwi\tr7,\t0\n\tbne\t\tinv3ok\n\tlis\t\tr12,\t0xD302\n\tlwz\t\tr0,\t0x66E4(r12)\n\taddi\tr0,\tr0,\t1\n\tstw\t\tr0,\t0x66E4(r12)\n\tb\t\tresetbuffer\ninv3ok:\n\tmtctr\tr7\n")
open(p, 'w').write(s)
PYS
    grep -q "dmaok" "$t/kernel/asm/__DSPHandler.S" && echo "STRMGUARD build" | tee -a "$log"
    python3 - "$t/kernel/vbba_glue.c" <<'PYT'
import sys
g = sys.argv[1]
u = open(g).read()
f = 'read32(0x130266D8));'
if u.count(f) == 1:
    u = u.replace(f, f + '\n\tsdumpf("STRMGUARD: mix skipped (DMA length 0) %u times, loop 3 skipped (count 0) %u times\\r\\n", read32(0x130266E0), read32(0x130266E4));')
    open(g, 'w').write(u)
PYT
  fi
  if [[ "${NIN_MS_FZ8:-}" == 1 ]]; then   # test build (q27): the resume callback of the AX DSP task (mail 0xDCD10001, res_cb = __AXDSPResumeCallback) runs the WHOLE AX frame (__AXOutNewFrame: __AXSyncPBs, __AXServiceCallbackStack = every voice callback, __AXProcessAux, __AXUserFrameCallback = Melee's audio engine, AIInitDMA) inside the DSP interrupt with EE = 0.  Markers (value = site + 4, the registry of the last one reached) in __AXOutNewFrame (80359748 entered, 80359760 after SyncPBs/PrintStudio/GetCommandList, 80359798 after ServiceCallbackStack, 803597A8 after ProcessAux, 803597D4 after the user frame callback, 80359820 after AIInitDMA) and in __AXServiceCallbackStack (80358B1C after a voice callback, 80358B48 loop left), and the callback address of the user frame callback (80359 7C8) and of each voice callback (80358B0C) in 0xD30267E4
    [[ "${NIN_MS_FZ7:-}" == 1 ]] || { echo "NIN_MS_FZ8 needs NIN_MS_FZ7"; exit 1; }
    python3 - "$t/kernel/Patch.c" "$B/fz7.bin" <<'PYK'
import sys, struct
pa, binp = sys.argv[1:3]
w = open(binp, 'rb').read()
arr = ", ".join("0x%08X" % x for x in struct.unpack(">13I", w))
t = open(pa).read()
a = "\t\t\t\t\tPatchBL( PatchCopy(FakeInterrupt, FakeInterrupt_size), (u32)Buffer + i + 4 );\n"
assert t.count(a) == 1, "Patch.c anchor"
blk = """	{	/* Multishine FZ8: markers in the AX frame code that runs inside the DSP interrupt */
		static const u32 fz7[13] = { %s };   /* stubCb at +0x18 */
		static const u32 dm[5] = { 0x7CA802A6, 0x3CC0D302, 0x90A667E8, 0x60000000, 0x4E800020 };
		static const u32 ga[8] = { 0x80359748, 0x80359760, 0x80359798, 0x803597A8, 0x803597D4, 0x80359820, 0x80358B1C, 0x80358B48 };
		static const u32 gw[8] = { 0x909F0784, 0x3C80BABE, 0x909F078C, 0x909F0794, 0x909F07A4, 0x909F07AC, 0x3BC30000, 0x80010014 };
		static const u32 cb[2] = { 0x803597C8, 0x80358B0C };
		u32 fb = (u32)Buffer + i - 0x1C, q, st;
		st = PatchCopy((const u8 *)fz7, sizeof fz7);
		for (q = 0; q < 8; q++) {
			u32 s2, site = fb + (ga[q] - 0x8034783Cu);
			if (read32(site) != gw[q]) { dbgprintf("FZ8: site %%08X differs (%%08X)\\r\\n", ga[q], read32(site)); continue; }
			s2 = PatchCopy((const u8 *)dm, sizeof dm);
			*(volatile u32 *)(s2 + 12) = gw[q];
			PatchBL(s2, site);
		}
		for (q = 0; q < 2; q++) {
			u32 site = fb + (cb[q] - 0x8034783Cu);
			if (read32(site) != 0x7D8803A6) { dbgprintf("FZ8: callback site %%08X differs (%%08X)\\r\\n", cb[q], read32(site)); continue; }
			PatchBL(st + 0x18, site);
		}
	}
""" % arr
t = t.replace(a, a + blk)
open(pa, 'w').write(t)
PYK
    grep -q "FZ8: Multishine\|Multishine FZ8" "$t/kernel/Patch.c" && echo "FZ8 test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_FZ9:-}" == 1 ]]; then   # test build (q28): q26 froze with the last marker 803362B4 = the DCD10002 mail ("task yielded", every DSP frame) was captured and the hang is AFTER it: Nintendont's streaming hook (patched at 0x80336354, __DSPHandler.S) or the rest of that mail case (__DSP_exec_task with its mail sends, task switch).  The mail polls never spin (longest 1) and the mixer guard never fired.  This build adds (a) stage words in the hook (0xD30266E8: 1 entry, 7 left at once because no stream / AI idle, 2 mix started, 3 invalidate loop done, 4 copy loop done, 5 flush loop done, 6 exit; 0xD30266EC = r8 DMA bytes, 0xD30266F0 = r4 stream position at the start) and (b) markers in the DCD10002 case: 80336350 before the hook, 803363F0 pending == 0, 803363FC first-task test, 80336420 / 8033643C / 80336454 / 80336460 / 80336478 / 80336490 / 8033649C the branches and the returns of __DSP_exec_task (value = site + 4)
    [[ "${NIN_MS_STRMGUARD:-}" == 1 ]] || { echo "NIN_MS_FZ9 needs NIN_MS_STRMGUARD"; exit 1; }
    python3 - "$t/kernel/Patch.c" "$t/kernel/asm/__DSPHandler.S" "$t/kernel/vbba_glue.c" "$t/kernel/asm/FakeInterrupt.S" <<'PYK'
import sys
pa, sa, g, fi = sys.argv[1:5]
t = open(pa).read()
a = "\t\t\t\t\tPatchBL( PatchCopy(FakeInterrupt, FakeInterrupt_size), (u32)Buffer + i + 4 );\n"
assert t.count(a) == 1, "Patch.c anchor"
blk = """	{	/* Multishine FZ9: markers in the DCD10002 case of __DSPHandler (stub uses r10/r11: r5 is live there) */
		static const u32 dm[5] = { 0x7D6802A6, 0x3D40D302, 0x916A67E8, 0x60000000, 0x4E800020 };   /* mflr r11; lis r10,0xD302; stw r11,0x67E8(r10); <replaced>; blr */
		static const u32 off[10] = { 0xF4, 0x194, 0x1A0, 0x1C4, 0x1E0, 0x1F8, 0x204, 0x21C, 0x234, 0x240 };
		static const u32 want[10] = { 0x800DBB78, 0x80050038, 0x800DBB88, 0x806DBB8C, 0x3C60CDD1, 0x806DBB8C, 0x806DBB8C, 0x3C60CDD1, 0x806DBB8C, 0x806DBB8C };
		u32 hb = (u32)Buffer + i - 0x1C - 0x115E0u, q;
		for (q = 0; q < 10; q++) {
			u32 s2;
			if (read32(hb + off[q]) != want[q]) { dbgprintf("FZ9: __DSPHandler +%%03X differs (%%08X)\\r\\n", off[q], read32(hb + off[q])); continue; }
			s2 = PatchCopy((const u8 *)dm, sizeof dm);
			*(volatile u32 *)(s2 + 12) = want[q];
			PatchBL(s2, hb + off[q]);
		}
	}
"""
t = t.replace(a, a + blk)
open(pa, 'w').write(t)
s = open(sa).read()
def stage(n, regs=("r12", "r3")):
    return "\tlis\t\tr12,\t0xD302\n\tli\t\tr3,\t%d\n\tstw\t\tr3,\t0x66E8(r12)\n" % n
a1 = "__DSPHandler:\n"
assert s.count(a1) == 1
s = s.replace(a1, a1 + stage(1))
a8 = "\tlwz\t\tr3,\tSTREAMING@l(r12)\n"
assert s.count(a8) == 1, "STREAMING load"
s = s.replace(a8, a8 + "\tlis\t\tr12,\t0xD302\n\tstw\t\tr12,\t0x66F8(r12)\n")   # 0x66F8 = 0xD3020000: the STREAMING read completed
fi_s = open(fi).read()
cl = "\tstw\t\tr9,\t0x67FC(r8)\n"
if fi_s.count(cl) == 1:
    fi_s = fi_s.replace(cl, cl + "\tstw\t\tr9,\t0x66E8(r8)\n\tstw\t\tr9,\t0x66F8(r8)\n")
    open(fi, 'w').write(fi_s)
a2 = "\tbne\t\tstartcp\n\tcmpwi\tr0,\t0\n\tblr\n"
assert s.count(a2) == 1
s = s.replace(a2, "\tbne\t\tstartcp\n" + stage(7) + "\tcmpwi\tr0,\t0\n\tblr\n")
a3 = "dmaok:\n"
assert s.count(a3) == 1
s = s.replace(a3, a3 + "\tlis\t\tr12,\t0xD302\n\tli\t\tr0,\t2\n\tstw\t\tr0,\t0x66E8(r12)\n\tstw\t\tr8,\t0x66EC(r12)\n\tstw\t\tr4,\t0x66F0(r12)\n")
a4 = "\tbdnz\tDCInvalidateRange1\n"
assert s.count(a4) == 1
s = s.replace(a4, a4 + "\tlis\t\tr12,\t0xD302\n\tli\t\tr0,\t3\n\tstw\t\tr0,\t0x66E8(r12)\n")
a5 = "\tblt\t\tstartcopy\n"
assert s.count(a5) == 1
s = s.replace(a5, a5 + "\tlis\t\tr12,\t0xD302\n\tli\t\tr0,\t4\n\tstw\t\tr0,\t0x66E8(r12)\n")
a6 = "\tbdnz\tDCFlushRange\n"
assert s.count(a6) == 1
s = s.replace(a6, a6 + "\tlis\t\tr12,\t0xD302\n\tli\t\tr0,\t5\n\tstw\t\tr0,\t0x66E8(r12)\n")
a7 = "exit:\n"
assert s.count(a7) == 1
s = s.replace(a7, a7 + "\tlis\t\tr12,\t0xD302\n\tli\t\tr0,\t6\n\tstw\t\tr0,\t0x66E8(r12)\n")
open(sa, 'w').write(s)
u = open(g).read()
f = 'read32(0x130266E0), read32(0x130266E4));'
assert u.count(f) == 1, "STRMGUARD print"
u = u.replace(f, f + '\n\tsdumpf("FZ9: hook stage %u (0 hook not entered in the last dispatch, 1 entry, 7 no stream, 2 mix, 3-5 loops done, 6 exit) STREAMING-read %08X r8 %08X r4 %08X\\r\\n", read32(0x130266E8), read32(0x130266F8), read32(0x130266EC), read32(0x130266F0));')
open(g, 'w').write(u)
PYK
    grep -q "FZ9:" "$t/kernel/vbba_glue.c" && echo "FZ9 test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_PATCHSPACE:-}" == 1 ]]; then   # test build (q29): the Nintendont patch stubs (PatchCopy) grow DOWN from 0x2F90 in low memory; with cheats off nothing stops them going below 0x1800, where the game's own OSInit writes its exception vectors (0x100..0x17FF), and the Multishine stubs of the last builds (FZ5..FZ9, DSPFIX) add about 1 KB.  A stub that is overwritten there executes garbage once a rare path runs.  This build logs the final stub pointer ('PATCHSPACE: POffset %08X') into ndebug.log and a shared word (0xD30266F4), and the freeze dump prints it
    python3 - "$t/kernel/Patch.c" "$t/kernel/vbba_glue.c" <<'PYP'
import sys
pa, g = sys.argv[1:3]
t = open(pa).read()
a = "\tif(cheatsWanted || debuggerWanted)\n\t{\n\t\t//setup jump to codehandler stub\n"
assert t.count(a) == 1, "cheats anchor"
t = t.replace(a, "\tdbgprintf(\"PATCHSPACE: POffset %08X (patch stubs occupy %u bytes below 0x3000)\\r\\n\", POffset, 0x3000u - POffset);\n\twrite32(0x130266F4, POffset);\n\tsync_after_write((void*)0x130266C0, 0x40);\n\t{ extern void vgPatchSnap(void); vgPatchSnap(); }\n" + a)
open(pa, 'w').write(t)
u = open(g).read()
u = u.replace("void vgDumpX(void)\n{", "void vgPatchDiff(void);\nvoid vgDumpX(void)\n{", 1)
u += """

static u32 vgPatchSnapBuf[0x400], vgPatchLo, vgPatchHi;   /* PATCHSPACE: copy of the patch stub area [POffset, 0x2FF4) right after patching */
void vgPatchSnap(void)
{
	u32 a, n = 0;
	vgPatchLo = read32(0x130266F4) & ~3u; vgPatchHi = 0x2FF4;
	if (vgPatchLo < 0x1800 || vgPatchLo >= vgPatchHi || (vgPatchHi - vgPatchLo) > sizeof vgPatchSnapBuf) { vgPatchLo = vgPatchHi = 0; return; }
	sync_before_read((void *)(vgPatchLo & ~31u), (vgPatchHi - (vgPatchLo & ~31u) + 31u) & ~31u);
	for (a = vgPatchLo; a < vgPatchHi; a += 4) vgPatchSnapBuf[n++] = read32(a);
}
void vgPatchDiff(void)
{
	u32 a, n = 0, d = 0;
	if (!vgPatchHi) { sdumpf("PATCHDIFF: no snapshot (area below 0x1800 or too large)\\r\\n"); return; }
	sync_before_read((void *)(vgPatchLo & ~31u), (vgPatchHi - (vgPatchLo & ~31u) + 31u) & ~31u);
	for (a = vgPatchLo; a < vgPatchHi; a += 4, n++) {
		u32 now = read32(a);
		if (now != vgPatchSnapBuf[n]) { if (d < 12u) sdumpf("PATCHDIFF: %08X boot %08X now %08X\\r\\n", a, vgPatchSnapBuf[n], now); d++; }
	}
	sdumpf("PATCHDIFF: %u of %u stub words differ from the boot image (area %08X..%08X)\\r\\n", d, n, vgPatchLo, vgPatchHi);
}
"""
f = 'read32(0x130266E8), read32(0x130266EC), read32(0x130266F0));'
if u.count(f) == 1:
    u = u.replace(f, f + '\n\tsdumpf("PATCHSPACE: POffset %08X (the stubs overlap the exception vectors only if below 0x1800)\\r\\n", read32(0x130266F4)); vgPatchDiff();')
else:
    f = 'read32(0x130266E0), read32(0x130266E4));'
    assert u.count(f) == 1
    u = u.replace(f, f + '\n\tsdumpf("PATCHSPACE: POffset %08X (the stubs overlap the exception vectors only if below 0x1800)\\r\\n", read32(0x130266F4)); vgPatchDiff();')
open(g, 'w').write(u)
PYP
    grep -q "PATCHSPACE" "$t/kernel/Patch.c" && echo "PATCHSPACE test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_FZ10:-}" == 1 ]]; then   # test build (q31): q30 showed the streaming hook RETURNED in the last dispatch (stage 7) and no later marker was written, so the PowerPC is between the hook return and the first marker after it.  Possible: the rude-task path (DSPAssertTask: the AX frame sync, __DSP_rude_task_pending != 0) which has no marker on its way, or a fault.  This build adds markers (each stub stores its marker BEFORE the replaced instruction) on that path: 8033635C (-> 80336360), 80336368 (curr == rude, -> 8033636C), 80336380 / 80336384 (after the mail wait, -> 80336384 / 80336388), 80336390 (before the done callback, -> 80336394), 803363A8 (curr != rude, -> 803363AC), 803363C0 (-> 803363C4), 803363CC (after __DSP_exec_task, -> 803363D0); the caller of every DSPCheckMailToDSP call (0xD30266FC, 'FZ10: last ToDSP caller'); and the OS exception entry __OSUnhandledException (0x80345884: exception number, srr0, dsisr, dar, count into 0xD3026704..14) so a fault inside the interrupt shows up
    [[ "${NIN_MS_FZ9:-}" == 1 ]] || { echo "NIN_MS_FZ10 needs NIN_MS_FZ9"; exit 1; }
    python3 - "$t/kernel/Patch.c" "$t/kernel/vbba_glue.c" <<'PYK'
import sys
pa, g = sys.argv[1:3]
t = open(pa).read()
a = "\t\t\t\t\tPatchBL( PatchCopy(FakeInterrupt, FakeInterrupt_size), (u32)Buffer + i + 4 );\n"
assert t.count(a) == 1, "Patch.c anchor"
blk = """	{	/* Multishine FZ10: markers on the rude-task path of the DCD10002 case, and the OS exception entry */
		static const u32 dm[5] = { 0x7D6802A6, 0x3D40D302, 0x916A67E8, 0x60000000, 0x4E800020 };   /* mflr r11; lis r10,0xD302; stw r11,0x67E8(r10); <replaced>; blr */
		static const u32 off[8] = { 0x100, 0x10C, 0x124, 0x128, 0x134, 0x14C, 0x164, 0x170 };
		static const u32 want[8] = { 0x800DBB7C, 0x3C60CDD1, 0x38000000, 0x806DBB8C, 0x8183002C, 0x3C60CDD1, 0x806DBB8C, 0x806DBB8C };
		static const u32 ex[10] = { 0x3D40D302, 0x906A6704, 0x81640198, 0x916A6708, 0x90AA670C, 0x90CA6710, 0x81660000, 0x60000000, 0x4E800020, 0x60000000 };
		u32 fb = (u32)Buffer + i - 0x1C, hb = fb - 0x115E0u, q, s2;
		for (q = 0; q < 8; q++) {
			if (read32(hb + off[q]) != want[q]) { dbgprintf("FZ10: __DSPHandler +%03X differs (%08X)\\r\\n", off[q], read32(hb + off[q])); continue; }
			s2 = PatchCopy((const u8 *)dm, sizeof dm);
			*(volatile u32 *)(s2 + 12) = want[q];
			PatchBL(s2, hb + off[q]);
		}
		/* exception entry: lis r10,0xD302; stw r3,0x6704(r10); lwz r11,0x198(r4); stw r11,0x6708(r10); stw r5,0x670C(r10); stw r6,0x6710(r10); lwz r11,0x6714(r10); addi r11,r11,1; stw r11,0x6714(r10); <replaced addi r25,r3,0>; blr */
		{	static const u32 exs[12] = { 0x3D40D302, 0x906A6704, 0x81640198, 0x916A6708, 0x90AA670C, 0x90CA6710, 0x816A6714, 0x396B0001, 0x916A6714, 0x60000000, 0x4E800020, 0x60000000 };
			u32 site = fb - 0x1FB8u;
			(void)ex;
			if (read32(site) != 0x3B230000) dbgprintf("FZ10: __OSUnhandledException differs (%08X), no exception marker\\r\\n", read32(site));
			else { s2 = PatchCopy((const u8 *)exs, sizeof exs); *(volatile u32 *)(s2 + 36) = 0x3B230000; PatchBL(s2, site); }
		}
	}
"""
t = t.replace(a, a + blk)
open(pa, 'w').write(t)
u = open(g).read()
f = 'read32(0x130266F8), read32(0x130266EC), read32(0x130266F0));'
assert u.count(f) == 1, "FZ9 print anchor"
u = u.replace(f, f + '\n\tsdumpf("FZ10: last DSPCheckMailToDSP caller %08X | OS exceptions %u (last: number %u srr0 %08X dsisr %08X dar %08X)\\r\\n", read32(0x130266FC), read32(0x13026714), read32(0x13026704), read32(0x13026708), read32(0x1302670C), read32(0x13026710));')
open(g, 'w').write(u)
PYK
    grep -q "FZ10:" "$t/kernel/vbba_glue.c" && echo "FZ10 test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_GAMELOG:-}" == 1 ]]; then   # the game log reaches the SD by itself at game exit.  Needs RAMLOG + FREEZEDUMP + ARMLOG.  Nothing is written during play: dbgprintf (RAMLOG branch) also copies each line into an 8 KB RAM ring (a memcpy, no SD).  On the two exit paths that already exist (Shutdown: power button / reset 0x7DEA; main loop 'Game Exit' 0x1DEA, which flushed nothing before), AFTER DIFinishAsync (disc reads stopped), /multishine/game.log is rotated to game.old and written: header (tick, retraceCount, PASSHIST), the SHUTDUMP lines of the existing dump (ARM pass-cause ring, router, samples), then the newest 8 KB of the log.  Hard cap 60 KB, every failure silent, once per run.
    [[ "${NIN_MS_RAMLOG:-}" == 1 && "${NIN_MS_FREEZEDUMP:-}" == 1 && "${NIN_MS_ARMLOG:-}" == 1 ]] || { echo "NIN_MS_GAMELOG needs RAMLOG, FREEZEDUMP and ARMLOG"; exit 1; }
    python3 - "$t/kernel/vsprintf.c" "$t/kernel/vbba_glue.c" "$t/kernel/common.c" "$t/kernel/main.c" <<'PYGL'
import sys
v, g, c, m = sys.argv[1:5]
s = open(v).read()
# 1. the 8 KB ring, filled from the RAMLOG branch of dbgprintf (RAM only)
a = "char dbgRamBuf[16384];\n"
assert s.count(a) == 1, "GAMELOG ring anchor"
s = s.replace(a, a + "char glRing[8192];   /* GAMELOG: newest 8 KB of the game-phase log */\nu32 glRingN;\nstatic void glRingPut(const char *p, u32 n) { u32 i, k = glRingN; for (i = 0; i < n; i++) glRing[(k + i) & 8191u] = p[i]; glRingN = k + n; }\n")
a = "\t\tu32 n = strlen(buffer);\n\t\tif (dbgRamN + n < sizeof dbgRamBuf)"
assert s.count(a) == 1, "GAMELOG hook anchor"
s = s.replace(a, "\t\tu32 n = strlen(buffer);\n\t\tglRingPut(buffer, n);   /* GAMELOG */\n\t\tif (dbgRamN + n < sizeof dbgRamBuf)")
# 2. sdumpf (the existing dump's line writer) also copies into game.log while it is open
a = "\tif (!SDisInit) return -1;\n\tif (f_open_char(&f, \"/shutdump.log\""
assert s.count(a) == 1, "GAMELOG sdumpf anchor"
s = s.replace(a, "\t{ extern void glTee(const char *p, u32 n); glTee(buffer, strlen(buffer)); }   /* GAMELOG */\n" + a)
s += r'''

/* GAMELOG: /multishine/game.log; the previous one is kept as game.old */
#define GL_CAP 61440u       /* whole file */
#define GL_DUMPCAP 40960u   /* the dump lines (tee) stop here, so the 8 KB ring always fits */
static FIL glF;
static u32 glOpen, glBytes, glDone;
static void glWrite(const char *p, u32 n, u32 cap)
{
	UINT w;
	if (!glOpen || glBytes + n > cap) return;
	if (f_write(&glF, p, n, &w) != FR_OK) { glOpen = 0; return; }
	glBytes += n;
}
void glTee(const char *p, u32 n) { glWrite(p, n, GL_DUMPCAP); }
void glf(const char *fmt, ...)
{
	va_list args;
	char buffer[0x100];
	va_start(args, fmt);
	_vsprintf(buffer, fmt, args);
	va_end(args);
	glWrite(buffer, strlen(buffer), GL_CAP);
}
extern u32 dbgRamOn;
static u8 glCopyBuf[512] ALIGNED(32);
void glBegin(void)
{
	FIL fo, fn;
	UINT r, w;
	if (glDone || !dbgRamOn || !SDisInit) return;
	glDone = 1;
	/* the kernel's FatFs is built with _FS_MINIMIZE 2 (no f_rename / f_unlink): the previous game.log is COPIED to game.old (at most 60 KB, a failed step is silent) */
	if (f_open_char(&fo, "/multishine/game.log", FA_READ | FA_OPEN_EXISTING) == FR_OK) {
		if (f_open_char(&fn, "/multishine/game.old", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
			u32 tot = 0;
			while (tot < GL_CAP && f_read(&fo, glCopyBuf, sizeof glCopyBuf, &r) == FR_OK && r) { if (f_write(&fn, glCopyBuf, r, &w) != FR_OK) break; tot += r; }
			f_close(&fn);
		}
		f_close(&fo);
	}
	if (f_open_char(&glF, "/multishine/game.log", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return;
	glOpen = 1; glBytes = 0;
}
void glEnd(void)
{
	u32 n, start, i;
	char tmp[64];
	if (!glOpen) return;
	n = glRingN < 8192u ? glRingN : 8192u;
	start = glRingN - n;
	glf("GAMELOG: log ring (newest %u of %u bytes of game-phase log, oldest first)\r\n", n, glRingN);
	for (i = 0; i < n; i += sizeof tmp) { u32 j, l = n - i < sizeof tmp ? n - i : sizeof tmp; for (j = 0; j < l; j++) tmp[j] = glRing[(start + i + j) & 8191u]; glWrite(tmp, l, GL_CAP); }
	glf("GAMELOG: end (%u bytes)\r\n", glBytes);
	f_close(&glF);
	glOpen = 0;
}
'''
open(v, 'w').write(s)
w = open(g).read()
w += r'''

/* GAMELOG: header of /multishine/game.log, then the existing dump (vgDumpAll writes through sdumpf, which copies into the file) */
extern void glBegin(void); extern void glEnd(void); extern void glf(const char *fmt, ...);
extern u32 dbgRamN, dbgRamLost;
void vgGameLogBegin(void)
{
	glBegin();
	glf("GAMELOG v1: tick %u hw70 %08X | game retraceCount %u | scene %08X | RAM log %u bytes (lost %u)\r\n", read32(HW_TIMER), read32(0x0D800070), read32(0x004D7420), read32(0x00479D30), dbgRamN, dbgRamLost);
	glf("GAMELOG: PASSHIST ARM main-loop passes >= 5 ms %u, >= 20 ms %u, longest %u ms, passes %u\r\n", vgPsH[0], vgPsH[1], vgPsH[2], vgPsH[3]);
}
void vgGameLogEnd(void) { glEnd(); }
'''
open(g, 'w').write(w)
t = open(c).read()
a = "\n\t{ extern void vgDumpAll(void); vgDumpAll(); }\n"
assert t.count(a) == 1, "GAMELOG common anchor 1"
t = t.replace(a, "\n\t{ extern void vgGameLogBegin(void); vgGameLogBegin(); }   /* GAMELOG */\n\t{ extern void vgDumpAll(void); vgDumpAll(); }\n")
a = "\t{ extern void dbgRamFlush(void); dbgRamFlush(); }\n"
assert t.count(a) == 1, "GAMELOG common anchor 2"
t = t.replace(a, a + "\t{ extern void vgGameLogEnd(void); vgGameLogEnd(); }   /* GAMELOG */\n")
open(c, 'w').write(t)
mm = open(m).read()
a = "\tHIDClose();\n\tIOS_Close(DI_Handle); //close game\n"
assert mm.count(a) == 1, "GAMELOG main anchor"
mm = mm.replace(a, "\t{ extern void vgGameLogBegin(void), vgGameLogEnd(void), vgDumpAll(void); vgGameLogBegin(); vgDumpAll(); vgGameLogEnd(); }   /* GAMELOG: 'Game Exit' (0x1DEA), disc reads stopped by DIFinishAsync above */\n" + a)
open(m, 'w').write(mm)
PYGL
    grep -q "vgGameLogBegin" "$t/kernel/main.c" && echo "GAMELOG test build: /multishine/game.log written at game exit and at Shutdown" | tee -a "$log"
  fi
  if [[ "${NIN_MS_TCP:-}" == 1 && "${NIN_MS_GAMELOG:-}" == 1 ]]; then   # the TCP counters also go into game.log (our reads that file after a session)
    python3 - "$t/kernel/vbba_glue.c" <<'PYTCPGL'
import sys
p = sys.argv[1]; s = open(p).read()
a = '\tglf("GAMELOG: PASSHIST ARM main-loop passes'
assert s.count(a) == 1, "TCP gamelog anchor"
s = s.replace(a, '\tif (V) { const struct vbba_tcp_stats *q = &V->router.tcp.st; const struct vbba_tp_ios_tcp_stats *w = &V->tp.tst;   /* */\n'
  '\t  glf("GAMELOG: TCP guest syn %u opened %u fail %u denied %u noslot %u closed %u rst out %u in %u | bytes up %u down %u | retrans %u ooo %u bad %u | aborts linger %u tries %u idle %u\\r\\n", q->syn, q->opened, q->open_fail, q->denied, q->noslot, q->closed, q->rst_tx, q->rst_rx, q->bytes_up, q->bytes_down, q->retrans, q->ooo, q->bad, q->abort_linger, q->abort_tries, q->abort_idle);\n'
  '\t  glf("GAMELOG: TCP ios opens %u connected %u fail %u cpoll %u | rx %u B in %u, tx %u B in %u (short %u, refused %u) | polls %u eof %u closes %u | last rc connect %d send %d poll %d\\r\\n", w->opens, w->connected, w->open_fail, w->conn_poll, w->rx_bytes, w->rx_n, w->tx_bytes, w->tx_n, w->tx_short, w->tx_neg, w->rx_poll, w->eof, w->closes, w->last_conn_rc, w->last_tx_rc, w->last_poll_rev); }\n' + a)
open(p, 'w').write(s)
PYTCPGL
    grep -q "GAMELOG: TCP guest" "$t/kernel/vbba_glue.c" && echo "TCP: game.log lines added" | tee -a "$log"
  fi
  if [[ "${NIN_MS_NULLGUARD:-}" == 1 ]]; then   # FIX candidate (q32), from the q31 freeze dump: 'OS exceptions' grows while frozen, last one DSI at 0x8033671C, dar 0xC: the PowerPC is in an ENDLESS DSI LOOP inside Melee's __DSPHandler.  0x8033671C is 'lwz r3,0xC(r31)' in __DSP_exec_task = next->iram_mmem_addr with next (r4) == NULL; the caller is the rude-task branch of the DCD10002 mail (marker 803363C4): '__DSP_rude_task_pending != 0' but '__DSP_rude_task == NULL' (loaded at 0x803363C4 and passed as next).  The OS error handler returns to the same instruction, so the load faults forever (EE = 0, nothing else runs).  On a Cube the load from address 0xC reads low memory silently, so the same state is harmless there.  This switch replaces the load at 0x803363C4 by a stub: with a non-NULL rude task nothing changes; with NULL it counts, remembers curr / pending, clears __DSP_rude_task_pending and jumps to the common tail of __DSPHandler (0x8033665C), skipping __DSP_exec_task.  The DSP may be left waiting for the task-switch mails (audio may stop), but the PowerPC lives.
    [[ "${NIN_MS_FZ10:-}" == 1 ]] || { echo "NIN_MS_NULLGUARD needs NIN_MS_FZ10"; exit 1; }
    cat > "$B/nullguard.S" <<'PYASM'
	.text
	.globl guard
guard:                            # replaces 'lwz r4,__DSP_rude_task' at 0x803363C4 (rude branch, curr != rude) in __DSPHandler
	lwz	4,-0x4484(13)             # __DSP_rude_task
	cmplwi	4,0
	bnelr                         # normal: back to the 'bl __DSP_exec_task'
	lis	10,0xD302                 # NULL next task: __DSP_exec_task would load next->iram_mmem_addr from address 0xC (DSI loop)
	lwz	11,0x6718(10)
	addi	11,11,1
	stw	11,0x6718(10)             # NULLGUARD count
	lwz	11,-0x4474(13)
	stw	11,0x671C(10)             # __DSP_curr_task at that time
	lwz	11,-0x4488(13)
	stw	11,0x67D8(10)             # __DSP_rude_task_pending at that time
	li	11,0
	stw	11,-0x4488(13)            # pending = 0
	lis	10,0x8033
	ori	10,10,0x665C              # .L_8033665C: the common tail of __DSPHandler
	mtlr	10
	blr
PYASM
    "$DEVKITPPC/bin/powerpc-eabi-as" -mgekko -o "$B/nullguard.o" "$B/nullguard.S" && "$DEVKITPPC/bin/powerpc-eabi-objcopy" -O binary -j .text "$B/nullguard.o" "$B/nullguard.bin" || { echo "nullguard stub assembly failed"; exit 1; }
    python3 - "$t/kernel/Patch.c" "$t/kernel/vbba_glue.c" "$B/nullguard.bin" <<'PYK'
import sys, struct
pa, g, binp = sys.argv[1:4]
w = open(binp, 'rb').read()
assert len(w) == 68, len(w)
arr = ", ".join("0x%08X" % x for x in struct.unpack(">17I", w))
t = open(pa).read()
a = "\t\t\t\t\tPatchBL( PatchCopy(FakeInterrupt, FakeInterrupt_size), (u32)Buffer + i + 4 );\n"
assert t.count(a) == 1, "Patch.c anchor"
blk = """	{	/* Multishine NULLGUARD: no __DSP_exec_task(curr, NULL) from the rude-task branch of __DSPHandler */
		static const u32 guard[17] = { %s };
		u32 hb = (u32)Buffer + i - 0x1C - 0x115E0u, site = hb + 0x168;
		if (read32(site) == 0x808DBB7C) {
			u32 st = PatchCopy((const u8 *)guard, sizeof guard);
			PatchBL(st, site);
			dbgprintf("NULLGUARD: installed (stub %%08X)\\r\\n", st);
		} else dbgprintf("NULLGUARD: site differs (%%08X), not installed\\r\\n", read32(site));
	}
""" % arr
t = t.replace(a, a + blk)
open(pa, 'w').write(t)
u = open(g).read()
f = 'read32(0x13026710));'
assert u.count(f) == 1, "FZ10 print anchor"
u = u.replace(f, f + '\n\tsdumpf("NULLGUARD: fired %u times (last: __DSP_curr_task %08X, rude pending %08X)\\r\\n", read32(0x13026718), read32(0x1302671C), read32(0x130267D8));')
open(g, 'w').write(u)
PYK
    grep -q "NULLGUARD:" "$t/kernel/vbba_glue.c" && echo "NULLGUARD build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_BBACH2:-}" == 1 ]]; then   # test build (finding 51): the virtual BBA ALSO answers on EXI channel 2 device 0 (the engine moves to it when the kernel says so), so its select / callback / mask state is not the card's; the guest EXIDMA stub touches the OS interrupt mask only when a callback comes with the DMA (as the SDK does); needs NIN_MS_EXIPUB for the marker word
    [[ "${NIN_MS_EXIPUB:-}" == 1 ]] || { echo "NIN_MS_BBACH2 needs NIN_MS_EXIPUB"; exit 1; }
    python3 - "$t/kernel/EXI.c" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
a1 = "\t\t\t\t\t\tcase EXI_DEV_SP1:\n\t\t\t\t\t\t\t// Device is present."
assert s.count(a1) == 1
s = s.replace(a1, "\t\t\t\t\t\tcase EXI_DEV_SP1:\n\t\t\t\t\t\tcase EXI_DEV_AD16:      /* Multishine: the virtual BBA on channel 2 device 0 */\n\t\t\t\t\t\t\t// Device is present.")
a2 = "\t\t\t\t\t\tcase EXI_DEV_AD16:\n\t\t\t\t\t\tdefault:"
assert s.count(a2) == 1
s = s.replace(a2, "\t\t\t\t\t\tdefault:")
a3 = "\t\t\t\t\tcase EXI_DEV_SP1:\n\t\t\t\t\t\tif (vbbaActive)\n\t\t\t\t\t\t{\n\t\t\t\t\t\t\t// Multishine: virtual BBA.  Reads"
assert s.count(a3) == 1
s = s.replace(a3, "\t\t\t\t\tcase EXI_DEV_SP1:\n\t\t\t\t\tcase EXI_DEV_AD16:\n\t\t\t\t\t\tif (vbbaActive)\n\t\t\t\t\t\t{\n\t\t\t\t\t\t\t// Multishine: virtual BBA.  Reads")
a4 = "\t\t\t\t\tcase EXI_DEV_SP1:\n\t\t\t\t\t\tif (vbbaActive)\n\t\t\t\t\t\t{\n\t\t\t\t\t\t\t// Multishine: virtual BBA DMA"
assert s.count(a4) == 1
s = s.replace(a4, "\t\t\t\t\tcase EXI_DEV_SP1:\n\t\t\t\t\tcase EXI_DEV_AD16:\n\t\t\t\t\t\tif (vbbaActive)\n\t\t\t\t\t\t{\n\t\t\t\t\t\t\t// Multishine: virtual BBA DMA")
# the capability marker next to the EXPB marker, and the card counter counts channel 0 only
a5 = "write32(0x13026694, 0x45585042);"
assert s.count(a5) == 1
s = s.replace(a5, "write32(0x13026694, 0x45585042); write32(0x1302669C, 0x42434832);")
a6 = "static void exiCardCnt(u32 command) { u32 d = ((command >> 24) == 0x10) ? ((command >> 8) & 0xFF) : EXIDeviceSelect[0]; if (d != 2) {"
assert s.count(a6) == 1
s = s.replace(a6, "static void exiCardCnt(u32 command) { u32 sel = ((command >> 24) == 0x10); u32 c = sel ? (command & 0xFF) : ((command >> 20) & 0xF); u32 d = sel ? ((command >> 8) & 0xFF) : EXIDeviceSelect[c & 3]; if (c == 0 && d != 2) {")
open(p, "w").write(s)
PY
    python3 - "$t/kernel/asm/EXIDMA.S" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
a = "CallbackSet:\n"
assert s.count(a) == 1
s = s.replace(a, "CallbackSet:\n\tmr\t\tr11,\tr7\t\t# finding 51: remember whether a callback came with the DMA (after the first five instructions: Patch.c patches words at +2, +6 and +0x12)\n")
b = "\tmfmsr\t\tr9\t\t\t# "
assert s.count(b) == 1
s = s.replace(b, "\tcmpwi\t\tr11,\t0\t\t# finding 51: no callback, no interrupt-mask change (the SDK unmasks only for a DMA with a callback)\n\tbeq\t\tSkipMask\n" + b)
c = "\tstw\t\tr0,\tEXI_CMD_0@l(r7)"
assert s.count(c) == 1
s = s.replace(c, "SkipMask:\n\tstw\t\tr0,\tEXI_CMD_0@l(r7)")
open(p, "w").write(s)
PY
    grep -q "AD16:      /\* Multishine" "$t/kernel/EXI.c" && grep -q "SkipMask" "$t/kernel/asm/EXIDMA.S" && echo "BBACH2 test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_PVBBA:-}" == 1 ]]; then   # the PV fast path.  EXI channel 2 device 1 answers its select while the vBBA runs; every DMA on it is ONE whole operation of the chip model (core/vbba_chip.c vbba_chip_pv_tx / vbba_chip_pv_rx: a datagram out, or the oldest received frame / 'none' in): one kernel main-loop pass per datagram instead of 5-19.  An immediate transfer on the device is answered with 0 (the PowerPC never waits on it).  Older kernels answer the select with 0, and the engine then keeps the register path.  Needs NIN_MS_BBACH2 (the BBA owns channel 2).
    [[ "${NIN_MS_BBACH2:-}" == 1 ]] || { echo "NIN_MS_PVBBA needs NIN_MS_BBACH2"; exit 1; }
    python3 - "$t/kernel/EXI.c" "$t/kernel/vbba_glue.c" "$t/kernel/vbba_glue.h" <<'PYPV'
import sys
e, g, h = sys.argv[1:4]
s = open(e).read()
a = "\n#ifdef GCNCARD_ENABLE_SLOT_B\n\t\t\t\t\t\tcase EXI_DEV_MEMCARD_B:\n\t\t\t\t\t\t\tret = GCNCard_IsEnabled(1);"
assert s.count(a) == 1, "PVBBA select anchor"
s = s.replace(a, "\n\t\t\t\t\t\tcase EXI_DEVICE_NUMBER(2, 1):   /* Multishine PV fast path: present while the vBBA runs */\n\t\t\t\t\t\t\tret = vbbaActive ? 1 : 0;\n\t\t\t\t\t\t\tVBBAPvSel(ret);   /* the first answers in the log */\n\t\t\t\t\t\t\tbreak;\n" + a)
a = "\t\t\t\t\tcase EXI_DEV_MASK_ROM_RTC_SRAM_UART:\n\t\t\t\t\t\tEXIDevice_ROM_RTC_SRAM_UART( (u8*)data, len, mode );"
assert s.count(a) == 1, "PVBBA imm anchor"
s = s.replace(a, "\t\t\t\t\tcase EXI_DEVICE_NUMBER(2, 1):   /* Multishine PV device: no immediate transfers; answered so the PowerPC never waits */\n\t\t\t\t\t\twrite32( EXI_CMD_1, 0 );\n\t\t\t\t\t\twrite32( EXI_CMD_0, 0 );\n\t\t\t\t\t\tsync_after_write((void*)EXI_BASE, 0x20);\n\t\t\t\t\t\tbreak;\n\n" + a)
a = "\t\t\t\t\tcase EXI_DEV_MASK_ROM_RTC_SRAM_UART:\n\t\t\t\t\t\tEXIDevice_ROM_RTC_SRAM_UART( ptr, len, mode );"
assert s.count(a) == 1, "PVBBA dma anchor"
s = s.replace(a, "\t\t\t\t\tcase EXI_DEVICE_NUMBER(2, 1):   /* Multishine PV device: one whole datagram (write) or the next received frame / 'none' (read) per DMA */\n\t\t\t\t\t\tif (vbbaActive) VBBAPvDma( ptr, len, mode );\n\t\t\t\t\t\twrite32( EXI_CMD_0, 0 );\n\t\t\t\t\t\tsync_after_write((void*)EXI_BASE, 0x20);\n\t\t\t\t\t\tbreak;\n\n" + a)
open(e, 'w').write(s)
w = open(g).read()
a = "\n/* ---- the thread ---- */\n"
assert w.count(a) == 1, "PVBBA glue anchor"
b = "void VBBAExiDma(u8 *ptr, u32 len, u32 mode) { (void)ptr; (void)len; (void)mode; }\n"
assert w.count(b) == 1, "PVBBA stub anchor"
w = w.replace(b, b + "void VBBAPvDma(u8 *ptr, u32 len, u32 mode) { (void)ptr; (void)len; (void)mode; }\nvoid VBBAPvSel(u32 ret) { (void)ret; }\n")
w = w.replace(a, '\n/* PV fast path: EXI channel 2 device 1, one DMA = one operation of the chip model; the cache handling of VBBAExiDma */\nstatic u32 vgPvSel, vgPvTx, vgPvRx, vgPvRxGot;   /* PV commands served (the first ones are logged; RAM ring only during play) */\nvoid VBBAPvSel(u32 ret)\n{\n\tif (++vgPvSel <= 3u) dbgprintf("VBBA: PV select #%u answered %u (vBBA %s) at %u ms\\r\\n", vgPvSel, ret, V ? "up" : "not up", vg_now_ms(NULL));\n}\nvoid VBBAPvDma(u8 *ptr, u32 len, u32 mode)\n{\n\tu32 a = (u32)ptr & ~31u, n = round32(((u32)ptr & 31u) + len);\n\tint r;\n\tif (!V) { if (vgPvTx + vgPvRx == 0u) dbgprintf("VBBA: PV transfer before the vBBA exists (mode %u len %u)\\r\\n", mode, len); return; }\n\tif (mode == 1) { sync_before_read((void *)a, n); r = vbba_chip_pv_tx(&V->chip, ptr, len); if (++vgPvTx == 1u || r) dbgprintf("VBBA: PV send #%u: %u bytes, header %02X%02X, rc %d at %u ms\\r\\n", vgPvTx, len, ptr[0], ptr[1], r, vg_now_ms(NULL)); }\n\telse { r = vbba_chip_pv_rx(&V->chip, ptr, len); sync_after_write((void *)a, n);\n\t\tif (++vgPvRx <= 4u || r < 0 || (r > 0 && ++vgPvRxGot <= 2u)) {   /* also where the answer went and its first 16 bytes (1257 field: the engine read 13 C7 where the kernel wrote \'P\' \'V\') */\n\t\t\tdbgprintf("VBBA: PV poll #%u: buffer %u, frame %d, ring rwp %u rrp %u at %u ms\\r\\n", vgPvRx, len, r, vbba_chip_rwp(&V->chip), vbba_chip_rrp(&V->chip), vg_now_ms(NULL));\n\t\t\tdbgprintf("VBBA: PV poll #%u at %08X (EXI %08X): %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\\r\\n", vgPvRx, (u32)ptr, read32(EXI_CMD_1),\n\t\t\t\tptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5], ptr[6], ptr[7], ptr[8], ptr[9], ptr[10], ptr[11], ptr[12], ptr[13], ptr[14], ptr[15]);\n\t\t} }\n}\n' + a, 1)
open(g, 'w').write(w)
v = open(h).read()
a = "void VBBAExiDma(u8 *ptr, u32 len, u32 mode);\n"
assert v.count(a) == 1, "PVBBA header anchor"
v = v.replace(a, a + "void VBBAPvDma(u8 *ptr, u32 len, u32 mode);   /* PV fast path (NIN_MS_PVBBA) */\nvoid VBBAPvSel(u32 ret);   /* logs the first PV select answers */\n")
open(h, 'w').write(v)
PYPV
    grep -q "VBBAPvDma( ptr" "$t/kernel/EXI.c" && echo "PVBBA build: PV fast path on EXI channel 2 device 1" | tee -a "$log"
  fi
  if [[ "${NIN_MS_PVSNAP:-}" == 1 ]]; then   # (diagnostic): VBBAPvDma prints the answer header as the ARM sees it before vbba_chip_pv_rx, after it (bytes and 32-bit reads), after the flush and after the first log line, plus EXI_CMD_0 and EXI_INT, for the first 4 polls, failing ones and the first 2 frames (field: delivered headers read wrong)
    [[ "${NIN_MS_PVBBA:-}" == 1 ]] || { echo "NIN_MS_PVSNAP needs NIN_MS_PVBBA"; exit 1; }
    python3 - "$t/kernel/vbba_glue.c" <<'PYSN'
import sys
p = sys.argv[1]; s = open(p).read()
a = '\telse { r = vbba_chip_pv_rx(&V->chip, ptr, len); sync_after_write((void *)a, n);\n\t\tif (++vgPvRx <= 4u || r < 0 || (r > 0 && ++vgPvRxGot <= 2u)) {'
assert s.count(a) == 1, "PVSNAP anchor"
s = s.replace(a, '\telse {\n\t\tu8 s0[8], s1[8], s2[8], s3[8]; u32 w0a, w0b, w1a, w1b, c0, ei, k;   /* PVSNAP: the header as the ARM sees it before the call, after it, after the flush, after the first log line */\n\t\tfor (k = 0; k < 8u; k++) s0[k] = ptr[k];\n\t\tr = vbba_chip_pv_rx(&V->chip, ptr, len);\n\t\tfor (k = 0; k < 8u; k++) s1[k] = ptr[k];\n\t\tw0a = read32((u32)ptr); w0b = read32((u32)ptr + 4u);\n\t\tsync_after_write((void *)a, n);\n\t\tfor (k = 0; k < 8u; k++) s2[k] = ptr[k];\n\t\tw1a = read32((u32)ptr); w1b = read32((u32)ptr + 4u);\n\t\tc0 = read32(EXI_CMD_0); ei = read32(EXI_INT);\n\t\tif (++vgPvRx <= 4u || r < 0 || (r > 0 && ++vgPvRxGot <= 2u)) {\n\t\t\tdbgprintf("VBBA: PV poll #%u: buffer %u, frame %d, ring rwp %u rrp %u at %u ms\\r\\n", vgPvRx, len, r, vbba_chip_rwp(&V->chip), vbba_chip_rrp(&V->chip), vg_now_ms(NULL));\n\t\t\tfor (k = 0; k < 8u; k++) s3[k] = ptr[k];\n\t\t\tdbgprintf("VBBA: PVSNAP #%u before %02X%02X%02X%02X %02X%02X%02X%02X ret %02X%02X%02X%02X %02X%02X%02X%02X w32 %08X %08X\\r\\n", vgPvRx,\n\t\t\t\ts0[0], s0[1], s0[2], s0[3], s0[4], s0[5], s0[6], s0[7], s1[0], s1[1], s1[2], s1[3], s1[4], s1[5], s1[6], s1[7], w0a, w0b);\n\t\t\tdbgprintf("VBBA: PVSNAP #%u sync %02X%02X%02X%02X %02X%02X%02X%02X w32 %08X %08X log %02X%02X%02X%02X %02X%02X%02X%02X cmd0 %08X int %08X\\r\\n", vgPvRx,\n\t\t\t\ts2[0], s2[1], s2[2], s2[3], s2[4], s2[5], s2[6], s2[7], w1a, w1b, s3[0], s3[1], s3[2], s3[3], s3[4], s3[5], s3[6], s3[7], c0, ei);\n\t\t}\n\t\tif (vgPvRx <= 4u || r < 0 || (r > 0 && vgPvRxGot <= 2u && vgPvRxGot >= 1u)) {')
open(p, 'w').write(s)
PYSN
    grep -q "PVSNAP #" "$t/kernel/vbba_glue.c" && echo "PVSNAP build: PV header snapshots in the log" | tee -a "$log"
  fi
  if [[ "${NIN_MS_PVCHK:-}" == 1 ]]; then   # (diagnostic): after the flush the delivered frame is read back from the PowerPC buffer and compared with the ring (vbba_chip_pv_cmp); the count and first differing offset, and the first 64 bytes, are logged for the first 3 frames of each length and the first 8 frames that differ
    [[ "${NIN_MS_PVSNAP:-}" == 1 ]] || { echo "NIN_MS_PVCHK needs NIN_MS_PVSNAP"; exit 1; }
    python3 - "$t/kernel/vbba_glue.c" <<'PYCK'
import sys
p = sys.argv[1]; s = open(p).read()
for a, b in (('\t\tu8 s0[8], s1[8], s2[8], s3[8]; u32 w0a, w0b, w1a, w1b, c0, ei, k;', '\t\tu8 s0[8], s1[8], s2[8], s3[8]; u32 w0a, w0b, w1a, w1b, c0, ei, k, rr0 = vbba_chip_rrp(&V->chip);'), ('\t\tc0 = read32(EXI_CMD_0); ei = read32(EXI_INT);\n', '\t\tc0 = read32(EXI_CMD_0); ei = read32(EXI_INT);\n\t\tif (r > 0) {   /* PVCHK: the frame read back from the PowerPC buffer after the flush, compared with the ring */\n\t\t\tstatic u32 vgPvLen[8], vgPvLenC[8], vgPvLenN, vgPvBad, vgPvBadLog;\n\t\t\tint f; u32 m = vbba_chip_pv_cmp(&V->chip, rr0, (const volatile u8 *)ptr, (u32)r, &f), j, seen = 0, q;\n\t\t\tfor (j = 0; j < vgPvLenN; j++) if (vgPvLen[j] == (u32)r) { seen = ++vgPvLenC[j]; break; }\n\t\t\tif (j == vgPvLenN && vgPvLenN < 8u) { vgPvLen[vgPvLenN] = (u32)r; vgPvLenC[vgPvLenN++] = seen = 1u; }\n\t\t\tif (m) vgPvBad++;\n\t\t\tif ((m && ++vgPvBadLog <= 8u) || (seen && seen <= 3u)) {\n\t\t\t\tdbgprintf("VBBA: PVCHK frame %d at %u ms: %u bytes differ, first at %d (frames with a difference so far %u)\\r\\n", r, vg_now_ms(NULL), m, f, vgPvBad);\n\t\t\t\tfor (q = 0; q < 64u; q += 32u)\n\t\t\t\t\tdbgprintf("VBBA: PVCHK +%02u %02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X\\r\\n", q,\n\t\t\t\t\t\tptr[q], ptr[q+1], ptr[q+2], ptr[q+3], ptr[q+4], ptr[q+5], ptr[q+6], ptr[q+7], ptr[q+8], ptr[q+9], ptr[q+10], ptr[q+11], ptr[q+12], ptr[q+13], ptr[q+14], ptr[q+15],\n\t\t\t\t\t\tptr[q+16], ptr[q+17], ptr[q+18], ptr[q+19], ptr[q+20], ptr[q+21], ptr[q+22], ptr[q+23], ptr[q+24], ptr[q+25], ptr[q+26], ptr[q+27], ptr[q+28], ptr[q+29], ptr[q+30], ptr[q+31]);\n\t\t\t}\n\t\t}\n')):
    assert s.count(a) == 1, "PVCHK anchor"
    s = s.replace(a, b)
open(p, 'w').write(s)
PYCK
    grep -q "PVCHK frame" "$t/kernel/vbba_glue.c" && echo "PVCHK build: delivered frames read back and compared" | tee -a "$log"
  fi
  if [[ -n "${NIN_MS_TCTIME:-}" ]]; then   # test build (finding 52): the card's transfer-complete interrupt is held back NIN_MS_TCTIME HW_TIMER ticks (stock 1900 = 1 ms; 0 = as soon as possible, what the GC BIOS / Triforce use)
    grep -q "^#define EXI_IRQ_DEFAULT		1900" "$t/kernel/EXI.c" || { echo "EXI.c: EXI_IRQ_DEFAULT line not found"; exit 1; }
    perl -pi -e "s/^(#define EXI_IRQ_DEFAULT\t\t)1900/\${1}$NIN_MS_TCTIME/" "$t/kernel/EXI.c"
    grep -q "^#define EXI_IRQ_DEFAULT		$NIN_MS_TCTIME" "$t/kernel/EXI.c" && echo "TCTIME test build: $NIN_MS_TCTIME ticks" | tee -a "$log"
  fi
  if [[ "${NIN_MS_DEFERIOS:-}" == 1 ]]; then   # test build (finding 55): the vBBA makes NO IOS call (ip/top, kd/request, ncd/manage opens, the ncd MAC ioctlv) until its network start is released; the video-retrace hang needs the vBBA present even idle, and these opens are all it does before any traffic
    grep -c "	if (tried && (u32)(now - last_tick) < 1898000u) return;" "$t/kernel/vbba_glue.c" | grep -q '^1$' || { echo "vbba_glue.c: vg_reopen anchor not unique"; exit 1; }
    perl -0pi -e 's/(\tif \(tried && \(u32\)\(now - last_tick\) < 1898000u\) return;)/\tif (!vgNetReleased) return;   \/* DEFERIOS *\/\n$1/' "$t/kernel/vbba_glue.c"
    grep -q "DEFERIOS" "$t/kernel/vbba_glue.c" && echo "DEFERIOS test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_LAZY:-}" == 1 ]]; then   # lazy test build: the vBBA starts only when the guest first selects the BBA channel
    [[ $(grep -c "^VBBA_FLAGS := -std=gnu99" "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_LAZY=1/' "$t/kernel/Makefile"
    grep -q "^VBBA_FLAGS := -std=gnu99.*-DVBBA_LAZY=1" "$t/kernel/Makefile" && echo "LAZY test build" | tee -a "$log"
  fi
  if [[ "${NIN_MS_NOBBA:-}" == 1 ]]; then   # test build without the network: the vBBA is never started, the cfg bit is ignored, the menu row cannot turn it on
    perl -pi -e 's/ncfg->Config \|= NIN_CFG_BBA_EMU;(\s+\/\* Multishine: the virtual BBA)/ncfg->Config &= ~NIN_CFG_BBA_EMU;$1/' "$t/loader/source/main.c"
    perl -pi -e 's/ncfg->Config \|= NIN_CFG_BBA_EMU;\r?$/ncfg->Config &= ~NIN_CFG_BBA_EMU;\r/ if $. > 1400 && $. < 1450' "$t/loader/source/menu.c"
    grep -q "Config &= ~NIN_CFG_BBA_EMU" "$t/loader/source/main.c" && [[ $(grep -c "Config &= ~NIN_CFG_BBA_EMU" "$t/loader/source/menu.c") -ge 1 ]] || { echo "no-network edit failed"; exit 1; }
    echo "NO NETWORK test build: BBA forced off" | tee -a "$log"
  fi
  if [[ "${NIN_MS_CACHEFIX:-0}" == 1 ]]; then   # ROOT CAUSE of the slot B freeze (hardware report found by reading the pinned kernel): ISOSetupCache puts the disc cache behind the memory cards inside a 3 MiB window (CACHE_SIZE 0x300000) and does 'DCacheLimit -= MemCardSize' with MemCardSize = slot A + slot B.  Two 2 MiB cards = 4 MiB > 3 MiB: the u32 wraps to about 4 GB, 'DataCacheOffset + Length >= DCacheLimit' never fires again, and the cache keeps writing disc data further and further past 0x11400000 until it hits something the kernel needs (the freeze comes at a big read, e.g. a scene change).  With slot A alone (2 MiB) the limit is 1 MiB: fine, which is why it ran until a backup card existed.  The fix keeps the stock arithmetic when the cards leave room in the window and otherwise gives the cache 1 MiB behind the cards, always clamped to the 16 MiB card region (0x12000000)
    python3 - "$t/kernel/ISO.c" <<'PYCF'
import sys
p = sys.argv[1]
s = open(p, newline="").read()
old = "\t\tDCCache += MemCardSize; //memcard is before cache\n\t\tDCacheLimit -= MemCardSize;"
nl = "\r\n" if "\r\n" in s else "\n"
old = old.replace("\n", nl)
assert s.count(old) == 1, "ISOSetupCache card lines not found"
new = ("\t\tDCCache += MemCardSize; //memcard is before cache" + nl +
       "\t\t/* Multishine (NIN_MS_CACHEFIX): the stock 'DCacheLimit -= MemCardSize' wraps when slot A + slot B are 3 MiB or more */" + nl +
       "\t\tif (MemCardSize >= CACHE_SIZE) DCacheLimit = 0x100000; else DCacheLimit -= MemCardSize;" + nl +
       "\t\tif ((u32)DCCache >= 0x12000000) DCacheLimit = 0; else if (DCacheLimit > 0x12000000 - (u32)DCCache) DCacheLimit = 0x12000000 - (u32)DCCache;")
s = s.replace(old, new)
open(p, "w", newline="").write(s)
PYCF
    grep -q "NIN_MS_CACHEFIX" "$t/kernel/ISO.c" || { echo "cache fix edit failed"; exit 1; }
    echo "CACHEFIX build: ISO cache limit cannot wrap" | tee -a "$log"
  fi
  if [[ "${NIN_MS_WIFIFIX:-0}" == 1 ]]; then   # Wi-Fi consoles: roaming off (each scan stops the link ~150 ms) + WiiConnect24 scheduler paused, from a one-shot kernel thread; ideas from wize-logic/melee-wii, our own code (wifi_fix_patch.py); wired consoles: the thread ends at once
    python3 "$d/wifi_fix_patch.py" "$t/kernel/vbba_glue.c" || { echo "wifi fix edit failed"; exit 1; }
    grep -q "VBBAWifiThread" "$t/kernel/vbba_glue.c" || { echo "wifi fix edit failed"; exit 1; }
    echo "WIFIFIX build: roam_off + WiiConnect24 pause thread" | tee -a "$log"
  fi
  if [[ "${NIN_MS_HB:-}" == 1 ]]; then   # q33-diag: a live trail that does not depend on the main loop: hb/ms_hb.c (sampler + writer threads, the IOS / FatFs calls --wrap'ed per thread, an SD gate in disk_read_sd / disk_write_sd), SD:/hb.log rewritten in place (raw 4 KB writes at sectors fixed at boot), decoder the project's other tooling.  Off by default: the release profile and q32 are unchanged
    [[ "${NIN_MS_LOG:-}" == 2 && "${NIN_MS_STALL:-}" == 1 ]] || { echo "NIN_MS_HB needs NIN_MS_LOG=2 and NIN_MS_STALL=1 (the msCrumb main-loop stage words)"; exit 1; }
    cp "$d/hb/ms_hb.c" "$d/hb/ms_hb_fmt.h" "$t/kernel/"
    python3 - "$t/kernel" <<'PYHB'
import sys, os
k = sys.argv[1]
def edit(name, a, b, count=1):
    p = os.path.join(k, name); s = open(p).read()
    assert s.count(a) == count, "%s: anchor %r found %d times" % (name, a, s.count(a))
    open(p, 'w').write(s.replace(a, b))
# main.c: the file and its sectors right after the RUNID line (main thread, before the game), the threads after 'Kernel Start', the pass counter at the top of the loop, stop at game exit
a = '\tdbgprintf("RUNID: game'
p = os.path.join(k, 'main.c'); s = open(p).read(); assert s.count(a) == 1, "main.c RUNID"
i = s.index('\n', s.index(a)) + 1
open(p, 'w').write(s[:i] + '\t{ extern void hb_init(u32); hb_init(UseUSB); }   /* Multishine q33-diag heartbeat */\n' + s[i:])
edit('main.c', '\tdbgprintf("Kernel Start\\r\\n");\n', '\tdbgprintf("Kernel Start\\r\\n");\n\t{ extern void hb_start(u32); hb_start(DI_Thread); }   /* Multishine q33-diag heartbeat */\n')
edit('main.c', '\twhile (1)\n\t{\n\t\tMS_ST(MSS_TOP);\n', '\twhile (1)\n\t{\n\t\tMS_ST(MSS_TOP);\n\t\t{ extern vu32 hbMain; hbMain++; }   /* q33-diag */\n')
edit('main.c', '\t\t\tdbgprintf("Game Exit\\r\\n");\n', '\t\t\tdbgprintf("Game Exit\\r\\n");\n\t\t\t{ extern void hb_stop(void); hb_stop(); }   /* q33-diag: no raw SD write after this */\n')
# common.c: Shutdown writes the last live block first
edit('common.c', '\tdbgprintf("Got Shutdown button call\\n");', '\t{ extern void hb_shutdown(void); hb_shutdown(); }   /* q33-diag */\n\tdbgprintf("Got Shutdown button call\\n");')
# diskio.c: the SD sector functions get the gate (ms_hb.c defines disk_read_sd / disk_write_sd around the renamed originals)
edit('diskio.c', 'DRESULT disk_read_sd (', 'DRESULT disk_read_sd_hbraw (')
edit('diskio.c', 'DRESULT disk_write_sd (', 'DRESULT disk_write_sd_hbraw (')
edit('diskio.c', 'void SetDiskFunctions(DWORD usb)\n', 'DRESULT disk_read_sd(BYTE pdrv, BYTE *buff, DWORD sector, UINT count);   /* q33-diag: kernel/ms_hb.c */\nDRESULT disk_write_sd(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count);\nvoid SetDiskFunctions(DWORD usb)\n')
# vbba_glue.c: the vBBA words the sampler records
p = os.path.join(k, 'vbba_glue.c'); s = open(p).read()
s += r"""

/* q33-diag (NIN_MS_HB): the vBBA state for the heartbeat sampler (kernel/ms_hb.c) */
void vgHbInfo(u32 *o)
{
	u32 i, n = 0;
	o[0] = (vgStarted ? 0x80000000u : 0u) | (vgNetReleased ? 0x40000000u : 0u) | (vst ? (vst->main_beat & 0x3FFFFFFFu) : 0u);
	if (slots && vgStarted) for (i = 0; i < VBBA_SLOTS; i++) if (slots[i].used) n++;
	o[1] = (n << 16) | (vst ? (vst->q_now & 0xFFFFu) : 0u);
	o[2] = vst ? (((vst->tx_inflight_now & 0xFFFFu) << 16) | (vst->ios_timeouts & 0xFFFFu)) : 0u;
	o[3] = vgNetThread; o[4] = vgDoneThread;
#if VBBA_SYNCTX
	o[5] = vgTxThread;
#else
	o[5] = 0;
#endif
}
"""
open(p, 'w').write(s)
# Makefile: the object and the wraps
w = ['IOS_Open', 'IOS_Close', 'IOS_Read', 'IOS_Write', 'IOS_Seek', 'IOS_Ioctl', 'IOS_Ioctlv', 'f_open_char', 'f_close', 'f_read', 'f_write', 'f_lseek', 'f_sync']
edit('Makefile', '\t   SDI.o usb.o usbstorage.o wdvd.o sock.o vbba_glue.o\n', '\t   SDI.o usb.o usbstorage.o wdvd.o sock.o vbba_glue.o ms_hb.o\n')
edit('Makefile', 'LDFLAGS\t:= -nostartfiles -nodefaultlibs -mbig-endian -Wl,--gc-sections,-T,kernel.ld,-Map,kernel.map -n\n',
     'LDFLAGS\t:= -nostartfiles -nodefaultlibs -mbig-endian -Wl,--gc-sections,-T,kernel.ld,-Map,kernel.map -n ' + ' '.join('-Wl,--wrap=' + x for x in w) + '\n')
PYHB
    grep -q "hb_start" "$t/kernel/main.c" && grep -q "ms_hb.o" "$t/kernel/Makefile" && echo "HB test build (q33-diag heartbeat, /hb.log)" | tee -a "$log"
  fi
  # loader screen tag "Multishine vBBA <id>" (menu.c MULTISHINE_TAG): id = SHA-1 prefix over the patch and the linked vBBA sources
  msid=$( { cat "$patch" "$patch2" "$patch3" "$patch4" "$patch5" "$patch6" "$d/ms_meta.py"; [[ "${NIN_MS_DNS:-}" == 1 ]] && cat "$patch7"; [[ "${NIN_MS_NET:-}" == 1 ]] && cat "$patch8"; [[ -n "${NIN_MS_ONLINE_INFO:-}" ]] && cat "$patch9" "$NIN_MS_ONLINE_INFO"; [[ "${NIN_MS_LOG:-}" == 1 || "${NIN_MS_LOG:-}" == 2 ]] && cat "$patch10"; [[ "${NIN_MS_LOG:-}" == 2 ]] && cat "$patch11"; [[ "${NIN_MS_MAC:-}" == 1 ]] && cat "$patch12"; [[ "${NIN_MS_STALL:-}" == 1 ]] && cat "$patch15"; [[ "${NIN_MS_RTCFG:-}" == 1 ]] && cat "$patch16"; [[ "${NIN_MS_WIRED:-}" == 1 ]] && cat "$patch17"; [[ "${NIN_MS_OSDUMP:-}" == 1 ]] && cat "$patch18"; [[ "${NIN_MS_NETEARLY:-}" == 1 ]] && cat "$patch19"; [[ "${NIN_MS_SDLOG:-}" == 1 ]] && cat "$patch20"; [[ "${NIN_MS_SDPROBE:-}" == 1 ]] && cat "$patch21"; [[ "${NIN_MS_AUTOPOKE:-}" == 1 ]] && cat "$patch22"; [[ "${NIN_MS_EXIFIX:-}" == 1 ]] && cat "$patch23"; [[ "${NIN_MS_PCSAMPLE:-}" == 1 ]] && cat "$patch24"; [[ "${NIN_MS_GATE2:-}" == 1 ]] && cat "$patch25"; [[ "${NIN_MS_QUIET:-}" == 1 ]] && cat "$patch26"; [[ "${NIN_MS_MACPUB:-}" == 1 ]] && cat "$patch27"; [[ "${NIN_MS_ROAM:-}" == 1 ]] && cat "$patch29"; [[ "${NIN_MS_WC24:-}" == 1 ]] && cat "$patch30"; echo "release $rel nobba ${NIN_MS_NOBBA:-0} lazy ${NIN_MS_LAZY:-0} netdelay ${NIN_MS_NETDELAY:-0}"; cat "$d"/core/*.[ch] "$d"/platform/nintendont/*.[ch]; [[ "${NIN_MS_HB:-}" != 1 ]] || cat "$d"/hb/*.[ch]; echo "step $step"; [[ -z "$split" ]] || echo "split $split"; [[ -z "$tweak" ]] || echo "tweak $tweak"; } | shasum | cut -c1-7)
  [[ -n "${NIN_MS_BUILD_ID:-}" ]] && msid=$NIN_MS_BUILD_ID   # reproduce the id of an earlier build (e.g. to prove a comment-only source change leaves the binaries identical)
  # VBBA_TWEAK is a space-separated list; every b10/b11 build must carry plainprobe too (b10a..b11a built without it were b9a-minus-the-fix: the live-stack invalidate came back)
  # b9 tweaks (VBBA_TWEAK): plainprobe = the stack high-water probe reads the stacks with plain loads (no sync_before_read = no cache invalidate of a live stack)
  if [[ " $tweak " == *" plainprobe "* ]]; then
    [[ $(grep -c 'sync_before_read((void \*)base, bytes);' "$t/kernel/vbba_glue.c") == 1 ]]
    perl -pi -e 's/^\tsync_before_read\(\(void \*\)base, bytes\);\r?\n//' "$t/kernel/vbba_glue.c"
    ! grep -q 'sync_before_read((void \*)base, bytes);' "$t/kernel/vbba_glue.c"
  # b10 tweaks: idle1ms = the fast thread sleeps 1000 us instead of 50 us when idle (VBBA_IDLE_US, ifndef-guarded in the glue; about 20x fewer IOS timers and ARM wake-ups, H1/H2 of)
  fi
  if [[ " $tweak " == *" idle1ms "* ]]; then
    [[ $(grep -c '^+\?VBBA_FLAGS := -std=gnu99' "$patch") -ge 1 ]]
    [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_IDLE_US=1000/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99 -DVBBA_IDLE_US=1000' "$t/kernel/Makefile"
  # b10b tx16: 16 send buffers and 16 sendto requests in flight (b9a: 8/8), step-1 tree otherwise unchanged
  fi
  if [[ " $tweak " == *" tx16 "* ]]; then
    [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_TP_IOS_TX=16 -DVBBA_TP_IOS_TX_INFLIGHT=16/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99 -DVBBA_TP_IOS_TX=16 -DVBBA_TP_IOS_TX_INFLIGHT=16' "$t/kernel/Makefile"
  # b10c cap2: at most 2 sendto requests in IOS; the 8 buffers of b9a queue the rest (FIFO code is linked at step 1)
  fi
  if [[ " $tweak " == *" cap2 "* ]]; then
    [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_TP_IOS_TX_INFLIGHT=2/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99 -DVBBA_TP_IOS_TX_INFLIGHT=2' "$t/kernel/Makefile"
  # b11a startwait: no 15 s abandon of the start-up call (VBBA_TMO_START_MS 60000) and a 60 s address wait (IP_WAIT_MS 60000) = one start-up sequence, no variant rotation for 60 s
  fi
  if [[ " $tweak " == *" startwait "* ]]; then
    [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_TMO_START_MS=60000 -DIP_WAIT_MS=60000u/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99 -DVBBA_TMO_START_MS=60000 -DIP_WAIT_MS=60000u' "$t/kernel/Makefile"
  fi
  # b13 tweak: yield1ms = after ~2 ms of continuous EXI service the fast thread yields 1000 us instead of 100 us (VBBA_YIELD_US, ifndef-guarded): the thread can use at most about 2/3 of the ARM, so the main loop (DI/SI) and the IOS network process get time under heavy guest traffic
  if [[ " $tweak " == *" yield1ms "* ]]; then
    [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_YIELD_US=1000/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99 -DVBBA_YIELD_US=1000' "$t/kernel/Makefile"
  fi
  # b14 tweak: nonblock = the vBBA's UDP socket is made non-blocking (fcntl) and receives are polled every 2 ms instead of keeping 2 blocking recvfrom requests in IOS (hypothesis: IOS serves /dev/net/ip/top requests in order, so a blocking recvfrom holds up the sendtos behind it)
  if [[ " $tweak " == *" nonblock "* ]]; then
    [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_TP_NONBLOCK=1/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99 -DVBBA_TP_NONBLOCK=1' "$t/kernel/Makefile"
  fi
  # b15 tweak: inflight1 = at most ONE sendto request in IOS (the next is handed over when the previous completion is collected); the FIFO of 8 buffers queues the rest. libogc's net_sendto is strictly one request at a time.
  if [[ " $tweak " == *" inflight1 "* ]]; then
    [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_TP_IOS_TX_INFLIGHT=1/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99 -DVBBA_TP_IOS_TX_INFLIGHT=1' "$t/kernel/Makefile"
  fi
  # b16 tweak: prio48 = the fast EXI thread runs at priority 0x48 (below the main loop's 0x50) instead of 0x70, so IOS's own network threads and the main loop are not preempted by it while it spins
  if [[ " $tweak " == *" prio48 "* ]]; then
    [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_THREAD_PRIO=0x48/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99 -DVBBA_THREAD_PRIO=0x48' "$t/kernel/Makefile"
  fi
  # b18 tweak: rxpoll = the libogc receive pattern (VBBA_TP_RXPOLL): the socket is made non-blocking, no recvfrom stands in IOS; every 2 ms ONE poll(POLLIN, 0), a recvfrom only when it reports data, and sendto / poll / recvfrom never overlap. Needs inflight1.
  if [[ " $tweak " == *" rxpoll "* ]]; then
    [[ " $tweak " == *" inflight1 "* ]] || { echo "rxpoll needs inflight1"; exit 1; }
    [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_TP_RXPOLL=1/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99.* -DVBBA_TP_RXPOLL=1' "$t/kernel/Makefile"
  fi
  # main<N> (b17d: main1000, b21: main2000 / main4000) = the Nintendont main loop sleeps N us per pass instead of 20 us while the vBBA runs (-DVBBA_MAIN_US=N; applied at every VBBA_STEP).
  # Confirmed on the console (b17d,): the 10,000 timer create/destroy pairs a second starved the IOS network threads; sendto completion 27 ms -> 4-5 ms., 22
  for w in $tweak; do
    if [[ $w =~ ^main([0-9]+)$ ]]; then
      mn=${BASH_REMATCH[1]}
      [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
      perl -pi -e "s/^(VBBA_FLAGS := -std=gnu99)/\$1 -DVBBA_MAIN_US=$mn/" "$t/kernel/Makefile"
      grep -q "^VBBA_FLAGS := -std=gnu99.* -DVBBA_MAIN_US=$mn" "$t/kernel/Makefile"
    fi
  done
  # b19 tweak: iosbuf = each sendto's datagram, params and vectors are copied into blocks of the stock socket code's IOS heap (soHeap) before IOS_IoctlvAsync
  if [[ " $tweak " == *" iosbuf "* ]]; then
    [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_IOSBUF=1/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99.* -DVBBA_IOSBUF=1' "$t/kernel/Makefile"
  fi
  # b20 tweak: synctx = sendto is a blocking IOS_Ioctlv from a dedicated sender thread (libogc's call shape), result posted to the completion queue. Needs inflight1.
  if [[ " $tweak " == *" synctx "* ]]; then
    [[ " $tweak " == *" inflight1 "* ]] || { echo "synctx needs inflight1"; exit 1; }
    [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_SYNCTX=1/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99.* -DVBBA_SYNCTX=1' "$t/kernel/Makefile"
  fi
  # b22a tweak: engstat = the periodic "VBBA: tl@" sets (and the shutdown set) also print a read-only snapshot of the PowerPC engine's status blocks (S3 + S7, found by their magic words) and of the interrupt words the ARM keeps for the PowerPC (-DVBBA_ENGSTAT=1)
  if [[ " $tweak " == *" engstat "* ]]; then
    [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_ENGSTAT=1/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99.* -DVBBA_ENGSTAT=1' "$t/kernel/Makefile"
  fi
  # b22b tweak: scenestat = the same sets also print PowerPC game words (scene id, match frame, scene frames, game-end id), the DI registers/flags and the emulated card state (-DVBBA_SCENESTAT=1 for the glue and for kernel/GCNCard.c, which gets a read-only accessor)
  if [[ " $tweak " == *" scenestat "* ]]; then
    [[ $(grep -c '^VBBA_FLAGS := -std=gnu99' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(VBBA_FLAGS := -std=gnu99)/$1 -DVBBA_SCENESTAT=1/' "$t/kernel/Makefile"
    grep -q '^VBBA_FLAGS := -std=gnu99.* -DVBBA_SCENESTAT=1' "$t/kernel/Makefile"
    [[ $(grep -c '^\$(VBBA_OBJS) vbba_glue.o: CFLAGS += \$(VBBA_FLAGS)' "$t/kernel/Makefile") == 1 ]]
    perl -pi -e 's/^(\$\(VBBA_OBJS\) vbba_glue\.o: CFLAGS \+= \$\(VBBA_FLAGS\))(\r?)$/$1$2\nGCNCard.o: CFLAGS += -DVBBA_SCENESTAT=1$2/' "$t/kernel/Makefile"
    grep -q '^GCNCard.o: CFLAGS += -DVBBA_SCENESTAT=1' "$t/kernel/Makefile"
  fi
  for w in $tweak; do [[ $w =~ ^main[0-9]+$ || " plainprobe idle1ms tx16 cap2 startwait yield1ms nonblock inflight1 prio48 rxpoll iosbuf synctx engstat scenestat " == *" $w "* ]] || { echo "unknown VBBA_TWEAK $w"; exit 1; }; done
  # b8 split of step 2 (VBBA_SPLIT): a = kernel cfg parse + log only (the loader writes no mailbox), b = loader mailbox only (kernel stays at step 1)
  lstep=$step; kstep=$step
  [[ -z "$split" || "$step" == 2 ]] || { echo "VBBA_SPLIT needs VBBA_STEP=2"; exit 1; }
  if [[ "$split" == a ]]; then perl -pi -e 's/if \(VBBA_STEP < 2\) return;/return; \/\* b8a: no mailbox \*\//' "$t/loader/source/vbba_duty.c"; grep -q 'b8a: no mailbox' "$t/loader/source/vbba_duty.c"; fi
  if [[ "$split" == b ]]; then kstep=1; fi
  [[ -z "$split" || "$split" == a || "$split" == b ]] || { echo "VBBA_SPLIT must be a or b"; exit 1; }
  if [[ "${NIN_MS_ROAM:-}" == 1 ]]; then [[ "${NIN_MS_WIRED:-}" == 1 ]] || { echo "NIN_MS_ROAM needs NIN_MS_WIRED"; exit 1; }; (cd "$t" && git apply --whitespace=nowarn "$patch29"); echo "applied $(basename "$patch29")" | tee -a "$log"; fi   # LAST glue edits: patches 0029/0030 are cut from the tree after every other patch, flag edit (GAMELOG, PVBBA, ...) and tweak
  if [[ "${NIN_MS_WC24:-}" == 1 ]]; then [[ "${NIN_MS_ROAM:-}" == 1 ]] || { echo "NIN_MS_WC24 needs NIN_MS_ROAM"; exit 1; }; (cd "$t" && git apply --whitespace=nowarn "$patch30"); echo "applied $(basename "$patch30")" | tee -a "$log"; fi   # closed here (the blocks below used to sit inside it, so NETAUTO/WLPM/TOS needed NIN_MS_WC24)
  if [[ "${NIN_MS_IOSMARK:-}" == 1 ]]; then   # WHICH call made a long ARM main-loop pass.  Every IOS_Open/Close/Ioctl/Ioctlv/IoctlAsync/IoctlvAsync in vbba_glue.c (all the vBBA's own IOS calls, roam_off /dev/wl0 and WC24 kd/request included) is timed; per pass the longest MAIN-THREAD call (its vbba_glue.c line in the build tree) and the longest MS_ST stage segment go into the ARMLOG ring, and the call in flight is shown at the dump.  Needs NIN_MS_ARMLOG.
    [[ "${NIN_MS_ARMLOG:-}" == 1 ]] || { echo "NIN_MS_IOSMARK needs NIN_MS_ARMLOG"; exit 1; }
    python3 - "$t/kernel/vbba_glue.c" "$t/kernel/vbba_stall.h" <<'PYIM'
import sys
gl, st = sys.argv[1:3]
g = open(gl).read()
a = '#include "vbba_stall.h"'
assert g.count(a) == 1, "IOSMARK include anchor"
g = g.replace(a, a + r'''
/* IOSMARK: every IOS call of this file is timed (the line number names the call site in the build tree) */
void amNoteIos(u32 line, u32 ticks);
extern volatile u32 amIosLine, amIosT0;
#define AM_IOS_T(call) ({ u32 _amt = read32(HW_TIMER); s32 _amr; amIosLine = __LINE__; amIosT0 = _amt | 1u; _amr = (call); amIosLine = 0; amNoteIos(__LINE__, read32(HW_TIMER) - _amt); _amr; })
#define IOS_Open(...) AM_IOS_T((IOS_Open)(__VA_ARGS__))
#define IOS_Ioctl(...) AM_IOS_T((IOS_Ioctl)(__VA_ARGS__))
#define IOS_Ioctlv(...) AM_IOS_T((IOS_Ioctlv)(__VA_ARGS__))
#define IOS_IoctlAsync(...) AM_IOS_T((IOS_IoctlAsync)(__VA_ARGS__))
#define IOS_IoctlvAsync(...) AM_IOS_T((IOS_IoctlvAsync)(__VA_ARGS__))
#define IOS_Close(...) ({ u32 _amt = read32(HW_TIMER); amIosLine = __LINE__; amIosT0 = _amt | 1u; (IOS_Close)(__VA_ARGS__); amIosLine = 0; amNoteIos(__LINE__, read32(HW_TIMER) - _amt); })
''', 1)
for a, b in (
    ("static struct { u32 diT, diN, offw, lenw, modew, svT, slT; } amCur;", "static struct { u32 diT, diN, offw, lenw, modew, svT, slT, ioL, ioT, ioN, ioS, sgS, sgT; } amCur;"),
    ("static struct { u32 t, gap, diT, diN, off, len, mode, svT, slT; } amRing[32];", "static struct { u32 t, gap, diT, diN, off, len, mode, svT, slT, ioL, ioT, ioN, ioS, sgS, sgT; } amRing[32];\n"
     "volatile u32 amIosLine, amIosT0;\nstatic u32 amMainTid;\n"
     "void amNoteIos(u32 line, u32 ticks) { if (!amMainTid || (u32)thread_get_id() != amMainTid) return; amCur.ioN++; amCur.ioS += ticks; if (ticks > amCur.ioT) { amCur.ioT = ticks; amCur.ioL = line; } }\n"
     "void amNoteSeg(u32 stage, u32 ticks) { if (!amMainTid || (u32)thread_get_id() != amMainTid) return; if (ticks > amCur.sgT) { amCur.sgT = ticks; amCur.sgS = stage; } }"),
    ("amRing[i].svT = amCur.svT; amRing[i].slT = amCur.slT;", "amRing[i].svT = amCur.svT; amRing[i].slT = amCur.slT;\n\t\tamRing[i].ioL = amCur.ioL; amRing[i].ioT = amCur.ioT; amRing[i].ioN = amCur.ioN; amRing[i].ioS = amCur.ioS; amRing[i].sgS = amCur.sgS; amRing[i].sgT = amCur.sgT;"),
    ("amCur.diT = amCur.diN = amCur.offw = amCur.lenw = amCur.modew = amCur.svT = amCur.slT = 0;", "amCur.diT = amCur.diN = amCur.offw = amCur.lenw = amCur.modew = amCur.svT = amCur.slT = 0;\n\tamCur.ioL = amCur.ioT = amCur.ioN = amCur.ioS = amCur.sgS = amCur.sgT = 0;\n\tif (!amMainTid) amMainTid = (u32)thread_get_id();   /* amPass runs on the main loop */"),
):
    assert g.count(a) == 1, a[:50]
    g = g.replace(a, b)
a = "| sleep overshoot %u ms\\r\\n\", "
assert g.count(a) == 1, "IOSMARK ring line"
g = g.replace(a, "| sleep overshoot %u ms | IOS longest glue line %u %u ms (%u calls %u ms) | stage longest %u %u ms\\r\\n\", ")
a = "amRing[k].slT / 1898u);"
assert g.count(a) == 1, "IOSMARK ring args"
g = g.replace(a, "amRing[k].slT / 1898u, amRing[k].ioL, amRing[k].ioT / 1898u, amRing[k].ioN, amRing[k].ioS / 1898u, amRing[k].sgS, amRing[k].sgT / 1898u);")
a = "\tu32 i, n = amN > 32u ? 32u : amN, now = read32(HW_TIMER);\n"
assert g.count(a) == 1
g = g.replace(a, a + "\t{ u32 l = amIosLine, t0 = amIosT0; if (l) sdumpf(\"SHUTDUMP: IOS call in flight: glue line %u for %u ms\\r\\n\", l, (now - (t0 & ~1u)) / 1898u); }\n")
open(gl, 'w').write(g)
s = open(st).read()
a = "#define MS_ST(s) do { if (!msCrumb.wd) { msCrumb.stage = (s); msCrumb.t = read32(HW_TIMER); msCrumb.n++; } } while (0)"
assert s.count(a) == 1, "IOSMARK MS_ST"
s = s.replace(a, "extern void amNoteSeg(u32 stage, u32 ticks);   /* IOSMARK: the longest stage segment of the pass */\n"
              "#define MS_ST(s) do { if (!msCrumb.wd) { u32 _mt = read32(HW_TIMER); amNoteSeg(msCrumb.stage, _mt - msCrumb.t); msCrumb.stage = (s); msCrumb.t = _mt; msCrumb.n++; } } while (0)")
open(st, 'w').write(s)
PYIM
    echo "IOSMARK build: long ARM passes name the IOS call / stage" | tee -a "$log"
  fi
  if [[ "${NIN_MS_WLSIG:-}" == 1 ]]; then   # Wi-Fi signal word.  IOS writes into MEM1 0x31A0 (Nintendont's Patch31A0 moves game code away from it: "This area gets used by IOS"); melee-wii records it as the last received frame's signal info.  Wireless interface only: sampled every 250 ms by the main loop (one uncached read, no IOS call), kept as a 64-sample ring, per-byte min/mean/max and 16-bucket histograms, the receive gaps >= 100 ms after steady traffic with the word before and after, and the newest word on every long ARM pass.  Summary in game.log.  Needs NIN_MS_IOSMARK.
    [[ "${NIN_MS_ARMLOG:-}" == 1 ]] || { echo "NIN_MS_WLSIG needs NIN_MS_ARMLOG"; exit 1; }   # with or without IOSMARK
    python3 - "$t/kernel/vbba_glue.c" <<'PYWS'
import sys
gl = sys.argv[1]
g = open(gl).read()
def rep(a, b):
    global g
    assert g.count(a) == 1, "WLSIG anchor: " + a[:60]
    g = g.replace(a, b)
rep("\tif (!vgStarted) return;\n\tif (last_tick) {", "\tif (!vgStarted) return;\n\t{ extern void vg_wsig(u32 now); vg_wsig(now); }   /* WLSIG: Wi-Fi signal word every 250 ms */\n\tif (last_tick) {")
if "sgS, sgT; } amRing[32];" in g:                      # IOSMARK built in
    rep("sgS, sgT; } amRing[32];", "sgS, sgT, wsW, wsA; } amRing[32];\nextern u32 wsLast, wsLastT;")
    rep("amRing[i].sgS = amCur.sgS; amRing[i].sgT = amCur.sgT;", "amRing[i].sgS = amCur.sgS; amRing[i].sgT = amCur.sgT; amRing[i].wsW = wsLast; amRing[i].wsA = wsLastT ? now - wsLastT : 0;")
    rep("| stage longest %u %u ms\\r\\n\", ", "| stage longest %u %u ms | wsig %08X %u ms old\\r\\n\", ")
    rep("amRing[k].sgS, amRing[k].sgT / 1898u);", "amRing[k].sgS, amRing[k].sgT / 1898u, amRing[k].wsW, amRing[k].wsA / 1898u);")
else:                                                    # the plain ARMLOG ring
    rep("svT, slT; } amRing[32];", "svT, slT, wsW, wsA; } amRing[32];\nextern u32 wsLast, wsLastT;")
    rep("amRing[i].svT = amCur.svT; amRing[i].slT = amCur.slT;", "amRing[i].svT = amCur.svT; amRing[i].slT = amCur.slT; amRing[i].wsW = wsLast; amRing[i].wsA = wsLastT ? now - wsLastT : 0;")
    rep("| sleep overshoot %u ms\\r\\n\", ", "| sleep overshoot %u ms | wsig %08X %u ms old\\r\\n\", ")
    rep("amRing[k].slT / 1898u);", "amRing[k].slT / 1898u, amRing[k].wsW, amRing[k].wsA / 1898u);")
i = g.index('glf("GAMELOG: PASSHIST'); j = g.index("\n", i) + 1
g = g[:j] + "\t{ extern void vg_wsig_log(void); vg_wsig_log(); }   /* WLSIG */\n" + g[j:]
g += r"""

/* WLSIG: the word IOS keeps at MEM1 0x31A0 (signal info of the last received Wi-Fi frame, per melee-wii; FORMAT NOT VERIFIED: logged raw and per byte).  Main loop only, Wi-Fi only. */
#define WS_N 64u
static struct { u32 t, w; } wsRing[WS_N];
static struct { u32 t, gap, w0, w1; } wsGap[16];
u32 wsLast, wsLastT;
static u32 wsN, wsChg, wsMin[4] = { 255u, 255u, 255u, 255u }, wsMax[4], wsSum[4], wsHist[4][16], wsGapN, wsRxN, wsRxT, wsRxPrevGap = 0xFFFFFFFFu, wsAtRx;
void vg_wsig(u32 now)
{
	static u32 next;
	u32 rx, w, k;
	if (vgCfgKind != 1) return;
	rx = vst->rx_dgrams;
	if (rx != wsRxN) {                                   /* a datagram came in: close the receive gap; record it if it is >= 100 ms after steady (< 50 ms) traffic */
		u32 gap = now - wsRxT;
		if (wsRxT && gap >= 189800u && wsRxPrevGap < 94900u) { u32 i = wsGapN++ & 15u; wsGap[i].t = now; wsGap[i].gap = gap; wsGap[i].w0 = wsAtRx; wsGap[i].w1 = wsLast; }
		wsRxPrevGap = wsRxT ? gap : 0xFFFFFFFFu; wsRxN = rx; wsRxT = now; wsAtRx = wsLast;
	}
	if (next && (s32)(now - next) < 0) return;
	next = now + 474500u;                                /* 250 ms */
	sync_before_read((void *)0x31A0, 32);
	w = read32(0x31A0);
	wsRing[wsN % WS_N].t = now; wsRing[wsN % WS_N].w = w;
	if (wsN && w != wsLast) wsChg++;
	wsN++; wsLast = w; wsLastT = now;
	for (k = 0; k < 4u; k++) { u32 b = (w >> (24u - 8u * k)) & 255u; if (b < wsMin[k]) wsMin[k] = b; if (b > wsMax[k]) wsMax[k] = b; wsSum[k] += b; wsHist[k][b >> 4]++; }
}
void vg_wsig_log(void)
{
	u32 k, i, now = read32(HW_TIMER), best = 0, n, lo, cnt;
	if (!wsN) { glf("GAMELOG: WSIG not sampled (interface kind %u; 1 = Wi-Fi)\r\n", vgCfgKind); return; }
	glf("GAMELOG: WSIG word at MEM1 0x31A0 (Wi-Fi, every 250 ms; format not verified): samples %u, changes %u, newest %08X %u ms old\r\n", wsN, wsChg, wsLast, (now - wsLastT) / 1898u);
	for (k = 0; k < 4u; k++) {
		glf("GAMELOG: WSIG byte %u (bits %u-%u): min %u mean %u max %u | hist16 %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\r\n", k, 31u - 8u * k, 24u - 8u * k, wsMin[k], wsSum[k] / wsN, wsMax[k],
			wsHist[k][0], wsHist[k][1], wsHist[k][2], wsHist[k][3], wsHist[k][4], wsHist[k][5], wsHist[k][6], wsHist[k][7], wsHist[k][8], wsHist[k][9], wsHist[k][10], wsHist[k][11], wsHist[k][12], wsHist[k][13], wsHist[k][14], wsHist[k][15]);
		if (wsMax[k] - wsMin[k] > wsMax[best] - wsMin[best]) best = k;
	}
	lo = wsMin[best] + (wsMax[best] - wsMin[best]) / 4u;     /* GUESS: the byte that varies most is the signal; 'weak' = the lowest quarter of its range (counted on the ring) */
	n = wsN < WS_N ? wsN : WS_N;
	for (i = cnt = 0; i < n; i++) if (((wsRing[i].w >> (24u - 8u * best)) & 255u) < lo) cnt++;
	glf("GAMELOG: WSIG guess: byte %u varies most; newest %u samples below %u (lowest quarter): %u\r\n", best, n, lo, cnt);
	for (i = 0; i < 16u && i < wsN; i += 8u) {
		u32 j, m = wsN - 1u - i, v[16];
		for (j = 0; j < 8u; j++) { u32 e = (m >= j && wsN - (m - j) <= WS_N) ? (m - j) % WS_N : 0xFFFFFFFFu; v[2 * j] = e != 0xFFFFFFFFu ? wsRing[e].w : 0; v[2 * j + 1] = e != 0xFFFFFFFFu ? (now - wsRing[e].t) / 1898u : 0; }
		glf("GAMELOG: WSIG newest %u-%u: %08X@%u %08X@%u %08X@%u %08X@%u %08X@%u %08X@%u %08X@%u %08X@%u ms\r\n", i, i + 7u, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
	}
	glf("GAMELOG: WSIG receive gaps >= 100 ms after steady traffic: %u (newest 16 below)\r\n", wsGapN);
	n = wsGapN > 16u ? 16u : wsGapN;
	for (i = 0; i < n; i++) { u32 e = (wsGapN - 1u - i) & 15u; glf("GAMELOG: WSIG gap %u ms, ended %u ms ago: word at the last datagram %08X, at the gap end %08X\r\n", wsGap[e].gap / 1898u, (now - wsGap[e].t) / 1898u, wsGap[e].w0, wsGap[e].w1); }
}
"""
open(gl, 'w').write(g)
PYWS
    echo "WLSIG build: Wi-Fi signal word (MEM1 0x31A0) in game.log" | tee -a "$log"
  fi
  if [[ "${NIN_MS_NETSEG:-}" == 1 ]]; then   # per-connection network table in game.log (needs NIN_MS_WLSIG for the hooks' place)
    [[ "${NIN_MS_WLSIG:-}" == 1 ]] || { echo "NIN_MS_NETSEG needs NIN_MS_WLSIG"; exit 1; }
    python3 - "$t/kernel/vbba_glue.c" <<'PYNS'
import sys
gl = sys.argv[1]
g = open(gl).read()
a = "\t{ extern void vg_wsig(u32 now); vg_wsig(now); }   /* WLSIG: Wi-Fi signal word every 250 ms */\n"
assert g.count(a) == 1, "NETSEG hook"
g = g.replace(a, a + "\t{ extern void vg_netseg(u32 now); vg_netseg(now); }   /* NETSEG */\n")
a = "\t{ extern void vg_wsig_log(void); vg_wsig_log(); }   /* WLSIG */\n"
assert g.count(a) == 1, "NETSEG log hook"
g = g.replace(a, a + "\t{ extern void vg_netseg_log(void); vg_netseg_log(); }   /* NETSEG */\n")
g += r"""
/* NETSEG (, every interface): one row per connection segment (receive traffic that starts after >= 10 s of quiet and ends after 10 s of quiet): when, how long,
   datagrams in / out, the longest receive gap and when, and the datagrams received in each of the first 60 seconds (vBBA counters: the opponent plus a little server / DNS). */
#define NS_N 6u
static struct { u32 t0, t1, rx0, tx0, rx, tx, gmax, gat; u8 sec[60]; u8 open; } nsSeg[NS_N];
static u32 nsN, nsLastRx, nsLastT, nsSecT, nsSecRx;
void vg_netseg(u32 now)
{
	u32 rx = vst->rx_dgrams, i;
	if (!nsLastT) { nsLastT = nsSecT = now; nsLastRx = nsSecRx = rx; return; }
	i = (nsN + NS_N - 1u) % NS_N;
	if (rx != nsLastRx) {
		u32 g = now - nsLastT;
		if (nsN && nsSeg[i].open && g > nsSeg[i].gmax) { nsSeg[i].gmax = g; nsSeg[i].gat = now - nsSeg[i].t0; }
		nsLastRx = rx; nsLastT = now;
	}
	if ((u32)(now - nsSecT) < 1898000u) return;
	{
		u32 d = rx - nsSecRx;
		nsSecT = now; nsSecRx = rx;
		if (nsN && nsSeg[i].open) {
			u32 s = (now - nsSeg[i].t0) / 1898000u;
			if (s < 60u) nsSeg[i].sec[s] = (u8)(d > 255u ? 255u : d);
			nsSeg[i].rx = rx - nsSeg[i].rx0; nsSeg[i].tx = vst->tx_udp - nsSeg[i].tx0; nsSeg[i].t1 = now;
			if ((u32)(now - nsLastT) >= 10u * 1898000u) nsSeg[i].open = 0;
		} else if (d >= 5u) {
			i = nsN++ % NS_N;
			memset(&nsSeg[i], 0, sizeof nsSeg[i]);
			nsSeg[i].t0 = nsSeg[i].t1 = now; nsSeg[i].rx0 = rx - d; nsSeg[i].tx0 = vst->tx_udp; nsSeg[i].open = 1; nsSeg[i].sec[0] = (u8)(d > 255u ? 255u : d);
		}
	}
}
void vg_netseg_log(void)
{
	u32 k, n = nsN > NS_N ? NS_N : nsN, now = read32(HW_TIMER);
	char b[260];
	glf("GAMELOG: NETSEG %u connection segment(s) since the start (newest %u below)\r\n", nsN, n);
	for (k = 0; k < n; k++) {
		u32 i = (nsN - n + k) % NS_N, j, o = 0;
		b[0] = 0;
		for (j = 0; j < 60u && o + 5u < sizeof b; j++) {          /* (no snprintf in the kernel) */
			u32 v = nsSeg[i].sec[j];
			if (j) b[o++] = ' ';
			if (v >= 100u) b[o++] = (char)('0' + v / 100u);
			if (v >= 10u) b[o++] = (char)('0' + v / 10u % 10u);
			b[o++] = (char)('0' + v % 10u);
		}
		b[o] = 0;
		glf("GAMELOG: NETSEG %u: began %u s ago, %u s long%s, rx %u tx %u, longest rx gap %u ms at +%u s | rx/s 0-59: %s\r\n", nsN - n + k + 1u, (now - nsSeg[i].t0) / 1898000u,
		    (nsSeg[i].t1 - nsSeg[i].t0) / 1898000u, nsSeg[i].open ? " (open)" : "", nsSeg[i].rx, nsSeg[i].tx, nsSeg[i].gmax / 1898u, nsSeg[i].gat / 1898000u, b);
	}
}
"""
open(gl, 'w').write(g)
PYNS
    echo "NETSEG build: per-connection network table in game.log" | tee -a "$log"
  fi
  if [[ "${NIN_MS_CCHECK:-}" == 1 ]]; then   # (TEST kernel, never published without our): the Connection check.  The PPC writes a request into the mailbox at ARM 0x13026600 (PPC 0xD3026600, 28 words); the kernel then sends 60 echo datagrams a second for 45 s through the SAME vbba_transport (IOS sendto / recvfrom) to the server's UDP wire port + 1, counts loss / reorder / duplicates / RTT / receive gaps and writes a result block and a verdict (core/vbba_cc.[ch], host-tested in tests/cc_test.c).  Runs from the same place that polls the chip and router (thread, single-context main loop, takeover), one extra call per pass; idle cost = one cache invalidate every 25 ms.
    [[ "${NIN_MS_RXPUB:-}" != 1 ]] || { echo "NIN_MS_CCHECK shares the cache line 0x13026660 with the RXPUB word 0x13026670: refused with NIN_MS_RXPUB"; exit 1; }
    python3 - "$t/kernel/vbba_glue.c" "$t/kernel/Makefile" <<'PYCC'
import sys
gl, mk = sys.argv[1:3]
g = open(gl).read(); m = open(mk).read()
def rep(txt, a, b):
    assert txt.count(a) == 1, "CCHECK anchor: " + a[:70]
    return txt.replace(a, b)
m = rep(m, "VBBA_OBJS := vbba_chip.o vbba_router.o vbba_ndexi.o vbba_tp_ios.o", "VBBA_OBJS := vbba_chip.o vbba_router.o vbba_ndexi.o vbba_tp_ios.o vbba_cc.o")
m = rep(m, "$(VBBA_OBJS) vbba_glue.o: CFLAGS += $(VBBA_FLAGS)\n", "$(VBBA_OBJS) vbba_glue.o: CFLAGS += $(VBBA_FLAGS)\nvbba_cc.o: CFLAGS += -Os   # CCHECK: size, not speed\n")
open(mk, 'w').write(m)
g = rep(g, '#include "vbba_tp_ios.h"', '#include "vbba_tp_ios.h"\n#include "vbba_cc.h"   /* CCHECK */')
g = rep(g, "\tvbba_router_init(&V->router, &rc, vbba_tp_ios_transport(&V->tp));\n", "\tvbba_router_init(&V->router, &rc, vbba_tp_ios_transport(&V->tp));\n\t{ extern void vg_cc_init(void); vg_cc_init(); }   /* CCHECK: mailbox + state */\n")
g = rep(g, "if (EXILockTry()) { (void)vbba_chip_poll(&V->chip, 4); EXIUnlock(); }      /* the chip is also driven by whoever serves EXI */",
        "if (EXILockTry()) { (void)vbba_chip_poll(&V->chip, 4); { extern void vg_cc_step(void); vg_cc_step(); } EXIUnlock(); }      /* the chip is also driven by whoever serves EXI; CCHECK steps with the router */")
g = rep(g, "\t\tif (EXILockTry()) { (void)vbba_chip_poll(&V->chip, 4); EXIUnlock(); }\n\t\tif ((++pass & 255u) == 0) vg_status_update();",
        "\t\tif (EXILockTry()) { (void)vbba_chip_poll(&V->chip, 4); { extern void vg_cc_step(void); vg_cc_step(); } EXIUnlock(); }   /* CCHECK steps with the router */\n\t\tif ((++pass & 255u) == 0) vg_status_update();")
g = rep(g, "\tif (vbbaTakeover) (void)vbba_chip_poll(&V->chip, 4);   /* the thread's background work */",
        "\tif (vbbaTakeover) { (void)vbba_chip_poll(&V->chip, 4); { extern void vg_cc_step(void); vg_cc_step(); } }   /* the thread's background work (+ CCHECK) */")
if 'glf("GAMELOG: PASSHIST' in g:                      # (the host glue test tree has no GAMELOG block: no log hook there)
    i = g.index('glf("GAMELOG: PASSHIST'); j = g.index("\n", i) + 1
    g = g[:j] + "\t{ extern void vg_cc_log(void); vg_cc_log(); }   /* CCHECK */\n" + g[j:]
g += r"""

/* CCHECK: the Connection check, core/vbba_cc.[ch].  Mailbox words 0..23 are written by the ARM (three whole cache lines), 24..27 by the PowerPC (the line at 0x13026660, which the ARM only invalidates and reads),
   so cleaning an ARM line can never write a stale PPC word back.  ARM stores are aligned 32-bit (write32).  Everything below runs where the router runs. */
extern void glf(const char *fmt, ...);
static struct vbba_cc vgCc;
static u32 vgCcOk, vgCcLast, vgCcUs, vgCcRem;
static u32 cc_now(void *x)   /* microseconds since the first call, from HW_TIMER (1.898 MHz) without 64-bit arithmetic; survives the timer wrap */
{
	u32 raw = read32(HW_TIMER), d = raw - vgCcLast;
	(void)x;
	vgCcLast = raw; vgCcUs += (d / 1898u) * 1000u; vgCcRem += (d % 1898u) * 1000u; vgCcUs += vgCcRem / 1898u; vgCcRem %= 1898u;
	return vgCcUs;
}
static void cc_fetch(void *x, u32 first, u32 n, u32 *dst)
{
	u32 i;
	(void)x;
	sync_before_read_align32((void *)(VBBA_CC_BASE + 4u * first), n * 4u);
	for (i = 0; i < n; i++) dst[i] = read32(VBBA_CC_BASE + 4u * (first + i));
}
static void cc_pub(void *x, u32 first, u32 n, const u32 *src)
{
	u32 i;
	(void)x;
	for (i = 0; i < n; i++) write32(VBBA_CC_BASE + 4u * (first + i), src[i]);
	sync_after_write_align32((void *)(VBBA_CC_BASE + 4u * first), n * 4u);
}
static u32 cc_gtx(void *x) { (void)x; return V->router.st.tx_udp; }
static void cc_txlat(void *x, struct vbba_cc_txlat *o, int rm) { (void)x; o->n = V->tp.st.tx_lat_n; o->sum_us = V->tp.st.tx_lat_sum_us; o->max_us = V->tp.st.tx_lat_max_us; if (rm) V->tp.st.tx_lat_max_us = 0; }
static void cc_restore(void *x, u32 old) { (void)x; if (V->tp.st.tx_lat_max_us < old) V->tp.st.tx_lat_max_us = old; }
static void cc_arm(void *x, u32 *a, u32 *b) { (void)x; *a = vgPsH[0]; *b = vgPsH[1]; }
static u32 cc_link(void *x) { (void)x; return vgCfgKind == 1 ? VBBA_CC_L_WIFI : vgCfgKind == 2 ? VBBA_CC_L_WIRED : VBBA_CC_L_UNKNOWN; }
static u32 cc_wsig(void *x) { (void)x; sync_before_read((void *)0x31A0, 32); return read32(0x31A0); }
void vg_cc_init(void)
{
	struct vbba_cc_hooks h;
	h.ctx = 0; h.now_us = cc_now; h.mb_fetch = cc_fetch; h.mb_publish = cc_pub; h.guest_tx = cc_gtx; h.txlat = cc_txlat; h.txlat_restore = cc_restore; h.arm_passes = cc_arm; h.link_type = cc_link; h.wsig = cc_wsig;
	vgCcLast = read32(HW_TIMER); vgCcUs = 0; vgCcRem = 0;
	vbba_cc_init(&vgCc, &h, vbba_tp_ios_transport(&V->tp));
	vgCcOk = 1;
	dbgprintf("VBBA: connection check ready (mailbox PPC 0xD3026600, ABI %u)\r\n", VBBA_CC_ABI);
}
void vg_cc_step(void) { if (vgCcOk) vbba_cc_step(&vgCc); }
void vg_cc_log(void)
{
	const struct vbba_cc_result *r = vbba_cc_result(&vgCc);
	if (!vgCcOk || vgCc.phase == VBBA_CC_P_IDLE) return;
	glf("GAMELOG: CCHECK verdict %u (2 good 3 okay 4 poor 5 failed) reason %u link %u | sent %u recv %u lost %u ooo %u dup %u loss %u/10000\r\n", vgCc.verdict, vgCc.reason, vgCc.link, r->sent, r->recv, r->lost, r->ooo, r->dup, r->loss_x100);
	glf("GAMELOG: CCHECK rtt us min %u p50 %u p95 %u p99 %u max %u mean %u jitter %u | gaps >=100 ms %u >=250 ms %u longest %u ms\r\n", r->rtt_min, r->p50, r->p95, r->p99, r->rtt_max, r->rtt_mean, r->jitter, r->gap100, r->gap250, r->gap_max_ms);
	glf("GAMELOG: CCHECK sendto us mean %u max %u | ARM passes >=5 ms %u >=20 ms %u | step gap max %u ms | refused %u slips %u late %u | run %u ds\r\n", r->sendto_mean, r->sendto_max, r->arm5, r->arm20, r->step_gap_max_ms, r->tx_refused, r->slips, r->late, r->run_ds);
	glf("GAMELOG: CCHECK wsig samples %u min %08X mean %08X last %08X (format not verified)\r\n", r->ws_n, r->ws_min, r->ws_mean, r->ws_last);
}
"""
open(gl, 'w').write(g)
PYCC
    echo "CCHECK build: Connection check (mailbox 0x13026600, core/vbba_cc.c)" | tee -a "$log"
  fi
  if [[ "${NIN_MS_ONETHREAD:-}" == 1 ]]; then   # Wi-Fi only, ONE sendto in flight in IOS at a time (vbba_tp_ios_set_tx_cap 1; wired keeps 16).  All socket submits already come from the main thread in this kernel (VBBA_SINGLE, VBBA_SYNCTX 0), so a funnel thread would change nothing; this is the closest small step to melee-wii's one-call-at-a-time Wi-Fi pattern.  The 2 standing recvfroms per socket stay.
    python3 - "$t/kernel/vbba_glue.c" <<'PYOT'
import sys
gl = sys.argv[1]
g = open(gl).read()
a = "\t{ extern void vg_wsig(u32 now); vg_wsig(now); }   /* WLSIG: Wi-Fi signal word every 250 ms */\n"
assert g.count(a) == 1, "ONETHREAD needs WLSIG"
g = g.replace(a, a + "\t{ static int ot; if (!ot && V && vgCfgKind == 1) { ot = 1; vbba_tp_ios_set_tx_cap(&V->tp, 1); dbgprintf(\"VBBA: onethread: Wi-Fi: one sendto in flight at a time (was %u)\\r\\n\", (u32)VBBA_TP_IOS_TX_INFLIGHT); } }   /* ONETHREAD */\n")
a = "\t{ extern void vg_wsig_log(void); vg_wsig_log(); }   /* WLSIG */\n"
assert g.count(a) == 1
g = g.replace(a, a + "\tif (V) glf(\"GAMELOG: onethread: sendto in flight cap %u (interface kind %u; 1 = Wi-Fi gets 1)\\r\\n\", V->tp.tx_cap, vgCfgKind);\n")
open(gl, 'w').write(g)
PYOT
    echo "ONETHREAD build: Wi-Fi one sendto in flight" | tee -a "$log"
  fi
  if [[ "${NIN_MS_NETAUTO:-}" == 1 ]]; then   # / shell: Network Profile auto falls back to a passed wireless connection when the wired one is selected but no USB LAN adapter is attached (ota/ms_netauto.h); LOADER code (shell.dol)
    cp "$d/ota/ms_netauto.h" "$t/loader/source/"
    python3 - "$t/loader/source/main.c" <<'PYNA'
import sys
p = sys.argv[1]; s = open(p).read()
a = "\tif(ncfg->Config & NIN_CFG_BBA_EMU)\n\t{\n\t\tncfg->NetworkProfile &= 3;\n\t\tif(ncfg->NetworkProfile > 0)\n\t\t{"
assert s.count(a) == 1, "NETAUTO anchor"
s = s.replace(a, "\tif(ncfg->Config & NIN_CFG_BBA_EMU)\n\t{\n\t\tu32 msProf;\n\t\tncfg->NetworkProfile &= 3;\n\t\tmsProf = ncfg->NetworkProfile ? ncfg->NetworkProfile : ms_net_auto();   /* NETAUTO: auto may pick a profile for this start */\n\t\tif(msProf > 0)\n\t\t{")
for q in ("if(ncfg->NetworkProfile == 1)", "else if(ncfg->NetworkProfile == 2)"):
    assert s.count("\t\t\t" + q) == 1, q
    s = s.replace("\t\t\t" + q, "\t\t\t" + q.replace("ncfg->NetworkProfile", "msProf"))
b = "static ioctlv IOCTL_Buf[2] ALIGNED(32);\n"
assert s.count(b) == 1, "NETAUTO IOCTL_Buf anchor"
s = s.replace(b, b + '#include "ms_netauto.h"\n')
c = "\tif(ncfg->Config & NIN_CFG_BBA_EMU)\n\t{\n\t\tu32 msProf;"
i = s.index(c)
j = s.rfind("\n}\n", 0, i)          # the start of the function that holds the block: insert the helpers before it
k = s.rfind("\n\n", 0, j)
s = s[:j + 3] + '/* Multishine NETWORK PROFILE AUTO (NIN_MS_NETAUTO, ms_netauto.h): with profile \'auto\' a wired selection on a Wii without a USB LAN adapter falls back to a wireless\n   connection that passed the Wii\'s test (and back to wired when the adapter is there); applied like a forced profile for this start */\nextern s32 USB_OGC_Initialize(void);\nextern s32 USB_OGC_GetDeviceList(usb_device_entry *descr_buffer, u8 num_descr, u8 interface_class, u8 *cnt_descr);\nstatic int ms_usb_lan(void)                          /* 1 an ASIX (Wii LAN adapter) USB device is attached, 0 none, -1 unknown */\n{\n\tusb_device_entry *d;\n\tu8 n = 0, i;\n\tint r = -1;\n\tif (USB_OGC_Initialize() < 0) return -1;\n\td = (usb_device_entry *)memalign(32, 8 * sizeof(usb_device_entry));\n\tif (!d) return -1;\n\tmemset(d, 0, 8 * sizeof(usb_device_entry));\n\tif (USB_OGC_GetDeviceList(d, 8, 0, &n) >= 0) { r = 0; for (i = 0; i < n && i < 8; i++) if (d[i].vid == 0x0B95) r = 1; }\n\tfree(d);\n\treturn r;\n}\nstatic u32 ms_net_auto(void)\n{\n\tunsigned char f[3];\n\tint lan;\n\tu32 p;\n\ts32 fd = IOS_Open("/dev/net/ncd/manage", 0);\n\tif (fd < 0) return 0;\n\tIOCTL_Buf[0].data = loader_stub; IOCTL_Buf[0].len = 0x1B5C;\n\tIOCTL_Buf[1].data = loader_stub + 0x1B60; IOCTL_Buf[1].len = 0x20;\n\tIOS_Ioctlv(fd, 3, 0, 2, IOCTL_Buf);\n\tIOS_Close(fd);\n\tDCInvalidateRange(loader_stub, 0x1C00);\n\tf[0] = loader_stub[0x8]; f[1] = loader_stub[0x924]; f[2] = loader_stub[0x1240];\n\tlan = ms_usb_lan();\n\tp = ms_netauto_pick(f, lan);\n\tgprintf("netauto: flags %02X %02X %02X, USB LAN adapter %d -> profile %u\\r\\n", f[0], f[1], f[2], lan, p);\n\t{ extern void ms_ota_netauto_note(const unsigned char *f, int lan, unsigned p); ms_ota_netauto_note(f, lan, p); }   /* into ota.log */\n\treturn p;\n}\n' + s[j + 3:]
open(p, 'w').write(s)
PYNA
    grep -q "netauto: flags" "$t/loader/source/main.c" && echo "NETAUTO build: Network Profile auto falls back to Wi-Fi without a LAN adapter" | tee -a "$log"
  fi
  if [[ "${NIN_MS_WLPM:-}" == 1 ]]; then   # Wi-Fi power save OFF (WLC_GET_PM 85 / WLC_SET_PM 86 = 0 on /dev/wl0, core/vbba_wl.h vbba_wl_build_cmd), wireless interface only, read first, set only if on, every 10 s like roam_off (patch 0029); separate flag for the A/B
    [[ "${NIN_MS_ROAM:-}" == 1 ]] || { echo "NIN_MS_WLPM needs NIN_MS_ROAM"; exit 1; }
    python3 - "$t/kernel/vbba_glue.c" <<'PYPM'
import sys
p = sys.argv[1]; s = open(p).read()
a = "static void vg_roam(void)\n{"
assert s.count(a) == 1, "WLPM function anchor"
s = s.replace(a, '#if VBBA_ROAM\n/* (NIN_MS_WLPM): Wi-Fi power save OFF (WLC_SET_PM 0 = CAM, the radio always awake) on the wireless interface only, the same /dev/wl0 path as roam_off: 2 s after\n   the address is up and every 10 s, read the mode (WLC_GET_PM) and set 0 only when it is not 0.  Any failure leaves the driver alone; 40 failed rounds and it stops.\n   UNVERIFIED: that IOS\'s WL module takes the plain commands 85 / 86 like the iovars (core/vbba_wl.h). */\nstatic void vg_wlpm(void)\n{\n\tstatic s32 pmFd = -1;\n\tstatic u32 next, started, fails, sets, last = 0xFFFFFFFFu;\n\tu32 now, was;\n\tu8 *io;\n\tchar *name;\n\ts32 rc, rc2;\n\tif (!vgGotIp || vgCfgKind != 1 || vgHeap < 0 || fails >= 40) return;\n\tnow = vg_now_ms(NULL);\n\tif (!started) { started = 1; next = now + 2000u; return; }\n\tif ((s32)(now - next) < 0) return;\n\tnext = now + 10000u;\n\tio = (u8 *)heap_alloc_aligned(vgHeap, VBBA_WL_IO_SIZE, 32);\n\tif (!io) { fails++; return; }\n\tif (pmFd < 0) {\n\t\tname = (char *)heap_alloc_aligned(vgHeap, 32, 32);\n\t\tif (name) { strcpy(name, VBBA_WL_PATH); pmFd = IOS_Open(name, 0); dbgprintf("VBBA: wl: pm open of wl0: %d\\r\\n", pmFd); heap_free(vgHeap, name); }\n\t\tif (pmFd < 0) { fails += 10; heap_free(vgHeap, io); return; }\n\t}\n\tvbba_wl_build_cmd(io, (u32)io & 0x3FFFFFFFu, VBBA_WLC_GET_PM, 0xFFFFFFFFu);\n\tsync_after_write(io, VBBA_WL_IO_SIZE);\n\trc = IOS_Ioctl(pmFd, VBBA_WL_IOCTL_WLC, io, VBBA_WL_IO_SIZE, NULL, 0);\n\tsync_before_read(io, VBBA_WL_IO_SIZE);\n\twas = vbba_wl_value(io);\n\tif (rc < 0 || was > 2u) {                              /* a failed read, or an answer that is no PM mode (0..2): leave the driver alone */\n\t\tfails++;\n\t\tif (fails <= 3u) dbgprintf("VBBA: wl: pm read failed rc %d value %08X\\r\\n", rc, was);\n\t\theap_free(vgHeap, io); return;\n\t}\n\tif (was) {\n\t\tvbba_wl_build_cmd(io, (u32)io & 0x3FFFFFFFu, VBBA_WLC_SET_PM, 0u);\n\t\tsync_after_write(io, VBBA_WL_IO_SIZE);\n\t\trc2 = IOS_Ioctl(pmFd, VBBA_WL_IOCTL_WLC, io, VBBA_WL_IO_SIZE, NULL, 0);\n\t\tsets++;\n\t\tdbgprintf("VBBA: wl: pm was %u, set 0 rc %d (%u set(s) so far)\\r\\n", was, rc2, sets);\n\t\tif (rc2 < 0) fails++;\n\t} else if (last != 0u) dbgprintf("VBBA: wl: pm was 0, nothing to set\\r\\n");\n\tlast = was;\n\theap_free(vgHeap, io);\n}\n#endif\n' + a)
b = "\tvg_roam();\n"
assert s.count(b) == 1, "WLPM call anchor"
s = s.replace(b, b + "\tvg_wlpm();   /* */\n")
open(p, 'w').write(s)
PYPM
    grep -q "vg_wlpm();" "$t/kernel/vbba_glue.c" && echo "WLPM build: Wi-Fi power save off" | tee -a "$log"
  fi
  if [[ "${NIN_MS_TOS:-}" == 1 ]]; then   # IP_TOS 0xB8 (DSCP EF, the WMM voice class) on the vBBA's IOS UDP sockets, wireless interface only (ncd kind 1); setsockopt ioctl 9 as libogc net_setsockopt; separate flag for the A/B
    python3 - "$t/kernel/vbba_glue.c" <<'PYTOS'
import sys
p = sys.argv[1]; s = open(p).read()
a = "\tcase VBBA_IOS_CLOSE:            /* so_close 0x03 */"
assert s.count(a) == 1, "TOS encoder anchor"
s = s.replace(a, '\tcase VBBA_IOS_SETTOS:           /* so_setsockopt 9 (libogc net_setsockopt): {fd, level IPPROTO_IP 0, optname IP_TOS 1, optlen 4, optval[20]} = 36 bytes */\n\t\ts->params[0] = (u32)r->fd; s->params[1] = 0; s->params[2] = 1; s->params[3] = 4; s->params[4] = r->len;\n\t\ts->params[5] = s->params[6] = s->params[7] = s->params[8] = 0;\n\t\tsync_after_write(s, sizeof(*s));\n\t\trc = IOS_IoctlAsync(ipFd, 0x09, s->params, 36, NULL, 0, vgQueue, &s->msg);\n\t\tbreak;\n' + a)
b = "\tvg_jc_drain();"
assert s.count(b) == 1, "TOS update anchor"
s = s.replace(b, '\t{   /* (NIN_MS_TOS): game datagrams marked DSCP EF (TOS 0xB8) on the wireless interface only: the transport sets it on every socket after its bind */\n\t\tstatic u32 tosLogged;\n\t\tif (V && vgCfgKind == 1 && !V->tp.tos) V->tp.tos = 0xB8u;\n\t\tif (V && V->tp.tos_n != tosLogged && tosLogged < 6u) { tosLogged = V->tp.tos_n; dbgprintf("VBBA: tos: set 0x%02X on socket %d rc %d (%u socket(s) so far)\\r\\n", V->tp.tos, (int)V->tp.tos_fd, (int)V->tp.tos_rc, V->tp.tos_n); }\n\t}\n' + b)
open(p, 'w').write(s)
PYTOS
    grep -q "VBBA: tos: set" "$t/kernel/vbba_glue.c" && echo "TOS build: game sockets marked DSCP EF on Wi-Fi" | tee -a "$log"
  fi
    if [[ "${NIN_MS_WC24:-}" == 1 && "${NIN_MS_GAMELOG:-}" == 1 ]]; then   # game exit resumes WiiConnect24 and writes the roam / wc24 summary into game.log (vgGameLogBegin runs before EXIShutdown on the Game Exit path)
      python3 - "$t/kernel/vbba_glue.c" <<'PYWC'
import sys
p = sys.argv[1]; s = open(p).read()
a = "\n}\nvoid vgGameLogEnd(void) { glEnd(); }\n"
assert s.count(a) == 1, "WC24 gamelog anchor"
s = s.replace(a, "\n\t{ extern void VBBAWifiGameLog(void); VBBAWifiGameLog(); }   /* roam / wc24 summary, WiiConnect24 resumed */" + a)
open(p, 'w').write(s)
PYWC
      grep -q "VBBAWifiGameLog(); }" "$t/kernel/vbba_glue.c" || { echo "WC24 gamelog edit failed"; exit 1; }
      echo "WC24 build: summary + resume at game exit (game.log)" | tee -a "$log"
    fi
  sed -i '' "s/@VBBASTEP@/$lstep/" "$t/loader/source/vbba_duty.c"
  sed -i '' "s/@MSBUILD@/$msid/" "$t/loader/source/menu.c"; grep -q "Multishine vBBA $msid" "$t/loader/source/menu.c"; echo "loader tag: Multishine vBBA $msid" | tee -a "$log"
  if [[ -f "$t/kernel/vbba_build_id.h" ]]; then printf '#ifndef VBBA_BUILD_ID_H\n#define VBBA_BUILD_ID_H\n#define VBBA_BUILD_ID "%s "\n#endif\n' "$msid" > "$t/kernel/vbba_build_id.h"; fi   # the kernel prints the same id in its RUNID line
  mkdir -p "$B/hostshim"; echo '#include <stdlib.h>' > "$B/hostshim/malloc.h"
  cc -O2 -w -I"$B/hostshim" "$t/kernel/bin2h/main.c" -o "$t/kernel/bin2h/bin2h"
  { echo "== toolchain"; powerpc-eabi-gcc --version | head -1; arm-none-eabi-gcc --version | head -1; } >> "$log"
  for p in multidol kernel/asm resetstub codehandler loader/source/ppc/PADReadGC loader/source/ppc/IOSInterface; do
    echo "== $p" >> "$log"; make -C "$t/$p" >> "$log" 2>&1; done
  echo "== fatfs ppc" >> "$log"; make -C "$t/fatfs" -f Makefile.ppc >> "$log" 2>&1
  echo "== fatfs arm" >> "$log"; make -C "$t/fatfs" -f Makefile.arm >> "$log" 2>&1
  echo "== kernel MULTISHINE=$B/ms" >> "$log"; make -C "$t/kernel" MULTISHINE="$B/ms" VBBA_STEP=$kstep VBBA_OFF=${VBBA_OFF:-0} ${NIN_MS_QUIET:+VBBA_QUIET=1} >> "$log" 2>&1
  echo "== kernelboot" >> "$log"; make -C "$t/kernelboot" >> "$log" 2>&1
  echo "== loader" >> "$log"; make -C "$t/loader" >> "$log" 2>&1
  echo "== padsyms (second link: libogc pad statics for the pad: log line)" >> "$log"; bash "$d/ota/gen_padsyms.sh" "$t/loader" >> "$log" 2>&1 || { echo "pad symbol pass failed (see $log)"; exit 1; }
  arm-none-eabi-size -A "$t/kernel/kernel.elf" > "$B/kernel_size.txt"
  (cd "$t" && find . -type f -newer "$stamp" ! -name '*.o' ! -name '*.d' ! -name '*.map' | LC_ALL=C sort | xargs shasum -a 256) > "$B/vbba.SHA256SUMS"
  rm -rf "$out/vbba"; mkdir -p "$out/vbba"; cp -R "$t" "$out/vbba/tree"; cp "$log" "$out/vbba/build.log"; cp "$B/vbba.SHA256SUMS" "$out/vbba/SHA256SUMS"; cp "$B/kernel_size.txt" "$out/vbba/"
  echo "== vbba: $(grep -c -i ' warning:' "$log") warnings, $(grep -c -i ' error' "$log") error lines; log $out/vbba/build.log"
  [[ "$t/loader/loader.dol" -nt "$stamp" ]] || { echo "loader.dol was not built (see $log)"; exit 1; }
  echo "   built loader.dol (boot.dol) $(stat -f %z "$t/loader/loader.dol") B"
  cp "$t/loader/loader.dol" "$out/shell.dol"; echo "   shell.dol (the loader DOL: the stub's embedded shell and the shell OTA payload, tools/release/make_shell_ota.py) -> $out/shell.dol"
  cp "$t/loader/data/kernel.zip" "$out/kernel.zip"; echo "   kernel.zip (the kernel OTA payload, tools/release/make_kernel_ota.py) $(stat -f %z "$out/kernel.zip") B -> $out/kernel.zip"
}

case ${1:-build} in
  build) build ;;
  *) echo "usage: $0 build"; exit 2 ;;
esac
