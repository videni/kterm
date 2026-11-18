#!/bin/sh
# Try kterm's user interface without the Kindle: the last build runs under a
# virtual X server of the Kindle's size in Docker, driven with xdotool.
#
#   uitest.sh start            start X and kterm (fresh home directory)
#   uitest.sh tap X Y [X Y..]  touch one point after another
#   uitest.sh kswipe X1 Y1 X2 Y2  swipe with the event sequence of the real Kindle
#   uitest.sh klong X1 Y1 X2 Y2   long press at X1 Y1, then drag (selects text)
#   uitest.sh ktwo X Y         two finger tap
#   uitest.sh swipe X1 Y1 X2 Y2
#   uitest.sh shot NAME        screenshot to build/shots/NAME.png
#   uitest.sh sh 'command'     run a command in the container
#   uitest.sh stop
#
# Not covered here: the Kindle's 8-bit screen, its window manager and its
# system services (keyboard, quick settings).
set -e
KINDLE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(dirname "$KINDLE_DIR")
BUILD="${KTERM_BUILD:-$REPO/build}"
NAME=kterm-ui
X="docker exec -e DISPLAY=:1 $NAME"
case "$1" in
start)
    if ! docker image inspect kterm-test:armv7 >/dev/null 2>&1; then
        # needs kterm-build:armv7, made by build.sh
        docker build --platform linux/arm/v7 -t kterm-test:armv7 \
            -f "$KINDLE_DIR/docker/Dockerfile.test" "$KINDLE_DIR/docker"
    fi
    docker rm -f $NAME >/dev/null 2>&1 || true
    docker run -d --name $NAME --platform linux/arm/v7 \
        -v "$BUILD:/out:ro" -v "$REPO/layouts:/layouts:ro" \
        -v "$KINDLE_DIR/kpm/bwrap-shim.sh:/usr/local/bin/bwrap:ro" \
        kterm-test:armv7 sleep 86400 >/dev/null
    docker exec -d $NAME Xvfb :1 -screen 0 1072x1448x24 -dpi 300 -nolisten tcp
    sleep 3
    # What runs in the terminal: UITEST_CMD (a shell script text), a plain
    # shell by default.
    printf '%s\n' "${UITEST_CMD:-exec /bin/bash --norc}" | docker exec -i $NAME sh -c 'cat > /tmp/uitest-cmd.sh'
    docker exec -d -e DISPLAY=:1 -e HOME=/root -e GDK_CORE_DEVICE_EVENTS=1 -e NO_AT_BRIDGE=1 \
        -e LANG=en_US.UTF-8 ${UITEST_ENV:+-e $UITEST_ENV} $NAME sh -c \
        '/out/kterm -d -l /layouts/keyboard-kindle.xml -f "DejaVu Sans Mono" -s 8 -e "/bin/sh /tmp/uitest-cmd.sh" > /tmp/kterm.log 2>&1'
    sleep 12
    # no window manager here to maximize the window, as the Kindle's does
    W=$($X xdotool search --name "L:A_N:application" | tail -1)
    $X xdotool windowmove "$W" 0 0 windowsize "$W" 1072 1448
    sleep 4
    echo "window $W" ;;
tap)
    # any number of points: tap X Y [X Y ...]
    shift
    while [ $# -ge 2 ]; do
        $X xdotool mousemove "$1" "$2" mousedown 1 sleep 0.08 mouseup 1; sleep 1.2
        shift 2
    done
    [ $# -eq 0 ] || { echo "tap: odd number of coordinates" >&2; exit 2; } ;;
kswipe)
    # A swipe as the Kindle's touch driver reports it (recorded on the
    # device with "xinput test"): button 1 goes down, and once the finger
    # has moved far enough another button is clicked while button 1 is
    # still held: 3 going up, 4 (a wheel turn to X) going down.
    mx=$(( ($2 + $4) / 2 )); my=$(( ($3 + $5) / 2 ))
    mark=3; [ "$5" -gt "$3" ] && mark=4
    $X xdotool mousemove "$2" "$3" mousedown 1 sleep 0.05 mousemove "$mx" "$my" \
        click $mark sleep 0.05 mousemove "$4" "$5" sleep 0.05 mouseup 1
    sleep 1.5 ;;
klong)
    # held still for half a second the driver clicks button 9, then the
    # finger moves
    mx=$(( ($2 + $4) / 2 )); my=$(( ($3 + $5) / 2 ))
    $X xdotool mousemove "$2" "$3" mousedown 1 sleep 0.5 click 9 sleep 0.3 \
        mousemove "$mx" "$my" sleep 0.1 mousemove "$4" "$5" sleep 0.2 mouseup 1
    sleep 1.5 ;;
ktwo)
    # the second finger is button 2
    $X xdotool mousemove "$2" "$3" mousedown 1 sleep 0.03 click 2 sleep 0.05 mouseup 1
    sleep 1.5 ;;
swipe)
    $X xdotool mousemove "$2" "$3" mousedown 1 sleep 0.05 \
        mousemove $((($2 + $4) / 2)) $((($3 + $5) / 2)) sleep 0.05 \
        mousemove "$4" "$5" sleep 0.05 mouseup 1; sleep 1.5 ;;
shot)
    mkdir -p "$BUILD/shots"
    $X import -window root png:- > "$BUILD/shots/$2.png"
    echo "$BUILD/shots/$2.png" ;;
sh)
    shift; $X sh -c "$*" ;;
stop)
    docker rm -f $NAME >/dev/null 2>&1 || true ;;
*)
    sed -n 2,13p "$0" ;;
esac
