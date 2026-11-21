<!-- Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only -->
# Multishine Wii loader

Source of the Multishine loader for Wii and Wii U (vWii): a modified build of FIX94's Nintendont that installs and updates
the Multishine card files and then starts Melee.

Players: get Multishine at https://multishine.link/get

## Build

Prerequisites: the Wii toolchain in `DKP_R35` (devkitPPC r35 with libogc 1.8.23, devkitARM r52), a current devkitPPC with libogc in `DEVKITPRO` (default `/opt/devkitpro`), `python3.14`, a host C compiler.

```sh
./prepare.sh  # fetch the pinned Nintendont into third_party/Nintendont
./build.sh    # build/wii/shell.dol, kernel.zip, stub/boot.dol, test-signed update containers
./test.sh     # host tests and lint
```

## Licence

GPL-2.0-only as built (`LICENSE`); our own files marked `GPL-3.0-or-later OR GPL-2.0-only` may also be used under GPL-3.0-or-later.
Third-party notices: `NOTICE`. Credits: `CREDITS.md`.
