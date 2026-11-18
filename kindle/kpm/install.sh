#!/bin/sh
# Install kterm into the Alpine image and add its library icon.
set -eu
PACKAGE_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ALPINE=/mnt/us/alpine/alpine.sh
DOCUMENTS=/mnt/us/documents

if [ ! -f "$ALPINE" ]; then
    echo "kterm runs inside the Alpine chroot, which is not installed ($ALPINE missing)." >&2
    exit 1
fi
# Mounts the image if it is not mounted yet.
sh "$ALPINE" -c true
MNT=$(cd /tmp && pwd -P)/alpine

# GTK+ 3 and VTE come from Alpine's own packages (about 60 MB, needs Wi-Fi
# the first time).
for pkg in gtk+3.0 vte3; do
    if ! sh "$ALPINE" -c "apk info -e $pkg >/dev/null 2>&1"; then
        sh "$ALPINE" -c "apk add $pkg"
    fi
done

install_file() {
    # Copy next to the target and rename, so that a running kterm keeps
    # the old file instead of having it overwritten under it.
    mkdir -p "$(dirname "$2")"
    cp "$1" "$2.new"
    chmod "$3" "$2.new"
    mv "$2.new" "$2"
}
# kterm's own VTE (Alpine's with a few changes), found by kterm before the
# system's; the vte3 package above brings the libraries it needs
install_file "$PACKAGE_ROOT/payload/libvte-2.91.so.0" "$MNT/usr/local/lib/kterm/libvte-2.91.so.0" 755
install_file "$PACKAGE_ROOT/payload/kterm" "$MNT/usr/local/bin/kterm" 755
install_file "$PACKAGE_ROOT/bwrap-shim.sh" "$MNT/usr/local/bin/bwrap" 755
install_file "$PACKAGE_ROOT/payload/keyboard-kindle.xml" "$MNT/usr/local/share/kterm/keyboard-kindle.xml" 644
# The shortcut bar's buttons: a starting point to edit, never replaced.
if [ ! -e "$MNT/usr/local/etc/kterm/shortcuts.conf" ]; then
    install_file "$PACKAGE_ROOT/payload/shortcuts.conf" "$MNT/usr/local/etc/kterm/shortcuts.conf" 644
fi

mkdir -p "$DOCUMENTS"
install_file "$PACKAGE_ROOT/scriptlets/kterm.sh" "$DOCUMENTS/kterm.sh" 755
sync
