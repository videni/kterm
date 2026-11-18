#!/bin/sh
# Name: Terminal 终端
# Author: kterm
# Package: kterm-vte
# Icon: data:image/png;base64,@ICON@
# DontUseFBInk

# Library icon: terminal with a shell inside the Alpine chroot.
# To close: type "exit", or press 符 and then 退出 on the keyboard.
exec /var/local/kmc/bin/kpm launch kterm-vte > /tmp/kterm.log 2>&1
