#!/usr/bin/env bash
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
# Host tests: the virtual BBA (unit, ASan/UBSan, fuzz, the Nintendont glue on a pristine bbc0a20 export), the shell's OTA / card / network cores,
# the stub build checks, the glue heap layout; then the shared-code lock and lint.  Nothing leaves 127.0.0.1.
set -uo pipefail
cd "$(dirname "$0")"
export OUT=${OUT:-$PWD/build/hosttest} MS_SERVER_PLACEHOLDER_OK=1 MS_NO_EMULATOR=${MS_NO_EMULATOR-1}
fails=()
step() { echo "== $*"; "$@" || fails+=("$*"); }
step bash offline/nintendont-vbba/run_tests.sh unit
step python3.14 -m unittest discover -s offline/nintendont-vbba/ota/tests -p 'test_*.py'
step python3.14 offline/nintendont-vbba/stub/tests/test_stub_build.py
step python3.14 offline/nintendont-vbba/tests/check_glue_heap.py
step python3.14 tools/lint_repo.py --licence "GPL-3.0-or-later OR GPL-2.0-only,GPL-2.0-only,GPL-2.0-or-later,GPL-3.0-or-later,MIT,Zlib,CC0-1.0"
echo
if ((${#fails[@]})); then printf 'FAILED: %s\n' "${fails[@]}"; exit 1; fi
echo "all test steps passed"
