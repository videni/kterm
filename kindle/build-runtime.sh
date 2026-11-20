#!/bin/sh
# Build the private ARMv7 runtime used when no external Alpine is installed.
set -eu
KINDLE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(dirname "$KINDLE_DIR")
BUILD=${KTERM_BUILD:-$REPO/build}
OUTPUT=$BUILD/runtime.ext3.gz
TMP_OUTPUT=$(mktemp "$BUILD/runtime.ext3.gz.XXXXXX")
trap 'rm -f "$TMP_OUTPUT"' EXIT

docker run --rm --platform linux/arm/v7 \
    -e APK_MIRROR="${APK_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}" \
    alpine:3.24 sh -eu -c '
    sed -i "s|https://dl-cdn.alpinelinux.org/alpine|$APK_MIRROR|" /etc/apk/repositories
    apk add --no-cache e2fsprogs >&2
    root=$(mktemp -d)
    mkdir -p "$root/etc/apk"
    cp -a /etc/apk/keys /etc/apk/repositories "$root/etc/apk/"
    apk --root "$root" --initdb --no-cache add \
        alpine-base ca-certificates gtk+3.0 vte3 font-dejavu \
        openssh-client mosh tmux bash >&2

    mkdir -p "$root/dev/pts" "$root/proc" "$root/sys" "$root/mnt/us"
    touch "$root/etc/resolv.conf"

    # Kindle kernels need the older ext3 on-disk feature set.
    used=$(du -sk "$root" | cut -f1)
    size=$(( (used + 1023) / 1024 + 32 ))
    mke2fs -q -t ext3 -m 0 -O none,has_journal,ext_attr,dir_index,filetype,sparse_super,large_file \
        -d "$root" /tmp/kterm-runtime.ext3 "${size}M" >&2
    e2fsck -fn /tmp/kterm-runtime.ext3 >&2
    gzip -c /tmp/kterm-runtime.ext3
' > "$TMP_OUTPUT"

gzip -t "$TMP_OUTPUT"
mv "$TMP_OUTPUT" "$OUTPUT"
printf '%s\n' "$OUTPUT"
