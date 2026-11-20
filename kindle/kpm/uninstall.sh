#!/bin/sh
# Remove kterm's files. The Alpine image and the GTK+/VTE packages inside it
# are left alone: other things in the image may use them.
# The private runtime image in /mnt/us/kterm-vte holds the terminal user's data.
set -eu
PACKAGE_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ALPINE=/mnt/us/alpine/alpine.sh
ENTRY=/mnt/us/documents/kterm.sh

if [ -f "$ENTRY" ] && cmp -s "$ENTRY" "$PACKAGE_ROOT/scriptlets/kterm.sh"; then
    rm "$ENTRY"
fi
if [ -f "$ALPINE" ] && sh "$ALPINE" -c true; then
    MNT=$(cd /tmp && pwd -P)/alpine
    rm -f "$MNT/usr/local/bin/kterm" "$MNT/usr/local/bin/kterm-mdns" "$MNT/usr/local/bin/bwrap"
    rm -f "$MNT/usr/local/bin/kterm-mosh"
    rm -rf "$MNT/usr/local/share/kterm" "$MNT/usr/local/lib/kterm"
fi
