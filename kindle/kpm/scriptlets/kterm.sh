#!/bin/sh
# Name: Terminal 终端
# Author: kterm
# Package: kterm-vte
# Icon: data:image/png;base64,@ICON@
# DontUseFBInk

# Library icon: terminal with a shell inside the Alpine chroot.
# To close: type "exit", or swipe up from the keyboard and tap 退出.
exec /var/local/kmc/bin/kpm launch kterm-vte > /tmp/kterm.log 2>&1
