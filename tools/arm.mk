# ARM64 (QEMU "virt", UEFI) targets, included by the Makefile when ARCH=aarch64.
#
# The kernel Image is also a UEFI application: BOOTAA64.EFI on a FAT volume. kernel/arch/aarch64/efi_stub.cpp
# loads /boot/initrd.tar, /boot/system.img and /boot/cmdline.txt from that volume. `live` skips firmware and
# boots the Image directly. The installer works like on x86: boot the installer image, Setup writes system.img
# to the target disk.

CPU          ?= cortex-a72
GIC          ?= 3
CMDLINE      ?= mode=live
FIRMWARE     ?= /usr/share/edk2/aarch64/QEMU_EFI.fd
INSTALLER    := $(BUILD)/KleinOS-installer.img
USB_IMG      := $(BUILD)/KleinOS-USB.img
SYS_SECTORS  := 260096
INST_SECTORS := 400000
USB_SECTORS  := 1048576
comma        := ,

QEMU_FLAGS := -machine virt,gic-version=$(GIC) -cpu $(CPU) -m 2048 -smp 1 \
              -device ramfb -device virtio-keyboard-device -device virtio-tablet-device \
              -netdev user,id=n0 -device virtio-net-device,netdev=n0 -serial stdio
EFI_FLAGS  := $(QEMU_FLAGS) -bios $(FIRMWARE)
DISK_DEV   := -drive file=$(DISK),if=none,format=raw,id=hd0 -device virtio-blk-device,drive=hd0

$(IMAGE): $(KERNEL)
	@echo "  IMAGE  $@"
	@$(OBJCOPY) -O binary $< $@

live: $(IMAGE) $(INITRD)
	$(QEMU) $(QEMU_FLAGS) $(if $(wildcard $(DISK)),$(DISK_DEV)) \
		-kernel $(IMAGE) -initrd $(INITRD) -append "$(CMDLINE) $(if $(APP),autostart=$(APP)) $(EXTRA_CMDLINE)"

shot: $(IMAGE) $(INITRD)
	@QEMU="$(QEMU)" QEMU_ARGS="$(QEMU_FLAGS)" tools/shot-arm.sh $(IMAGE) $(INITRD) \
		"$(CMDLINE) $(if $(APP),autostart=$(APP)) $(EXTRA_CMDLINE)" $(BUILD)/shot.png $(SHOT_DELAY) $(BUILD)/serial.log
	@echo "screenshot: $(BUILD)/shot.png  serial: $(BUILD)/serial.log"

# $(1) output, $(2) total sectors, $(3) sectors per cluster, $(4) kernel command line, $(5) extra file to put in /boot
define make_efi_image
	@echo "  IMG    $(1)"
	@truncate -s 0 $(1)
	@truncate -s $$(( $(2) * 512 )) $(1)
	@printf 'label: dos\nstart=2048, type=c, bootable\n' | sfdisk -q $(1)
	@mformat -i $(1)@@1M -F -T $$(( $(2) - 2048 )) -c $(3) -H 2048 -v KLEINOS ::
	@mmd -i $(1)@@1M ::/EFI ::/EFI/BOOT ::/boot ::/Users ::/KleinOS
	@mcopy -i $(1)@@1M $(IMAGE) ::/EFI/BOOT/BOOTAA64.EFI
	@mcopy -i $(1)@@1M $(INITRD) ::/boot/initrd.tar
	@printf '%s\n' '$(4)' > $(BUILD)/cmdline.tmp
	@mcopy -i $(1)@@1M $(BUILD)/cmdline.tmp ::/boot/cmdline.txt
	$(if $(5),@mcopy -i $(1)@@1M $(5) ::/boot/system.img)
endef

# the installed system (what Setup writes to the disk)
$(SYSIMG): $(IMAGE) $(INITRD)
	$(call make_efi_image,$@,$(SYS_SECTORS),1,mode=installed,)

# installer media: boot it from a USB stick or disk; Setup copies system.img to the target disk
installer: $(INSTALLER)
$(INSTALLER): $(IMAGE) $(INITRD) $(SYSIMG)
	$(call make_efi_image,$@,$(INST_SECTORS),4,mode=installer,$(SYSIMG))

# the whole installed system for a USB stick, running from the stick (dd it to the device)
usb-image: $(USB_IMG)
$(USB_IMG): $(IMAGE) $(INITRD)
	$(call make_efi_image,$@,$(USB_SECTORS),8,mode=installed storage=usb,)

# fresh virtual disk, boot the installer from a virtual USB stick, install, then `make ARCH=aarch64 run`
install-vm: $(INSTALLER)
	@truncate -s 0 $(DISK)
	@truncate -s 8G $(DISK)
	$(QEMU) $(EFI_FLAGS) $(DISK_DEV) -device qemu-xhci \
		-drive file=$(INSTALLER),if=none,format=raw,id=inst -device usb-storage,drive=inst,bootindex=0

run: $(DISK)
	$(QEMU) $(EFI_FLAGS) $(DISK_DEV)

# boot the USB image the way a stick boots (EHCI), exercising storage=usb
run-usb: $(USB_IMG)
	$(QEMU) $(EFI_FLAGS) -device usb-ehci,id=ehci -drive file=$(USB_IMG),if=none,format=raw,id=usb0 \
		-device usb-storage,bus=ehci.0,drive=usb0,bootindex=0

$(DISK):
	@qemu-img create -f raw $(DISK) 8G >/dev/null
