#!/bin/sh
# Start Mosh for a saved SSH host. Keep SSH available for kterm's queries.
set -eu

host=$1
session=$2
bootstrap=$3
config=$(ssh -G -- "$host")
remote_command=$(printf '%s\n' "$config" | sed -n 's/^remotecommand //p')

# The remote PTY belongs to mosh-server. Record it so a separate SSH
# channel can identify exactly this tmux client, even after roaming.
remote_shell='
    directory=$HOME/.cache/kterm
    marker=$directory/mosh-$1
    shift
    if [ -t 0 ] && (umask 077; mkdir -p "$directory" && tty > "$marker"); then
        trap '\''rm -f "$marker"'\'' EXIT
        trap '\''exit 129'\'' HUP
        trap '\''exit 143'\'' TERM
    fi
    shell=${SHELL:-/bin/sh}
    if [ -n "$1" ] && [ "$1" != none ]; then
        "$shell" -c "$1"
    else
        "$shell" -l
    fi
'

export KTERM_MOSH_HOST="$host" KTERM_MOSH_SESSION="$session"
# The SSH server reports the UDP destination. This also works when the SSH
# alias uses kterm-mdns as its ProxyCommand.
exec mosh --experimental-remote-ip=remote --no-ssh-pty \
    --ssh="$bootstrap -o RemoteCommand=none -o RequestTTY=no" \
    --server='PATH="$PATH:/opt/homebrew/bin:/usr/local/bin" mosh-server' \
    -- "$host" sh -c "$remote_shell" kterm-mosh "$session" "$remote_command"
