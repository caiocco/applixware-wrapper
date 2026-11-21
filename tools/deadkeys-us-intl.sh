#!/bin/bash
# deadkeys-us-intl.sh - add US-International dead-key sequences to Applixware's
# own dead-key table (axdata/deadkeys.dat), keeping it sorted as required:
#
#   ' + c / ' + C   -> ç / Ç   (the usual Brazilian/Portuguese convention)
#   dead key + space -> the accent itself (' " ` ^ ~)
#
# Applixware composes dead keys itself from that table (it does not use XIM),
# so this is needed even when the desktop composes them fine.
# The original file is kept as deadkeys.dat.orig~ (only on the first run).
#
# Usage: deadkeys-us-intl.sh [AX_INST]   (default ~/.local/opt/applixware-6/app)
#
# Part of applixware-wrapper. SPDX-License-Identifier: GPL-3.0-or-later

set -eu

inst=${1:-${AX_INST:-$HOME/.local/opt/applixware-6/app}}
table=$inst/axdata/deadkeys.dat
[ -f "$table" ] || { echo "not found: $table" >&2; exit 1; }
[ -e "$table.orig~" ] || cp -p "$table" "$table.orig~"

python3 -I - "$table" <<'EOF'
import sys
path = sys.argv[1]
lines = open(path, encoding='latin-1').read().split('\n')
add = {
    (0xFE50, 0x20): (0x60, "Grave + space -> grave (us-intl)"),
    (0xFE51, 0x20): (0x27, "Acute + space -> apostrophe (us-intl)"),
    (0xFE51, 0x43): (0xC7, "Acute + C -> C-cedilla (us-intl)"),
    (0xFE51, 0x63): (0xE7, "Acute + c -> c-cedilla (us-intl)"),
    (0xFE52, 0x20): (0x5E, "Circumflex + space -> circumflex (us-intl)"),
    (0xFE53, 0x20): (0x7E, "Tilde + space -> tilde (us-intl)"),
    (0xFE57, 0x20): (0x22, "Diaeresis + space -> quotedbl (us-intl)"),
}
first = next(i for i, l in enumerate(lines) if l.startswith('0x'))
maps = {}
for l in lines:
    if l.startswith('0x'):
        f = l.split()
        maps[(int(f[0], 16), int(f[1], 16))] = l
new = 0
for k, (v, c) in add.items():
    if k not in maps:
        maps[k] = "0x%04X    0x%02X    0x%02X        # %s" % (k[0], k[1], v, c)
        new += 1
tail = [l for l in lines[first:] if not l.startswith('0x')]
out = lines[:first] + [maps[k] for k in sorted(maps)] + tail
open(path, 'w', encoding='latin-1').write('\n'.join(out))
print("deadkeys.dat: %d mapping(s) added" % new)
EOF
