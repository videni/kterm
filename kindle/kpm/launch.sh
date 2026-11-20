#!/bin/sh
# Start kterm in the installed Alpine environment or its private runtime.
#
#   kpm launch kterm-vte                   terminal with an Alpine login shell
#   sh launch.sh -e 'cmd'                 run cmd instead (other kterm options work too)
#
# Bind the Kindle's X server, D-Bus and fonts into the selected environment.

# One terminal only: started again (from the library, kpm launch) while
# one runs, that one is brought to the front instead (SIGUSR1, kterm.c). A
# second would not get the phone page's port (voice.c), and the Kindle has
# little memory to spare.
if pids=$(pidof kterm); then
    kill -USR1 $pids
    exit 0
fi

PACKAGE_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUNTIME_PARENT=$(cd /tmp && pwd -P)
if [ "${KTERM_PRIVATE_RUNTIME:-}" = 1 ]; then
    . "$PACKAGE_ROOT/runtime.sh"
    MNT=$RUNTIME_PARENT/kterm-vte
    if ! mount_runtime; then
        echo "kterm could not start its private runtime." >&2
        exit 1
    fi
    TERMINAL_FONT="DejaVu Sans Mono"
else
    ALPINE=/mnt/us/alpine/alpine.sh
    MNT=$RUNTIME_PARENT/alpine
    if [ ! -r "$ALPINE" ] || ! sh "$ALPINE" -c true ||
        [ ! -x "$MNT/usr/local/bin/kterm" ] ||
        [ ! -r "$MNT/usr/local/share/kterm/keyboard-kindle.xml" ] ||
        ! cmp -s "$PACKAGE_ROOT/payload/kterm" "$MNT/usr/local/bin/kterm" ||
        ! cmp -s "$PACKAGE_ROOT/payload/libvte-2.91.so.0" "$MNT/usr/local/lib/kterm/libvte-2.91.so.0"; then
        exec env KTERM_PRIVATE_RUNTIME=1 unshare -m --propagation private \
            /bin/sh "$PACKAGE_ROOT/launch.sh" "$@"
    fi
    TERMINAL_FONT="FiraCode Nerd Font Mono"
fi

bind() {
    grep -q " $2 " /proc/mounts && return
    mkdir -p "$2"
    mount -o bind "$1" "$2"
}
bind /var/tmp/.X11-unix "$MNT/tmp/.X11-unix"

# The machines of the local network are found by multicast DNS (kterm-mdns):
# their answers come from port 5353, which the Kindle's firewall drops like
# everything it did not ask for by a connection. Let them in on Wi-Fi.
iptables -C INPUT -i wlan0 -p udp --sport 5353 -j ACCEPT 2>/dev/null \
    || iptables -A INPUT -i wlan0 -p udp --sport 5353 -j ACCEPT 2>/dev/null
# The page a phone types into (voice.c) is served on port 7788.
iptables -C INPUT -i wlan0 -p tcp --dport 7788 -j ACCEPT 2>/dev/null \
    || iptables -A INPUT -i wlan0 -p tcp --dport 7788 -j ACCEPT 2>/dev/null
bind /var/run/dbus "$MNT/run/dbus"
bind /usr/java/lib/fonts "$MNT/usr/share/fonts/kindle"
# Fonts the user dropped into the Kindle's own .fonts folder.
[ -e "$MNT/usr/share/fonts/us" ] || [ -L "$MNT/usr/share/fonts/us" ] ||
    ln -s /mnt/us/.fonts "$MNT/usr/share/fonts/us"

# Services the user wants up whenever the terminal is: whatever has been
# added to OpenRC's default runlevel inside the image (rc-update add <name>
# default). Nothing boots the image, so the runlevel is entered here - in the
# background, a slow service must not delay the terminal. Entering it again
# while everything is up changes nothing. Started from a subshell, so it is
# not a child of kterm (this shell becomes kterm), which would have to reap it.
if [ "${KTERM_PRIVATE_RUNTIME:-}" != 1 ] && [ -x "$MNT/sbin/openrc" ]; then
    (setsid chroot "$MNT" /usr/bin/env -i HOME=/root \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        /sbin/openrc default > /dev/null 2>&1 < /dev/null &)
fi

# The terminal runs as Alpine's root account. Read its configured login shell.
SHELL_BIN=$(awk -F: '$1 == "root" { print $7; exit }' "$MNT/etc/passwd")
SHELL_BIN=${SHELL_BIN:-/bin/sh}
case " $* " in
    *" -e "*) ;;
    *) set -- "$@" -e "$SHELL_BIN -l" ;;
esac

# env -i: nothing from the Kindle's glibc environment may leak into musl.
# GDK_CORE_DEVICE_EVENTS: the X server's XInput is too old for GTK+ 3.
exec chroot "$MNT" /usr/bin/env -i \
    HOME=/root \
    DISPLAY="${DISPLAY:-:0}" \
    TERM=xterm-256color \
    LANG=en_US.UTF-8 \
    SHELL="$SHELL_BIN" \
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket \
    GDK_CORE_DEVICE_EVENTS=1 \
    NO_AT_BRIDGE=1 \
    ${KTERM_IME_TRACE:+KTERM_IME_TRACE=1} \
    kterm -l /usr/local/share/kterm/keyboard-kindle.xml \
        -f "$TERMINAL_FONT" "$@"
