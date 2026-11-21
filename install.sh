#!/bin/bash
# install.sh - set up applixware-wrapper for the current user.
#
#   ./install.sh /path/to/applixware6-lnx-homeuser.tar.gz
#   ./install.sh            (re-run: updates scripts/configs, keeps Applixware)
#
# What it does (nothing outside $HOME, no root):
#   - untars Applixware into $AX_BASE/app (only if not there yet)
#   - builds the i386 Debian etch podman image (localhost/applixware6)
#   - installs the launcher (~/.local/bin/applixware), the Openbox config and
#     EMWM-Motif theme used by --xephyr, and a menu entry
#   - adds US-International dead keys to Applixware's table (tools/)
# Existing files that would be overwritten are kept with a '~' suffix.
#
# Then run `applixware --install` once to accept the Home User license and
# generate the license key, and optionally tools/make-icon.sh for the icon.
#
# Part of applixware-wrapper. SPDX-License-Identifier: GPL-3.0-or-later

set -eu

here=$(cd "$(dirname "$0")" && pwd)
AX_BASE=${AX_BASE:-$HOME/.local/opt/applixware-6}
BINDIR=${BINDIR:-$HOME/.local/bin}
DATA=${XDG_DATA_HOME:-$HOME/.local/share}
tarball=${1:-}

say() { printf '==> %s\n' "$*"; }
put() {  # put SRC DEST [MODE] - install, keeping a different old file as DEST~
    if [ -f "$2" ] && cmp -s "$1" "$2"; then
        chmod "${3:-644}" "$2"
        return
    fi
    install -D -m "${3:-644}" --backup=simple --suffix='~' "$1" "$2"
}

for t in podman python3 xauth xset; do
    command -v $t >/dev/null || { echo "missing required tool: $t" >&2; exit 1; }
done
for t in Xephyr openbox xprop xsetroot; do
    command -v $t >/dev/null || echo "note: $t not found (needed only for --xephyr)" >&2
done

# 1. Applixware itself
if [ -x "$AX_BASE/app/applix" ]; then
    say "Applixware already in $AX_BASE/app (left untouched)"
else
    [ -n "$tarball" ] || { echo "usage: $0 /path/to/applixware6-lnx-homeuser.tar.gz" >&2; exit 1; }
    [ -f "$tarball" ] || { echo "not found: $tarball" >&2; exit 1; }
    say "untarring $tarball into $AX_BASE/app"
    mkdir -p "$AX_BASE"
    tmp=$(mktemp -d "$AX_BASE/untar.XXXXXX")
    tar xzf "$tarball" -C "$tmp"
    top=$(find "$tmp" -mindepth 1 -maxdepth 1 -type d | head -1)
    [ -x "$top/applix" ] || { echo "unexpected tarball layout (no applix in $top)" >&2; exit 1; }
    mv "$top" "$AX_BASE/app"
    rmdir "$tmp"
fi

# 2. Container image
say "building podman image localhost/applixware6 (i386 Debian etch)"
podman build --arch 386 -t localhost/applixware6 "$here/container"

# 3. Launcher, Openbox config/theme, menu entry
say "installing launcher and configuration"
put "$here/bin/applixware" "$BINDIR/applixware" 755
put "$here/openbox/rc.xml" "$AX_BASE/openbox/rc.xml"
for f in "$here"/themes/EMWM-Motif/openbox-3/*; do
    put "$f" "$DATA/themes/EMWM-Motif/openbox-3/$(basename "$f")"
done
desk=$AX_BASE/applixware.desktop
sed "s|@BINDIR@|$BINDIR|g" "$here/desktop/applixware.desktop.in" >"$desk"
put "$desk" "$DATA/applications/applixware.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$DATA/applications" 2>/dev/null || true

# 4. Dead keys
"$here/tools/deadkeys-us-intl.sh" "$AX_BASE/app"

cat <<EOF

Done. Next steps:
  1. applixware --install     # once: accept the Home User license, key is generated
  2. applixware               # or the "Applixware" menu entry
  optional: $here/tools/make-icon.sh    # menu icon from your own copy
  optional: echo 'AX_MOUNTS=/data:/media/disk' >> ~/.config/applixware-wrapper.conf
EOF
case ":$PATH:" in *":$BINDIR:"*) ;; *) echo "note: $BINDIR is not in your PATH" ;; esac
