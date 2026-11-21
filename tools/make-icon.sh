#!/bin/bash
# make-icon.sh - build a 48x48 menu icon from YOUR Applixware installation:
# starts it on a hidden Xvfb (with Openbox, see the launcher's notes), grabs
# the main window and crops the round logo with a transparent background.
#
# The icon is not shipped with applixware-wrapper because the artwork belongs
# to VistaSource; this script only derives it locally from your own copy.
#
# Needs: Xvfb, openbox, xdotool, ImageMagick (magick or convert), and a
# working `applixware` (licensed or not: the main window opens either way).
# Applixware must not be running (one session at a time).
#
# Usage: make-icon.sh [output.png]
#   default: ~/.local/share/icons/hicolor/48x48/apps/applixware.png
#
# Part of applixware-wrapper. SPDX-License-Identifier: GPL-3.0-or-later

set -u

out=${1:-${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/48x48/apps/applixware.png}
im=$(command -v magick || command -v convert) || { echo "ImageMagick not found" >&2; exit 1; }
for t in Xvfb openbox xdotool applixware; do
    command -v $t >/dev/null || { echo "$t not found" >&2; exit 1; }
done
if [ "$(podman container inspect -f '{{.State.Running}}' applixware 2>/dev/null)" = true ]; then
    echo "Applixware is running; close it first." >&2
    exit 1
fi

tmp=$(mktemp -d)
n=90
while [ -e /tmp/.X11-unix/X$n ] || [ -e /tmp/.X$n-lock ]; do n=$((n + 1)); done
Xvfb :$n -screen 0 1280x900x24 -nolisten tcp -ac >/dev/null 2>&1 &
xvfb=$!
for _ in $(seq 50); do [ -e /tmp/.X11-unix/X$n ] && break; sleep 0.1; done
DISPLAY=:$n openbox >/dev/null 2>&1 &
wm=$!
sleep 2

cleanup() {
    podman kill applixware >/dev/null 2>&1
    kill $wm $xvfb 2>/dev/null
}
trap cleanup EXIT

(cd "$HOME" && DISPLAY=:$n applixware)
win=
for _ in $(seq 60); do
    win=$(DISPLAY=:$n xdotool search --name '^Applixware' 2>/dev/null | tail -1)
    [ -n "$win" ] && break
    sleep 1
done
[ -n "$win" ] || { echo "Applixware main window did not show up" >&2; exit 1; }
sleep 3
DISPLAY=:$n import -window "$win" "$tmp/panel.png" 2>/dev/null ||
    { echo "screenshot failed (ImageMagick 'import')" >&2; exit 1; }

# Find the round logo by its yellow fill in the upper-left part of the main
# window, then cut a 44x44 circle centred on it.
bbox=$("$im" "$tmp/panel.png" -crop 40%x50%+0+0 +repage \
    -fuzz 25% -fill black +opaque '#ffff00' -format '%@' info:)
read -r w h x y < <(echo "$bbox" | sed 's/[x+]/ /g')
[ "${w:-0}" -ge 20 ] 2>/dev/null || { echo "logo not found in screenshot ($bbox)" >&2; exit 1; }
ox=$(( x + w / 2 - 22 )); oy=$(( y + h / 2 - 22 ))
mkdir -p "$(dirname "$out")"
"$im" "$tmp/panel.png" -crop 44x44+$ox+$oy +repage \
    \( -size 44x44 xc:black -fill white -draw "circle 21.5,21.5 21.5,1.0" \) \
    -alpha off -compose CopyOpacity -composite \
    -compose Over -background none -gravity center -extent 48x48 "$out"
mv "$tmp" "$tmp~" 2>/dev/null
command -v gtk-update-icon-cache >/dev/null &&
    gtk-update-icon-cache -f -t "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor" 2>/dev/null
echo "icon written to $out"
