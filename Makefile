# KleinOS build system
#
#   make            -> build/KleinOS.iso (installer ISO, BIOS + UEFI)
#   make dev        -> build/dev.iso (boots straight into live desktop, no menu)
#   make install-vm -> fresh virtual disk + boot ISO once (Windows-style install flow)
#   make run        -> boot the installed virtual disk
#   make live       -> boot dev.iso in QEMU
#   make usb-image  -> $(BUILD)/KleinOS-USB.img (a whole installed system to flash to a USB stick)
#   make shot       -> headless screenshot of dev.iso -> $(BUILD)/shot.png
#                      (APP=<app id> opens that app at boot, SHOT_DELAY=<secs>,
#                       MODE=installer|live|oobe picks the boot mode)
#
# ARCH=aarch64 (alias arm64) builds the ARM64 port for QEMU's "virt" machine with clang + lld:
#   make ARCH=aarch64          -> build-arm64/Image + initrd.tar (arm64 Linux-style boot image)
#   make ARCH=aarch64 live     -> boot it in qemu-system-aarch64 (ramfb display, virtio keyboard/tablet/net)
#   make ARCH=aarch64 shot     -> headless screenshot -> $(BUILD)/shot.png
#   make ARCH=aarch64 installer-> KleinOS-installer.img (UEFI); `make ARCH=aarch64 install-vm`, then `run`
#   make ARCH=aarch64 usb-image-> KleinOS-USB.img: a whole system for a USB stick (dd it, boot via UEFI)
#
# Use BUILD=build-<name> to get a private build directory (parallel builds).
# Every rebuild with changed sources bumps BUILD_NUMBER (shown in the OS), but
# only in the development checkout: a gitignored development.cfg containing
# development_version=true. Everyone else builds with the committed number.

# ARCH=i386 builds a 32-bit system (i686 with SSE2) into its own directory.
# ARCH=aarch64 builds the ARM64 port (see above).
ARCH       ?= x86_64
ifeq ($(ARCH),arm64)
override ARCH := aarch64
endif
ifeq ($(ARCH),i386)
BUILD      ?= build32
else ifeq ($(ARCH),aarch64)
BUILD      ?= build-arm64
else
BUILD      ?= build
endif
CXX        := g++
CC         := gcc
KERNEL     := $(BUILD)/kernel.elf
ISO        := $(BUILD)/KleinOS.iso
DEV_ISO    := $(BUILD)/dev.iso
SYSIMG     := $(BUILD)/system.img
DISK       ?= $(BUILD)/disk.img
APP        ?=
SHOT_DELAY ?= 6
SKIP       ?=
DEV_VERSION := $(shell grep -qs '^development_version=true' development.cfg && echo 1)
QEMU       := qemu-system-x86_64
QEMU_FLAGS := -machine pc -m 2048 -smp 1 -enable-kvm -cpu host -vga std \
              -netdev user,id=n0 -device e1000,netdev=n0 \
              -audiodev none,id=snd0 -machine pcspk-audiodev=snd0 \
              -serial stdio

CXXFLAGS := -std=gnu++20 -O2 -g -ffreestanding -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -fno-stack-protector -fno-pic -fno-pie \
            -mno-red-zone -mcmodel=small -fno-asynchronous-unwind-tables \
            -fno-omit-frame-pointer -msse2 -mfpmath=sse -fno-math-errno \
            -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            -Wno-sign-compare -Wno-unused-function \
            -Ikernel/libc -Ikernel -I$(BUILD)/gen -MMD -MP
ifeq ($(ARCH),i386)
QEMU       := qemu-system-i386
QEMU_FLAGS := $(subst -cpu host,-cpu qemu32,$(QEMU_FLAGS))
CXXFLAGS   := $(filter-out -mcmodel=small -mno-red-zone,$(CXXFLAGS)) -m32 -march=i686 -mtune=bonnell
ARCH_CFLAGS := -m32 -march=i686 -mtune=bonnell
DL_LIBS    := -lgcc
endif
ifeq ($(ARCH),aarch64)
# clang + lld cross toolchain (no aarch64 gcc needed). compiler-rt's builtins stand in for libgcc, and
# kernel/libcxx has the few freestanding C++ headers (<new>, <utility>, <initializer_list>).
TRIPLE     := aarch64-none-elf
CXX        := clang++ --target=$(TRIPLE) -fuse-ld=lld -Wno-unused-command-line-argument
CC         := clang --target=$(TRIPLE) -fuse-ld=lld -Wno-unused-command-line-argument
NM         := llvm-nm
OBJCOPY    := llvm-objcopy
LIBGCC     := $(wildcard $(shell clang --target=$(TRIPLE) -print-libgcc-file-name 2>/dev/null))
QEMU       := qemu-system-aarch64
CXXFLAGS   := $(filter-out -mno-red-zone -mcmodel=small -msse2 -mfpmath=sse,$(CXXFLAGS)) -Ikernel/libcxx \
              -Wno-unused-variable -Wno-unused-private-field
ARCH_CFLAGS :=
# The kernel is a position independent executable that relocates itself at boot (see boot.S): UEFI and U-Boot
# load it anywhere. Only kernel objects get -fPIE; apps/libs/drivers are -fPIC shared objects as usual.
ARCH_KFLAGS := -fPIE
DL_LIBS    := $(LIBGCC)
else
NM         := nm
OBJCOPY    := objcopy
endif
LDSCRIPT := linker.ld
ifeq ($(ARCH),aarch64)
LDSCRIPT := linker-aarch64.ld
endif
LDFLAGS  := -Wl,--build-id=none -nostdlib -static -no-pie -z max-page-size=0x1000 -z noexecstack -T $(LDSCRIPT)
ifeq ($(ARCH),aarch64)
LDFLAGS  := -Wl,--build-id=none -nostdlib -Wl,-pie -Wl,--no-dynamic-linker -Wl,-z,notext -Wl,-z,max-page-size=0x1000 \
            -Wl,-z,noexecstack -Wl,-T,$(LDSCRIPT)
endif

# Source files that only exist for one architecture family.
X86_ONLY := kernel/arch/boot.S kernel/arch/isr.S kernel/arch/cpu.cpp kernel/arch/interrupts.cpp \
            kernel/boot/multiboot2.cpp kernel/drivers/ata.cpp kernel/drivers/display_mode.cpp
ARM_ONLY := kernel/arch/aarch64/% kernel/drivers/arm/% kernel/boot/dtb.cpp
ifeq ($(ARCH),aarch64)
# Drivers for x86 chipsets (port I/O, PCI) have nothing to bind to on ARM.
ARCH_SKIP := $(X86_ONLY) drivers/ACPIBattery drivers/AR9285 drivers/GenericWiFi drivers/GenericEthernet drivers/IntelHDA
else
ARCH_SKIP := $(ARM_ONLY)
endif

SRCS := $(filter-out $(SKIP) $(ARCH_SKIP),$(shell find kernel -name '*.cpp'))
ASMS := $(filter-out $(ARCH_SKIP),$(shell find kernel -name '*.S'))
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS) $(ASMS))
HDRS := $(shell find kernel apps libs drivers -name '*.h' 2>/dev/null)

# Every directory in apps/ becomes /System/Apps/<Name>.run,
# every directory in libs/ becomes /System/Libraries/<Name>.dl. They are ELF
# shared objects linked at load time against the kernel's exported symbols.
# Every directory in drivers/ becomes /System/Drivers/<Name>.dri (same format).
DL_CXXFLAGS := $(filter-out -fno-pic -fno-pie -mcmodel=small,$(CXXFLAGS)) -fPIC -fvisibility=default
DL_LDFLAGS  := -shared -nostdlib -Wl,-Bsymbolic -Wl,-z,now -Wl,--hash-style=both -Wl,-z,max-page-size=0x1000 -Wl,-z,noexecstack
APP_DIRS := $(filter-out $(SKIP),$(patsubst %/,%,$(sort $(dir $(wildcard apps/*/*.cpp)))))
LIB_DIRS := $(filter-out $(SKIP),$(patsubst %/,%,$(sort $(dir $(wildcard libs/*/*.cpp libs/*/*.c)))))
APP_DLS  := $(patsubst apps/%,$(BUILD)/initrd/System/Apps/%.run,$(APP_DIRS))
LIB_DLS  := $(patsubst libs/%,$(BUILD)/initrd/System/Libraries/%.dl,$(LIB_DIRS))
DRV_DIRS := $(filter-out $(SKIP) $(ARCH_SKIP),$(patsubst %/,%,$(sort $(dir $(wildcard drivers/*/*.cpp)))))
DRV_DRIS := $(patsubst drivers/%,$(BUILD)/initrd/System/Drivers/%.dri,$(DRV_DIRS))
DL_SRCS  := $(wildcard apps/*/*.cpp libs/*/*.cpp libs/*/*.c drivers/*/*.cpp)
# C sources in libs/ (e.g. the QuickJS engine) use a per-library include/ dir as libc
DL_CFLAGS := -std=gnu11 -O2 -g -ffreestanding -fPIC -fno-stack-protector -mno-red-zone -fno-asynchronous-unwind-tables \
             -fno-math-errno -msse2 -mfpmath=sse -D__wasi__ -U__linux__ -U__unix__ -U__GLIBC__ -w -MMD -MP $(ARCH_CFLAGS)
ifeq ($(ARCH),aarch64)
DL_CFLAGS := $(filter-out -mno-red-zone -msse2 -mfpmath=sse,$(DL_CFLAGS))
# The bare-metal clang driver ignores -shared, so hand it to lld directly. The kernel supplies the symbols
# at load time (like GNU ld, lld otherwise rejects undefined ones).
DL_LDFLAGS := -Wl,-shared $(filter-out -shared,$(DL_LDFLAGS)) -Wl,-z,undefs
endif
DL_OBJS  := $(patsubst %,$(BUILD)/%.o,$(DL_SRCS))
INITRD   := $(BUILD)/initrd.tar

.PHONY: all dev iso run live install-vm shot clean check installer usb-image run-usb

ifeq ($(ARCH),aarch64)
IMAGE := $(BUILD)/Image
all: $(IMAGE) $(INITRD)
dev: all
else
all: $(ISO)
dev: $(DEV_ISO)
endif

# ---------------------------------------------------------------- build number
$(BUILD)/buildnum.stamp: $(SRCS) $(ASMS) $(HDRS) $(DL_SRCS) assets/fonts/*
	@mkdir -p $(BUILD)/gen
	@$(if $(DEV_VERSION),flock .build_number.lock sh -c 'n=$$(cat BUILD_NUMBER 2>/dev/null || echo 0); echo $$((n+1)) > BUILD_NUMBER',true)
	@printf '#define KLEIN_BUILD_NUMBER %s\n#define KLEIN_DEV_BUILD %s\n' "$$(cat BUILD_NUMBER)" "$(if $(DEV_VERSION),1,0)" > $(BUILD)/gen/build_number.h.tmp
	@cmp -s $(BUILD)/gen/build_number.h.tmp $(BUILD)/gen/build_number.h || mv $(BUILD)/gen/build_number.h.tmp $(BUILD)/gen/build_number.h
	@touch $@
	@echo "  BUILD  $$(cat BUILD_NUMBER)"

$(BUILD)/gen/build_number.h: $(BUILD)/buildnum.stamp

# The installed system's grub.cfg, built into the kernel so updates can bring old installs up to date.
$(BUILD)/gen/grub_hdd_cfg.h: tools/grub-hdd.cfg
	@mkdir -p $(BUILD)/gen
	@{ printf 'static const char kGrubHddCfg[] =\n'; sed 's/\\/\\\\/g; s/"/\\"/g; s/^/    "/; s/$$/\\n"/' $<; printf '    ;\n'; } > $@
$(BUILD)/kernel/sys/update.cpp.o: $(BUILD)/gen/grub_hdd_cfg.h

# ---------------------------------------------------------------- compile
$(BUILD)/%.cpp.o: %.cpp | $(BUILD)/gen/build_number.h
	@mkdir -p $(dir $@)
	@echo "  CXX    $<"
	@$(CXX) $(CXXFLAGS) $(ARCH_KFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS     $<"
	@$(CXX) $(CXXFLAGS) $(ARCH_KFLAGS) -Wa,-I. -c $< -o $@

# .dl objects (position independent)
$(BUILD)/apps/%.cpp.o: apps/%.cpp | $(BUILD)/gen/build_number.h
	@mkdir -p $(dir $@)
	@echo "  CXX    $< (dl)"
	@$(CXX) $(DL_CXXFLAGS) -Iapps -Ilibs -c $< -o $@

$(BUILD)/drivers/%.cpp.o: drivers/%.cpp | $(BUILD)/gen/build_number.h
	@mkdir -p $(dir $@)
	@echo "  CXX    $< (dri)"
	@$(CXX) $(DL_CXXFLAGS) -c $< -o $@

$(BUILD)/libs/%.c.o: libs/%.c
	@mkdir -p $(dir $@)
	@echo "  CC     $< (dl)"
	@$(CC) $(DL_CFLAGS) -isystem libs/$(firstword $(subst /, ,$(patsubst libs/%,%,$<)))/include -c $< -o $@

$(BUILD)/libs/%.cpp.o: libs/%.cpp | $(BUILD)/gen/build_number.h
	@mkdir -p $(dir $@)
	@echo "  CXX    $< (dl)"
	@$(CXX) $(DL_CXXFLAGS) -Ilibs -c $< -o $@

.SECONDEXPANSION:
$(BUILD)/initrd/System/Apps/%.run: $$(addprefix $(BUILD)/,$$(addsuffix .o,$$(wildcard apps/$$*/*.cpp)))
	@mkdir -p $(dir $@)
	@echo "  DL     $@"
	@$(CXX) $(filter-out -MMD -MP,$(DL_CXXFLAGS)) $(DL_LDFLAGS) -o $@ $^ $(DL_LIBS)

$(BUILD)/initrd/System/Drivers/%.dri: $$(addprefix $(BUILD)/,$$(addsuffix .o,$$(wildcard drivers/$$*/*.cpp)))
	@mkdir -p $(dir $@)
	@echo "  DRI    $@"
	@$(CXX) $(filter-out -MMD -MP,$(DL_CXXFLAGS)) $(DL_LDFLAGS) -o $@ $^ $(DL_LIBS)

$(BUILD)/initrd/System/Libraries/%.dl: $$(addprefix $(BUILD)/,$$(addsuffix .o,$$(wildcard libs/$$*/*.cpp libs/$$*/*.c)))
	@mkdir -p $(dir $@)
	@echo "  DL     $@"
	@$(CXX) $(filter-out -MMD -MP,$(DL_CXXFLAGS)) $(DL_LDFLAGS) -o $@ $^ $(DL_LIBS)

# initrd: /System/{Apps,Libraries,Fonts,kernel.sym} + assets/root/* as-is
$(INITRD): $(KERNEL) $(APP_DLS) $(LIB_DLS) $(DRV_DRIS) $(wildcard assets/fonts/*) $(shell find assets/root -type f 2>/dev/null)
	@echo "  INITRD $@"
	@mkdir -p $(BUILD)/initrd/System/Fonts $(BUILD)/initrd/System/Apps $(BUILD)/initrd/System/Libraries $(BUILD)/initrd/System/Drivers
	@rm -f $(BUILD)/initrd/System/Apps/*.dl
	@cp assets/fonts/* $(BUILD)/initrd/System/Fonts/
	@$(NM) --defined-only $(KERNEL) | awk '$$2 ~ /^[TDBRVWG]$$/ {print $$1, $$3}' > $(BUILD)/initrd/System/kernel.sym
	@if [ -d assets/root ]; then cp -r assets/root/. $(BUILD)/initrd/; fi
	@cd $(BUILD)/initrd && tar --format=ustar --owner=0 --group=0 -cf ../initrd.tar .

ifeq ($(ARCH),aarch64)
$(KERNEL): $(OBJS) $(LDSCRIPT)
	@echo "  LD     $@"
	@$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)
else
$(KERNEL): $(OBJS) $(LDSCRIPT)
	@echo "  LD     $@"
	@$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) -lgcc
	@grub-file --is-x86-multiboot2 $@ || (echo "kernel is not multiboot2!" && false)
endif

# Syntax/type check a single file: make check F=apps/Calculator/main.cpp
check: $(BUILD)/gen/build_number.h
	$(CXX) $(if $(filter apps/% libs/% drivers/%,$(F)),$(DL_CXXFLAGS) -Iapps -Ilibs,$(CXXFLAGS)) -fsyntax-only $(F)

ifeq ($(ARCH),aarch64)
include tools/arm.mk
else
# ---------------------------------------------------------------- installed-system disk image
# MBR: GRUB boot.img + core.img (sectors 1..), then one FAT32 partition with
# /boot (kernel, initrd), /KleinOS (configuration) and /Users as normal files.
SYS_SECTORS := 260096
USB_SECTORS := 1048576
USB_IMG     := $(BUILD)/KleinOS-USB.img

$(BUILD)/core.img: tools/grub-early.cfg
	@mkdir -p $(BUILD)
	@grub-mkimage -O i386-pc -o $@ -p '(hd0,msdos1)/boot/grub' -c tools/grub-early.cfg \
		biosdisk part_msdos fat multiboot2 normal configfile test loadenv all_video vbe vga video_bochs video_cirrus gfxterm echo boot

# $(1) output, $(2) total sectors, $(3) sectors per cluster, $(4) grub.cfg
define make_system_image
	@echo "  IMG    $(1)"
	@rm -f $(1)
	@dd if=/dev/zero of=$(1) bs=512 count=0 seek=$(2) status=none
	@printf 'label: dos\nstart=2048, type=c, bootable\n' | sfdisk -q $(1)
	@dd if=/usr/lib/grub/i386-pc/boot.img of=$(1) bs=440 count=1 conv=notrunc status=none
	@dd if=$(BUILD)/core.img of=$(1) bs=512 seek=1 conv=notrunc status=none
	@mformat -i $(1)@@1M -F -T $$(( $(2) - 2048 )) -c $(3) -H 2048 -v KLEINOS ::
	@mmd -i $(1)@@1M ::/boot ::/boot/grub ::/Users ::/KleinOS
	@rm -f $(1).env && grub-editenv $(1).env create && grub-editenv $(1).env set slot=a
	@mcopy -i $(1)@@1M $(1).env ::/boot/grubenv
	@rm -f $(1).env
	@mcopy -i $(1)@@1M $(KERNEL) ::/boot/kernel_a.elf
	@mcopy -i $(1)@@1M $(INITRD) ::/boot/initrd_a.tar
	@mcopy -i $(1)@@1M $(4) ::/boot/grub/grub.cfg
endef

$(SYSIMG): $(KERNEL) $(INITRD) $(BUILD)/core.img tools/grub-hdd.cfg
	$(call make_system_image,$@,$(SYS_SECTORS),1,tools/grub-hdd.cfg)

# A complete installed system for a USB stick: flash it and boot from it.
usb-image: $(USB_IMG)
$(USB_IMG): $(KERNEL) $(INITRD) $(BUILD)/core.img tools/grub-hdd.cfg
	@sed 's/mode=installed/mode=installed storage=usb/' tools/grub-hdd.cfg > $(BUILD)/grub-usb.cfg
	$(call make_system_image,$@,$(USB_SECTORS),8,$(BUILD)/grub-usb.cfg)

# ---------------------------------------------------------------- update package
# make ARCH=i386 kupd TAG=v1.1.731 -> $(BUILD)/release/{KleinOS-*.kupd,latest-*.json}
kupd: $(KERNEL) $(INITRD)
	@test -n "$(TAG)" || (echo "usage: make kupd TAG=<release tag>" && false)
	@python3 tools/kupd.py pack --arch $(ARCH) \
		--version $$(sed -n 's/^#define KLEIN_VERSION "\([0-9.]*\)".*/\1/p' kernel/sys/system.h) \
		--build $$(sed -n 's/^#define KLEIN_BUILD_NUMBER //p' $(BUILD)/gen/build_number.h) \
		--kernel $(KERNEL) --initrd $(INITRD) --tag $(TAG) --outdir $(BUILD)/release \
		$(foreach f,$(PATCH_FROM),--patch-from $(f))

# ---------------------------------------------------------------- ISOs
$(ISO): $(KERNEL) $(INITRD) $(SYSIMG) tools/grub-iso.cfg
	@echo "  ISO    $@"
	@rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	@cp $(KERNEL) $(BUILD)/iso/boot/kernel.elf
	@cp $(SYSIMG) $(BUILD)/iso/boot/system.img
	@cp $(INITRD) $(BUILD)/iso/boot/initrd.tar
	@cp tools/grub-iso.cfg $(BUILD)/iso/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(BUILD)/iso 2>/dev/null

$(DEV_ISO): $(KERNEL) $(INITRD) $(if $(filter installer,$(MODE)),$(SYSIMG)) FORCE
	@rm -rf $(BUILD)/deviso && mkdir -p $(BUILD)/deviso/boot/grub
	@cp $(KERNEL) $(BUILD)/deviso/boot/kernel.elf
	@cp $(INITRD) $(BUILD)/deviso/boot/initrd.tar
	@$(if $(filter installer,$(MODE)),cp $(SYSIMG) $(BUILD)/deviso/boot/system.img,true)
	@printf 'set timeout=0\nset default=0\ninsmod all_video\nset gfxpayload=keep\nmenuentry "KleinOS dev" {\n  multiboot2 /boot/kernel.elf mode=$(or $(MODE),live) $(if $(APP),autostart=$(APP)) $(EXTRA_CMDLINE)\n  module2 /boot/initrd.tar initrd\n$(if $(filter installer,$(MODE)),  module2 /boot/system.img system.img\n)  boot\n}\n' > $(BUILD)/deviso/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(BUILD)/deviso 2>/dev/null

FORCE:

# ---------------------------------------------------------------- run targets
$(DISK):
	@qemu-img create -f raw $(DISK) 8G >/dev/null

install-vm: $(ISO)
	@rm -f $(DISK) && qemu-img create -f raw $(DISK) 8G >/dev/null
	$(QEMU) $(QEMU_FLAGS) -drive file=$(DISK),format=raw,if=ide,index=0 -cdrom $(ISO) -boot order=c,once=d

run: $(DISK)
	$(QEMU) $(QEMU_FLAGS) -drive file=$(DISK),format=raw,if=ide,index=0 -cdrom $(ISO) -boot order=c

live: $(DEV_ISO)
	$(QEMU) $(QEMU_FLAGS) -cdrom $(DEV_ISO) -boot d

shot: $(DEV_ISO)
	@TEST_DISK=$(TEST_DISK) tools/shot.sh $(DEV_ISO) $(BUILD)/shot.png $(SHOT_DELAY) $(BUILD)/serial.log
	@echo "screenshot: $(BUILD)/shot.png  serial: $(BUILD)/serial.log"
endif

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(DL_OBJS:.o=.d)
