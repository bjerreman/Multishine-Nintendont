#!/usr/bin/env bash
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
set -euo pipefail
cd "$(dirname "$0")"
python3.14 tools/lint_repo.py --licence "GPL-3.0-or-later OR GPL-2.0-only,GPL-2.0-only,GPL-2.0-or-later,GPL-3.0-or-later,MIT,Zlib,CC0-1.0" "$@"
