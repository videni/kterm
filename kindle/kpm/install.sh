#!/bin/sh
# Install kterm in the existing Alpine environment, or prepare its runtime.
set -eu
PACKAGE_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ALPINE=/mnt/us/alpine/alpine.sh
DOCUMENTS=/mnt/us/documents
. "$PACKAGE_ROOT/runtime.sh"

install_in_alpine() {
    sh "$ALPINE" -c '
        for pkg in gtk+3.0 vte3; do
            apk info -e "$pkg" >/dev/null 2>&1 || apk add "$pkg" || exit 1
        done
    ' || return
    MNT=$(cd /tmp && pwd -P)/alpine
    install_terminal
}
if [ ! -r "$ALPINE" ] || ! install_in_alpine; then
    ensure_runtime_image
fi

mkdir -p "$DOCUMENTS"
install_file "$PACKAGE_ROOT/scriptlets/kterm.sh" "$DOCUMENTS/kterm.sh" 755
sync
