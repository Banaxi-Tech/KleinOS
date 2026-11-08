#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "boot/multiboot2.h"
#include "drivers/serial.h"
#include "drivers/framebuffer.h"
#include "drivers/input.h"
#include "drivers/rtc.h"
#include "drivers/pci.h"
#include "drivers/ata.h"
#include "lib/heap.h"
#include "lib/log.h"
#include "sys/system.h"
#include "build_number.h"

void run_global_constructors();

extern "C" void kmain(uint32_t magic, uint32_t info) {
    serial_init();
    BootInfo& bi = boot_info();
    multiboot2_parse(magic, info, bi);
    heap_init(bi.heap_start, bi.heap_size);
    run_global_constructors();
    multiboot2_parse(magic, info, bi);   // constructors may have reset statics

    klog("KleinOS %s.%d booting, cmdline: '%s'", KLEIN_VERSION, KLEIN_BUILD_NUMBER, bi.cmdline);
    klog("memory: %llu MiB usable, heap %llu MiB at %p", (unsigned long long)(bi.mem_total >> 20),
         (unsigned long long)(bi.heap_size >> 20), (void*)bi.heap_start);
    FramebufferInfo& fb = framebuffer();
    klog("framebuffer: %ux%u bpp=%u pitch=%u at %p", fb.width, fb.height, fb.bpp, fb.pitch, fb.address);
    klog("cpu: %s", cpu_brand());
    klog("framebuffer: write-combining %s",
         cpu_framebuffer_wc((uintptr_t)fb.address, (size_t)fb.pitch * fb.height) ? "on" : "unavailable");

    interrupts_init();
    sti();
    rtc_init();
    pci_init();
    ata_init();
    input_init(fb.width, fb.height);

    system_main();
}
