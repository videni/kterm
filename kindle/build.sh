#!/bin/sh
# Build kterm for Kindle against Alpine's GTK+ 3 and VTE (armv7, musl).
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
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build --platform linux/arm/v7 \
        --build-arg APK_MIRROR="${APK_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}" \
        -t "$IMAGE" "$KINDLE_DIR/docker"
fi
docker run --rm --platform linux/arm/v7 \
    -v "$REPO:/src:ro" -v "$BUILD:/out" "$IMAGE" sh -c '
set -e
cd /out/obj
# tar keeps modification times: files that did not change stay older than
# their object files
tar -C /src --exclude=.git --exclude=tests --exclude=./build --exclude=./kindle -cf - . | tar -xf -
if [ ! -f Makefile ] || [ configure.ac -nt Makefile ] || [ Makefile.am -nt Makefile ]; then
    (sh autogen.sh || autoreconf -fi) >/tmp/ag.log 2>&1 || { tail -20 /tmp/ag.log; exit 1; }
    ./configure --enable-gtk3 --enable-kindle >/tmp/cf.log 2>&1 || { tail -25 /tmp/cf.log; exit 1; }
fi
make >/tmp/mk.log 2>&1 || { grep -B2 -A8 "error" /tmp/mk.log | head -80; exit 1; }
# deprecation warnings come with upstream'"'"'s GTK+ 2 compatible code
grep "warning:" /tmp/mk.log | grep -v "deprecated" | head -40 || true
cp kterm /out/kterm && echo BUILD_OK
'
