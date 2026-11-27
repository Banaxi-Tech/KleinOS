#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "boot/multiboot2.h"
#include "drivers/serial.h"
#include "drivers/framebuffer.h"
#include "drivers/display_mode.h"
#include "drivers/input.h"
#include "drivers/rtc.h"
#include "drivers/pci.h"
#include "drivers/ata.h"
#include "drivers/usb_storage.h"
#if defined(__aarch64__)
void ramfb_init(const BootInfo& bi);
void virtio_net_init();
#endif
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/log.h"
#include "sys/system.h"
#include "sys/panic.h"
#include "build_number.h"

void run_global_constructors();

extern "C" void kmain(uint32_t magic, uintptr_t info) {
    serial_init();
    BootInfo& bi = boot_info();
    boot_parse(magic, info, bi);
    heap_init(bi.heap_start, bi.heap_size);
    run_global_constructors();
    boot_parse(magic, info, bi);   // constructors may have reset statics

    klog("KleinOS %s.%d booting, cmdline: '%s'", KLEIN_VERSION, KLEIN_BUILD_NUMBER, bi.cmdline);
    klog("memory: %llu MiB usable, heap %llu MiB at %p", (unsigned long long)(bi.mem_total >> 20),
         (unsigned long long)(bi.heap_size >> 20), (void*)bi.heap_start);
    FramebufferInfo& fb = framebuffer();
#if defined(__aarch64__)
    ramfb_init(bi);     // no boot loader framebuffer on ARM: program QEMU's ramfb ourselves
    if (!fb.address) kpanic("no display: start QEMU with -device ramfb");
#endif
    klog("framebuffer: %ux%u bpp=%u pitch=%u at %p", fb.width, fb.height, fb.bpp, fb.pitch, fb.address);
    klog("cpu: %s", cpu_brand());
#if !defined(__aarch64__)
    klog("framebuffer: write-combining %s",
         cpu_framebuffer_wc((uintptr_t)fb.address, (size_t)fb.pitch * fb.height) ? "on" : "unavailable");
#endif

    interrupts_init();
    sti();
    rtc_init();
    pci_init();
    if (display_set_native_mode()) {
        klog("framebuffer: now %ux%u pitch=%u, write-combining %s", fb.width, fb.height, fb.pitch,
             cpu_framebuffer_wc((uintptr_t)fb.address, (size_t)fb.pitch * fb.height) ? "on" : "unavailable");
    }
    ata_init();
#if defined(__aarch64__)
    virtio_net_init();      // before net::init() (system_main), which uses whichever NIC registered
#endif
    {
        char storage[8];
        if (boot_arg("storage", storage, sizeof(storage)) && !strcmp(storage, "usb")) usb_storage_init();
    }
    input_init(fb.width, fb.height);

    system_main();
}
