#!/bin/sh
# Shared by package install and launch. PACKAGE_ROOT is set by the caller.

install_file() {
    if [ -f "$2" ] && [ ! -L "$2" ] && cmp -s "$1" "$2"; then
        [ "$(stat -c %a "$2")" = "$3" ] || chmod "$3" "$2"
        return
    fi
    mkdir -p "$(dirname "$2")" || return
    pending=$(mktemp "$2.XXXXXX") || return
    if cp "$1" "$pending" && chmod "$3" "$pending" && mv -f "$pending" "$2"; then
        return 0
    fi
    rm -f "$pending"
    return 1
}

install_terminal() {
    install_file "$PACKAGE_ROOT/payload/kterm-mdns" "$MNT/usr/local/bin/kterm-mdns" 755 || return
    install_file "$PACKAGE_ROOT/kterm-mosh.sh" "$MNT/usr/local/bin/kterm-mosh" 755 || return
    install_file "$PACKAGE_ROOT/bwrap-shim.sh" "$MNT/usr/local/bin/bwrap" 755 || return
    install_file "$PACKAGE_ROOT/payload/keyboard-kindle.xml" "$MNT/usr/local/share/kterm/keyboard-kindle.xml" 644 || return
    if [ ! -e "$MNT/usr/local/etc/kterm/shortcuts.conf" ]; then
        install_file "$PACKAGE_ROOT/payload/shortcuts.conf" "$MNT/usr/local/etc/kterm/shortcuts.conf" 644 || return
    fi
    install_file "$PACKAGE_ROOT/payload/libvte-2.91.so.0" "$MNT/usr/local/lib/kterm/libvte-2.91.so.0" 755 || return
    install_file "$PACKAGE_ROOT/payload/kterm" "$MNT/usr/local/bin/kterm" 755
}

# Keep the expanded image between runs. A copy of the release asset in the
# same directory also allows the first install to work offline.
ensure_runtime_image() (
    state=/mnt/us/kterm-vte
    image=$state/runtime.ext3
    test -s "$image" && return 0

    temp=$(cd /tmp && pwd -P) || return
    exec 8>"$temp/kterm-vte-image.lock" || return
    flock -x 8 || return
    test -s "$image" && return 0
    mkdir -p "$state" || return
    stage=$(mktemp -d "$state/.runtime-install.XXXXXX") || return
    trap 'rm -rf "$stage"' EXIT

    asset=kterm-runtime_armv7.ext3.gz
    archive=$state/$asset
    if [ ! -r "$archive" ]; then
        archive=$stage/$asset
        if ! curl --fail --location --proto '=https' --proto-redir '=https' \
            --connect-timeout 15 --speed-limit 1024 --speed-time 45 --retry 2 \
            --output "$archive" "https://github.com/videni/kterm/releases/download/v3.10.1/$asset"; then
            echo "Could not download the kterm runtime. Copy $asset to $state and retry." >&2
            return 1
        fi
    fi
    digest=$(sha256sum "$archive") || return
    if [ "${digest%% *}" != db3c30db5dde74c0e5f0384463f630bc246750ec2ec833077f514f9025400c66 ]; then
        echo "The kterm runtime checksum does not match. Download $asset again." >&2
        return 1
    fi
    gzip -dc "$archive" > "$stage/runtime.ext3" &&
        test -s "$stage/runtime.ext3" &&
        mv -f "$stage/runtime.ext3" "$image"
)

runtime_loop_device() {
    for backing in /sys/block/loop*/loop/backing_file; do
        [ -r "$backing" ] || continue
        case $(cat "$backing") in
            "$1"|/kterm-vte/runtime.ext3) ;;
            *) continue ;;
        esac
        number=${backing#/sys/block/loop}
        number=${number%%/*}
        if [ -b "/dev/loop/$number" ]; then
            printf '/dev/loop/%s\n' "$number"
        else
            printf '/dev/loop%s\n' "$number"
        fi
        return
    done
}

mount_runtime() (
    exec 9>"$MNT.lock" || return
    flock -x 9 || return
    ensure_runtime_image || return
    state=/mnt/us/kterm-vte
    image=$state/runtime.ext3
    # Prefer the real FAT filesystem over Kindle's FUSE view for loop devices.
    if [ -s /mnt/base-us/kterm-vte/runtime.ext3 ]; then
        image=/mnt/base-us/kterm-vte/runtime.ext3
    fi

    mkdir -p "$MNT" || return
    # A detached tmux session can still hold the previous mount namespace.
    # Share its loop device so only one superblock writes to the image.
    device=$(runtime_loop_device "$image")
    if [ -n "$device" ]; then
        mount -o noatime -t ext3 "$device" "$MNT" || return
    else
        mount -o loop,noatime -t ext3 "$image" "$MNT" || return
        device=$(awk -v root="$MNT" '$2 == root { print $1; exit }' /proc/mounts)
        # On the Kindle kernel, detaching an in-use loop enables autoclear.
        case "$device" in /dev/loop*) losetup -d "$device" || return ;; *) return 1 ;; esac
    fi
    mount -o bind /dev "$MNT/dev" &&
    mount -o bind /dev/pts "$MNT/dev/pts" &&
    mount -t proc proc "$MNT/proc" &&
    mount -t sysfs sysfs "$MNT/sys" &&
    mount -o bind /mnt/us "$MNT/mnt/us" || return
    if [ -r /etc/resolv.conf ]; then
        install_file /etc/resolv.conf "$MNT/etc/resolv.conf" 644 || return
    fi
    install_terminal
)
