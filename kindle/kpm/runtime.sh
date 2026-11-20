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

bundled_loop_device() {
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

mount_bundled_runtime() (
    exec 9>"$MNT.lock" || return
    flock -x 9 || return
    state=/mnt/us/kterm-vte
    image=$state/runtime.ext3
    if [ ! -s "$image" ]; then
        mkdir -p "$state" || return
        image_tmp=$state/.runtime.ext3.new
        [ ! -L "$image_tmp" ] || return 1
        trap 'rm -f "$image_tmp"' EXIT
        gzip -dc "$PACKAGE_ROOT/payload/runtime.ext3.gz" > "$image_tmp" &&
            [ -s "$image_tmp" ] && mv -f "$image_tmp" "$image" || return
    fi
    # Prefer the real FAT filesystem over Kindle's FUSE view for loop devices.
    if [ -s /mnt/base-us/kterm-vte/runtime.ext3 ]; then
        image=/mnt/base-us/kterm-vte/runtime.ext3
    fi

    mkdir -p "$MNT" || return
    # A detached tmux session can still hold the previous mount namespace.
    # Share its loop device so only one superblock writes to the image.
    device=$(bundled_loop_device "$image")
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
