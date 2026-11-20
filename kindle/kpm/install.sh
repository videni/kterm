#!/bin/sh
# Install kterm in an existing Alpine environment when available.
set -eu
PACKAGE_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ALPINE=/mnt/us/alpine/alpine.sh
DOCUMENTS=/mnt/us/documents
. "$PACKAGE_ROOT/runtime.sh"

if [ ! -s "$PACKAGE_ROOT/payload/runtime.ext3.gz" ]; then
    echo "kterm package is missing its bundled runtime." >&2
    exit 1
fi

install_in_alpine() {
    sh "$ALPINE" -c '
        for pkg in gtk+3.0 vte3; do
            apk info -e "$pkg" >/dev/null 2>&1 || apk add "$pkg" || exit 1
        done
    ' || return
    MNT=$(cd /tmp && pwd -P)/alpine
    install_terminal
}
if [ -r "$ALPINE" ] && ! install_in_alpine; then
    echo "kterm could not install into Alpine; the bundled runtime is still available." >&2
fi

mkdir -p "$DOCUMENTS"
install_file "$PACKAGE_ROOT/scriptlets/kterm.sh" "$DOCUMENTS/kterm.sh" 755
sync
