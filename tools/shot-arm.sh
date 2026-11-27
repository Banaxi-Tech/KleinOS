#!/bin/sh
# Boot the ARM64 kernel headless in QEMU, take a screenshot after N seconds, then quit.
# usage: QEMU=qemu-system-aarch64 QEMU_ARGS="..." tools/shot-arm.sh <Image> <initrd> <cmdline> <out.png> [delay] [serial.log]
IMAGE="$1"; INITRD="$2"; CMDLINE="$3"; OUT="$4"; DELAY="${5:-10}"; SERIAL="${6:-/dev/null}"
PPM="${OUT%.png}.ppm"
# the shared flags use -serial stdio; here the serial port goes to a log file instead
ARGS=$(printf '%s' "$QEMU_ARGS" | sed 's/-serial stdio//')
( sleep "$DELAY"; echo "screendump $PPM"; sleep 1; echo "quit" ) | \
  timeout $((DELAY + 30)) $QEMU $ARGS ${TEST_DISK:+-drive file=$TEST_DISK,if=none,format=raw,id=hd0 -device virtio-blk-device,drive=hd0} \
    -kernel "$IMAGE" -initrd "$INITRD" -append "$CMDLINE" \
    -display none -monitor stdio -serial file:"$SERIAL" -no-reboot >/dev/null 2>&1
if [ -f "$PPM" ]; then
  magick "$PPM" "$OUT" && echo "ok" >/dev/null
else
  echo "no screenshot produced" >&2
  exit 1
fi
