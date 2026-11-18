#!/bin/sh
# Start the GTK+ 3 / VTE kterm that lives in the Alpine image.
#
#   kpm launch kterm-vte                   terminal with an Alpine login shell
#   sh launch.sh -e 'cmd'                 run cmd instead (other kterm options work too)
#
# The X server, D-Bus and the Kindle fonts live outside the image, so they
# are bind-mounted in first. "alpine.sh stop" unmounts them again.

MNT=$(cd /tmp && pwd -P)/alpine
sh /mnt/us/alpine/alpine.sh -c true || exit 1

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
bind /var/run/dbus "$MNT/run/dbus"
bind /usr/java/lib/fonts "$MNT/usr/share/fonts/kindle"
# Fonts the user dropped into the Kindle's own .fonts folder.
[ -e "$MNT/usr/share/fonts/us" ] || ln -s /mnt/us/.fonts "$MNT/usr/share/fonts/us"

# Services the user wants up whenever the terminal is: whatever has been
# added to OpenRC's default runlevel inside the image (rc-update add <name>
# default). Nothing boots the image, so the runlevel is entered here - in the
# background, a slow service must not delay the terminal. Entering it again
# while everything is up changes nothing.
if [ -x "$MNT/sbin/openrc" ]; then
    setsid chroot "$MNT" /usr/bin/env -i HOME=/root \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        /sbin/openrc default > /dev/null 2>&1 < /dev/null &
fi

# Default command: a login shell, so /etc/profile sets the prompt and PATH.
# bash if it has been installed in the image (apk add bash), else busybox ash.
SHELL_BIN=/bin/sh
[ -x "$MNT/bin/bash" ] && SHELL_BIN=/bin/bash
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
        -f "FiraCode Nerd Font Mono" "$@"
