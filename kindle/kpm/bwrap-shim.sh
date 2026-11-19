#!/bin/sh
# Stand-in for bubblewrap on Kindle.
#
# gdk-pixbuf loads images through glycin, which runs every loader inside a
# bubblewrap sandbox. The Kindle kernel (4.1, no seccomp) cannot create that
# sandbox, so every image load fails and GTK aborts. This drops the sandbox
# options and runs the loader directly. Besides our own key images and
# GTK's icons, kterm shows images a tap asks for (imageview.c); their
# loaders (glycin-image-rs) are written in Rust.
while [ $# -gt 0 ]; do
    case "$1" in
        --setenv)
            case "$3" in /tmp-*) ;; *) export "$2=$3" ;; esac
            shift 3 ;;
        --ro-bind | --ro-bind-try | --bind | --bind-try | --dev-bind | --dev-bind-try | --symlink)
            shift 3 ;;
        --chdir | --dev | --tmpfs | --proc | --dir | --seccomp | --unsetenv | --cap-drop | --cap-add | --hostname | --uid | --gid)
            shift 2 ;;
        --*) shift ;;
        *) break ;;
    esac
done
exec "$@"
