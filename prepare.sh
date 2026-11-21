#!/usr/bin/env bash
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
# Fetch the pinned upstream Nintendont (FIX94, GPL-2.0) into third_party/Nintendont (git submodule, commit bbc0a20).
# The build never modifies it: build_wii.sh exports the pinned commit into a scratch tree and applies the patch series there.
set -euo pipefail
cd "$(dirname "$0")"
git submodule update --init third_party/Nintendont
[[ $(git -C third_party/Nintendont rev-parse HEAD) == bbc0a208e81d6ab0a828a431dc0b2ba6fc06aec5 ]] || { echo "third_party/Nintendont is not at the pinned commit"; exit 1; }
echo "Nintendont pinned at $(git -C third_party/Nintendont rev-parse --short HEAD)"
