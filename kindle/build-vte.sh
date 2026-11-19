#!/bin/sh
# Build kterm's own VTE: Alpine's (the version its vte3 package has, with
# its patch), with the patches in kindle/vte/ on top. GTK+ 3 only.
# Output: $BUILD/vte, installed there as under / (usr/lib, usr/include, ...)
#
# Slow under emulation (half an hour or more): done again only when the
# version or a patch changes. Remove $BUILD/vte to build it anew.
set -e
KINDLE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(dirname "$KINDLE_DIR")
BUILD="${KTERM_BUILD:-$REPO/build}"
VERSION=0.84.1
SHA256=aca1caa8478aebcdbb1d67897fb3511eb7601debae6810e16a15b6fa25f31ac8
TARBALL="vte-$VERSION.tar.xz"
IMAGE=kterm-vte-build:armv7

STAMP=$( { echo "$VERSION"; cat "$KINDLE_DIR"/vte/*.patch "$KINDLE_DIR/build-vte.sh"; } | shasum -a 256 | cut -d' ' -f1)
if [ -f "$BUILD/vte/stamp" ] && [ "$(cat "$BUILD/vte/stamp")" = "$STAMP" ]; then
    echo "VTE up to date"
    exit 0
fi

mkdir -p "$BUILD/vte-src"
if [ ! -f "$BUILD/vte-src/$TARBALL" ]; then
    curl -fL -o "$BUILD/vte-src/$TARBALL.part" \
        "https://download.gnome.org/sources/vte/${VERSION%.*}/$TARBALL"
    mv "$BUILD/vte-src/$TARBALL.part" "$BUILD/vte-src/$TARBALL"
fi
echo "$SHA256  $BUILD/vte-src/$TARBALL" | shasum -a 256 -c -

if ! docker image inspect kterm-build:armv7 >/dev/null 2>&1; then
    echo "kterm-build:armv7 missing: run kindle/build.sh first" >&2
    exit 1
fi
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build --platform linux/arm/v7 -t "$IMAGE" \
        -f "$KINDLE_DIR/docker/Dockerfile.vte" "$KINDLE_DIR/docker"
fi

rm -rf "$BUILD/vte"
docker run --rm --platform linux/arm/v7 \
    -v "$KINDLE_DIR/vte:/patches:ro" -v "$BUILD/vte-src:/tarball:ro" -v "$BUILD:/out" \
    "$IMAGE" sh -c '
set -e
cd /tmp
tar -xf /tarball/'"$TARBALL"'
cd vte-'"$VERSION"'
for p in /patches/*.patch; do
    echo "patch: $(basename "$p")"
    patch -p1 < "$p"
done
# the options of Alpine'"'"'s package, without GTK 4, introspection and docs
meson setup _build --prefix=/usr --buildtype=release -Doptimization=2 \
    -Dgtk3=true -Dgtk4=false -Dgir=false -Dvapi=false -Ddocs=false \
    -D_systemd=false -Dapp=false -Dglade=false >/tmp/setup.log 2>&1 \
    || { tail -40 /tmp/setup.log; exit 1; }
meson compile -C _build >/tmp/compile.log 2>&1 || { grep -B2 -A10 "error" /tmp/compile.log | head -60; exit 1; }
DESTDIR=/out/vte meson install -C _build --no-rebuild >/tmp/install.log 2>&1 || { tail -20 /tmp/install.log; exit 1; }
'
echo "$STAMP" > "$BUILD/vte/stamp"
ls -la "$BUILD/vte/usr/lib/"libvte*
