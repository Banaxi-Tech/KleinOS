#!/bin/bash
# Write a KleinOS ISO to a USB stick from macOS. Everything on the stick is erased.
# usage: sudo ./flash-usb-macos.sh KleinOS.iso [disk4]
set -u

die() { echo "error: $*" >&2; exit 1; }

[ "$(uname)" = "Darwin" ] || die "this script is for macOS"
[ "$(id -u)" = "0" ] || die "run it with sudo"
[ $# -ge 1 ] || die "usage: sudo $0 KleinOS.iso [disk4]"
ISO="$1"
[ -f "$ISO" ] || die "file not found: $ISO"

if [ $# -ge 2 ]; then
    DISK="${2#/dev/}"
else
    echo "External disks:"
    diskutil list external physical
    echo
    printf "Disk to erase (like disk4): "
    read -r DISK
    DISK="${DISK#/dev/}"
fi
echo "$DISK" | grep -Eq '^disk[0-9]+$' || die "'$DISK' is not a whole disk name (expected something like disk4)"

INFO="$(diskutil info "$DISK" 2>/dev/null)" || die "no such disk: $DISK"
field() { echo "$INFO" | sed -n "s/^ *$1: *//p" | head -1; }

[ "$(field 'Whole')" = "Yes" ] || die "$DISK is not a whole disk"
[ "$(field 'Device Location')" = "External" ] || die "$DISK is not an external disk, refusing"
[ "$(field 'Protocol')" = "USB" ] || die "$DISK is not a USB disk, refusing"

ISO_BYTES=$(stat -f %z "$ISO")
DISK_BYTES=$(echo "$INFO" | sed -n 's/.*Disk Size:.*(\([0-9]*\) Bytes).*/\1/p' | head -1)
[ -n "$DISK_BYTES" ] || die "could not read the size of $DISK"
[ "$DISK_BYTES" -gt "$ISO_BYTES" ] || die "the ISO does not fit on $DISK"

echo
echo "  ISO:    $ISO ($((ISO_BYTES / 1048576)) MB)"
echo "  SHA256: $(shasum -a 256 "$ISO" | cut -d' ' -f1)"
echo "  Target: /dev/$DISK - $(field 'Device / Media Name'), $(field 'Disk Size')"
echo
echo "EVERYTHING on /dev/$DISK will be erased."
printf "Type the disk name again to continue: "
read -r CONFIRM
[ "$CONFIRM" = "$DISK" ] || die "names do not match, nothing was written"

diskutil unmountDisk "/dev/$DISK" || die "could not unmount $DISK (close anything using it)"

echo "Writing. This can take a few minutes; press Ctrl-T to see progress."
dd if="$ISO" of="/dev/r$DISK" bs=4m || die "write failed"
sync

# compare what landed on the stick with the ISO
echo "Verifying..."
WANT=$(shasum -a 256 "$ISO" | cut -d' ' -f1)
GOT=$(dd if="/dev/r$DISK" bs=1m count=$(( (ISO_BYTES + 1048575) / 1048576 )) 2>/dev/null | head -c "$ISO_BYTES" | shasum -a 256 | cut -d' ' -f1)
[ "$WANT" = "$GOT" ] || die "verification failed: the stick does not match the ISO"

diskutil eject "/dev/$DISK" >/dev/null 2>&1
echo "Done. It is safe to remove the stick."
echo "If macOS says the disk is not readable, click Ignore."
