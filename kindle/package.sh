#!/bin/sh
# Assemble the KPM package from the last build.
# Output: build/kterm-vte_<version>_kindlehf.kpkg in the repository
set -e
KINDLE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(dirname "$KINDLE_DIR")
BUILD="${KTERM_BUILD:-$REPO/build}"
PKG="$KINDLE_DIR/kpm"
VERSION=$(python3 -I -c 'import json,sys; print(".".join(map(str, json.load(open(sys.argv[1]))["version"])))' "$PKG/manifest.json")
OUT="$BUILD/kterm-vte_${VERSION}_kindlehf.kpkg"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

cp "$PKG/manifest.json" "$PKG/install.sh" "$PKG/uninstall.sh" "$PKG/launch.sh" \
   "$PKG/bwrap-shim.sh" "$PKG/SOURCE.txt" "$STAGE/"
mkdir -p "$STAGE/scriptlets" "$STAGE/payload"
# The library cover travels inside the scriptlet, as a data: URL.
rsvg-convert -w 600 -h 900 "$PKG/icon.svg" -o "$STAGE/icon.png"
python3 -I -c 'import base64, sys
src, icon, dst = sys.argv[1:]
text = open(src, encoding="utf-8").read()
assert "@ICON@" in text
open(dst, "w", encoding="utf-8").write(text.replace("@ICON@", base64.b64encode(open(icon, "rb").read()).decode()))' \
    "$PKG/scriptlets/kterm.sh" "$STAGE/icon.png" "$STAGE/scriptlets/kterm.sh"
rm "$STAGE/icon.png"
cp "$BUILD/kterm" "$REPO/layouts/keyboard-kindle.xml" "$REPO/shortcuts.conf" "$STAGE/payload/"
# kterm's own VTE (kindle/build-vte.sh), under its soname
cp -L "$BUILD/vte/usr/lib/libvte-2.91.so.0" "$STAGE/payload/"
# GPL: the source this binary was built from travels with it.
tar -czf "$STAGE/source.tar.gz" -C "$(dirname "$REPO")" --exclude .git --exclude '*.o' \
    --exclude "$(basename "$REPO")/build" "$(basename "$REPO")"
# COPYFILE_DISABLE: no macOS ._ resource files in the archive
# entries without a leading "./", like the packages in the official repo
(cd "$STAGE" && COPYFILE_DISABLE=1 tar -czf "$OUT" *)
echo "$OUT"
tar -tzf "$OUT"
