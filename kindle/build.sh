#!/bin/sh
# Build kterm for Kindle against Alpine's GTK+ 3 and kterm's own VTE
# (armv7, musl): Alpine's VTE with the patches in kindle/vte/, built by
# kindle/build-vte.sh into $BUILD/vte. kterm finds it at run time in
# /usr/local/lib/kterm (where the package installs it), before the system's.
# Output: build/kterm in the repository (KTERM_BUILD to change)
#
# The compiler and libraries come from a Docker image built once from
# kindle/docker/Dockerfile; set APK_MIRROR to an Alpine mirror near you for
# that first build. Object files are kept in $BUILD/obj between runs, so
# only changed sources are recompiled. Remove it for a clean build.
set -e
KINDLE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(dirname "$KINDLE_DIR")
BUILD="${KTERM_BUILD:-$REPO/build}"
IMAGE=kterm-build:armv7
mkdir -p "$BUILD/obj"
if [ "$(docker image inspect --format '{{index .Config.Labels "kterm.build.rsvg-convert"}}' "$IMAGE" 2>/dev/null)" != 1 ]; then
    docker build --platform linux/arm/v7 \
        --build-arg APK_MIRROR="${APK_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}" \
        -t "$IMAGE" "$KINDLE_DIR/docker"
fi
sh "$KINDLE_DIR/build-vte.sh"
docker run --rm --platform linux/arm/v7 \
    -v "$REPO:/src:ro" -v "$BUILD:/out" "$IMAGE" sh -c '
set -e
# kterm'"'"'s VTE in place of the system'"'"'s, in this throwaway container
cp -a /out/vte/usr/. /usr/
cd /out/obj
# tar keeps modification times: files that did not change stay older than
# their object files
tar -C /src --exclude=.git --exclude=tests --exclude=./build --exclude=./kindle -cf - . | tar -xf -
if [ ! -f Makefile ] || [ configure.ac -nt Makefile ] || [ Makefile.am -nt Makefile ] \
    || [ /src/kindle/build.sh -nt Makefile ]; then
    (sh autogen.sh || autoreconf -fi) >/tmp/ag.log 2>&1 || { tail -20 /tmp/ag.log; exit 1; }
    ./configure --enable-gtk3 --enable-kindle LDFLAGS="-Wl,-rpath,/usr/local/lib/kterm" >/tmp/cf.log 2>&1 || { tail -25 /tmp/cf.log; exit 1; }
fi
make >/tmp/mk.log 2>&1 || { grep -B2 -A8 "error" /tmp/mk.log | head -80; exit 1; }
# deprecation warnings come with upstream'"'"'s GTK+ 2 compatible code
grep "warning:" /tmp/mk.log | grep -v "deprecated" | head -40 || true
cp kterm /out/kterm && cp kterm-mdns /out/kterm-mdns && echo BUILD_OK
'
