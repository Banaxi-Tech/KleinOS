# KleinOS build system
#
#   make            -> build/KleinOS.iso (installer ISO, BIOS + UEFI)
#   make dev        -> build/dev.iso (boots straight into live desktop, no menu)
#   make install-vm -> fresh virtual disk + boot ISO once (Windows-style install flow)
#   make run        -> boot the installed virtual disk
#   make live       -> boot dev.iso in QEMU
#   make shot       -> headless screenshot of dev.iso -> $(BUILD)/shot.png
#                      (APP=<app id> opens that app at boot, SHOT_DELAY=<secs>,
#                       MODE=installer|live|oobe picks the boot mode)
#
# Use BUILD=build-<name> to get a private build directory (parallel builds).
# Every rebuild with changed sources bumps BUILD_NUMBER (shown in the OS), but
# only in the development checkout: a gitignored development.cfg containing
# development_version=true. Everyone else builds with the committed number.

# ARCH=i386 builds a 32-bit system (i686 with SSE2) into its own directory.
ARCH       ?= x86_64
ifeq ($(ARCH),i386)
BUILD      ?= build32
else
BUILD      ?= build
endif
CXX        := g++
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
LDFLAGS  := -Wl,--build-id=none -nostdlib -static -no-pie -z max-page-size=0x1000 -z noexecstack -T linker.ld

SRCS := $(filter-out $(SKIP),$(shell find kernel -name '*.cpp'))
ASMS := $(shell find kernel -name '*.S')
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS) $(ASMS))
HDRS := $(shell find kernel apps libs drivers -name '*.h' 2>/dev/null)

# .dl dynamic libraries: every directory in apps/ becomes /System/Apps/<Name>.dl,
# every directory in libs/ becomes /System/Libraries/<Name>.dl. They are ELF
# shared objects linked at load time against the kernel's exported symbols.
# Every directory in drivers/ becomes /System/Drivers/<Name>.dri (same format).
DL_CXXFLAGS := $(filter-out -fno-pic -fno-pie -mcmodel=small,$(CXXFLAGS)) -fPIC -fvisibility=default
DL_LDFLAGS  := -shared -nostdlib -Wl,-Bsymbolic -Wl,-z,now -Wl,--hash-style=both -Wl,-z,max-page-size=0x1000 -Wl,-z,noexecstack
APP_DIRS := $(filter-out $(SKIP),$(patsubst %/,%,$(sort $(dir $(wildcard apps/*/*.cpp)))))
LIB_DIRS := $(filter-out $(SKIP),$(patsubst %/,%,$(sort $(dir $(wildcard libs/*/*.cpp libs/*/*.c)))))
APP_DLS  := $(patsubst apps/%,$(BUILD)/initrd/System/Apps/%.dl,$(APP_DIRS))
LIB_DLS  := $(patsubst libs/%,$(BUILD)/initrd/System/Libraries/%.dl,$(LIB_DIRS))
DRV_DIRS := $(filter-out $(SKIP),$(patsubst %/,%,$(sort $(dir $(wildcard drivers/*/*.cpp)))))
DRV_DRIS := $(patsubst drivers/%,$(BUILD)/initrd/System/Drivers/%.dri,$(DRV_DIRS))
DL_SRCS  := $(wildcard apps/*/*.cpp libs/*/*.cpp libs/*/*.c drivers/*/*.cpp)
# C sources in libs/ (e.g. the QuickJS engine) use a per-library include/ dir as libc
DL_CFLAGS := -std=gnu11 -O2 -g -ffreestanding -fPIC -fno-stack-protector -mno-red-zone -fno-asynchronous-unwind-tables \
             -fno-math-errno -msse2 -mfpmath=sse -D__wasi__ -U__linux__ -U__unix__ -U__GLIBC__ -w -MMD -MP $(ARCH_CFLAGS)
DL_OBJS  := $(patsubst %,$(BUILD)/%.o,$(DL_SRCS))
INITRD   := $(BUILD)/initrd.tar

.PHONY: all dev iso run live install-vm shot clean check

all: $(ISO)
dev: $(DEV_ISO)

# ---------------------------------------------------------------- build number
$(BUILD)/buildnum.stamp: $(SRCS) $(ASMS) $(HDRS) $(DL_SRCS) assets/fonts/*
	@mkdir -p $(BUILD)/gen
	@$(if $(DEV_VERSION),flock .build_number.lock sh -c 'n=$$(cat BUILD_NUMBER 2>/dev/null || echo 0); echo $$((n+1)) > BUILD_NUMBER',true)
	@printf '#define KLEIN_BUILD_NUMBER %s\n#define KLEIN_DEV_BUILD %s\n' "$$(cat BUILD_NUMBER)" "$(if $(DEV_VERSION),1,0)" > $(BUILD)/gen/build_number.h.tmp
	@cmp -s $(BUILD)/gen/build_number.h.tmp $(BUILD)/gen/build_number.h || mv $(BUILD)/gen/build_number.h.tmp $(BUILD)/gen/build_number.h
	@touch $@
	@echo "  BUILD  $$(cat BUILD_NUMBER)"

$(BUILD)/gen/build_number.h: $(BUILD)/buildnum.stamp

# ---------------------------------------------------------------- compile
$(BUILD)/%.cpp.o: %.cpp | $(BUILD)/gen/build_number.h
	@mkdir -p $(dir $@)
	@echo "  CXX    $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS     $<"
	@$(CXX) $(CXXFLAGS) -Wa,-I. -c $< -o $@

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
	@gcc $(DL_CFLAGS) -isystem libs/$(firstword $(subst /, ,$(patsubst libs/%,%,$<)))/include -c $< -o $@

$(BUILD)/libs/%.cpp.o: libs/%.cpp | $(BUILD)/gen/build_number.h
	@mkdir -p $(dir $@)
	@echo "  CXX    $< (dl)"
	@$(CXX) $(DL_CXXFLAGS) -Ilibs -c $< -o $@

.SECONDEXPANSION:
$(BUILD)/initrd/System/Apps/%.dl: $$(addprefix $(BUILD)/,$$(addsuffix .o,$$(wildcard apps/$$*/*.cpp)))
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
	@cp assets/fonts/* $(BUILD)/initrd/System/Fonts/
	@nm --defined-only $(KERNEL) | awk '$$2 ~ /^[TDBRVWG]$$/ {print $$1, $$3}' > $(BUILD)/initrd/System/kernel.sym
	@if [ -d assets/root ]; then cp -r assets/root/. $(BUILD)/initrd/; fi
	@cd $(BUILD)/initrd && tar --format=ustar --owner=0 --group=0 -cf ../initrd.tar .

$(KERNEL): $(OBJS) linker.ld
	@echo "  LD     $@"
	@$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) -lgcc
	@grub-file --is-x86-multiboot2 $@ || (echo "kernel is not multiboot2!" && false)

# Syntax/type check a single file: make check F=apps/Calculator/main.cpp
check: $(BUILD)/gen/build_number.h
	$(CXX) $(if $(filter apps/% libs/% drivers/%,$(F)),$(DL_CXXFLAGS) -Iapps -Ilibs,$(CXXFLAGS)) -fsyntax-only $(F)

# ---------------------------------------------------------------- installed-system disk image
# MBR: GRUB boot.img + core.img (sectors 1..), partition 1 FAT16 (kernel),
# partition 2 type 0x7f (KleinOS data: settings, user account, files).
SYS_P1_START := 2048
SYS_P1_SIZE  := 49152
SYS_P2_START := 51200
SYS_P2_SIZE  := 32768
SYS_SECTORS  := 83968

$(BUILD)/core.img: tools/grub-early.cfg
	@mkdir -p $(BUILD)
	@grub-mkimage -O i386-pc -o $@ -p '(hd0,msdos1)/boot/grub' -c tools/grub-early.cfg \
		biosdisk part_msdos fat multiboot2 normal configfile all_video vbe vga video_bochs video_cirrus gfxterm echo boot

$(SYSIMG): $(KERNEL) $(INITRD) $(BUILD)/core.img
	@echo "  IMG    $@"
	@rm -f $@
	@dd if=/dev/zero of=$@ bs=512 count=$(SYS_SECTORS) status=none
	@printf 'label: dos\nstart=$(SYS_P1_START), size=$(SYS_P1_SIZE), type=6, bootable\nstart=$(SYS_P2_START), size=$(SYS_P2_SIZE), type=7f\n' | sfdisk -q $@
	@dd if=/usr/lib/grub/i386-pc/boot.img of=$@ bs=440 count=1 conv=notrunc status=none
	@dd if=$(BUILD)/core.img of=$@ bs=512 seek=1 conv=notrunc status=none
	@mformat -i $@@@1M -t 96 -h 16 -s 32 -H $(SYS_P1_START) -v KLEINOS ::
	@mmd -i $@@@1M ::/boot ::/boot/grub
	@mcopy -i $@@@1M $(KERNEL) ::/boot/kernel.elf
	@mcopy -i $@@@1M $(INITRD) ::/boot/initrd.tar
	@mcopy -i $@@@1M tools/grub-hdd.cfg ::/boot/grub/grub.cfg

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

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(DL_OBJS:.o=.d)
