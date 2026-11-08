#!/bin/sh
# Boot an ISO headless, take a screenshot after N seconds, then quit.
# usage: tools/shot.sh <iso> <out.png> [delay-seconds] [serial.log]
# Also keeps a copy as <dir of out>/shots/build-<BUILD_NUMBER>[-n].png
ISO="$1"; OUT="$2"; DELAY="${3:-6}"; SERIAL="${4:-/dev/null}"
PPM="${OUT%.png}.ppm"
rm -f "$PPM" "$OUT"
( sleep "$DELAY"; echo "screendump $PPM"; sleep 1; echo "quit" ) | \
  timeout $((DELAY + 20)) qemu-system-x86_64 -machine pc -m 2048 -enable-kvm -cpu host -vga std \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    ${TEST_DISK:+-drive file=$TEST_DISK,format=raw,if=ide,index=0} \
    -cdrom "$ISO" -boot ${BOOT_ORDER:-d} -display none -monitor stdio -serial file:"$SERIAL" -no-reboot >/dev/null 2>&1
if [ -f "$PPM" ]; then
  magick "$PPM" "$OUT" && rm -f "$PPM"
  DIR="$(dirname "$OUT")/shots"; mkdir -p "$DIR"
  N="$(cat BUILD_NUMBER 2>/dev/null || echo 0)"; F="$DIR/build-$N.png"; i=2
  while [ -e "$F" ]; do F="$DIR/build-$N-$i.png"; i=$((i+1)); done
  cp "$OUT" "$F"
else
  echo "no screenshot produced" >&2; exit 1
fi
