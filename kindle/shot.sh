#!/bin/sh
# Grab the Kindle framebuffer (portrait) and save it as PNG: shot.sh <name>
# KINDLE: ssh destination of the Kindle, root@kindle by default
KINDLE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(dirname "$KINDLE_DIR")
BUILD="${KTERM_BUILD:-$REPO/build}"
mkdir -p "$BUILD/shots"
out="$BUILD/shots/${1:-shot}.png"
ssh -o ConnectTimeout=8 -o BatchMode=yes "${KINDLE:-root@kindle}" 'dd if=/dev/fb0 bs=1088 count=1448 2>/dev/null' 2>/dev/null > /tmp/kfb.raw || exit 1
python3 -I - "$out" <<'PY'
import zlib, struct, sys
raw = open('/tmp/kfb.raw', 'rb').read()
W, H, S = 1072, 1448, 1088
rows = b''.join(b'\x00' + raw[y*S:y*S+W] for y in range(H))
def ch(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 0, 0, 0, 0)) + ch(b'IDAT', zlib.compress(rows, 6)) + ch(b'IEND', b''))
print(sys.argv[1])
PY
