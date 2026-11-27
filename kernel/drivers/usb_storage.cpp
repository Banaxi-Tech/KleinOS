#include "drivers/usb_storage.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "drivers/pci.h"
#include "lib/log.h"
#include "lib/string.h"

namespace {

// EHCI operational registers (dword index)
enum { USBCMD = 0, USBSTS = 1, USBINTR = 2, CTRLDSSEGMENT = 4, PERIODICBASE = 5, ASYNCLIST = 6, CONFIGFLAG = 16, PORTSC = 17 };
enum : uint32_t {
    PORT_CONNECT = 1u << 0, PORT_CONNECT_CHANGE = 1u << 1, PORT_ENABLE = 1u << 2, PORT_ENABLE_CHANGE = 1u << 3,
    PORT_OC_CHANGE = 1u << 5, PORT_RESET = 1u << 8, PORT_POWER = 1u << 12, PORT_OWNER = 1u << 13,
};
constexpr uint32_t kPortWriteClear = PORT_CONNECT_CHANGE | PORT_ENABLE_CHANGE | PORT_OC_CHANGE;
constexpr uint32_t kTerminate = 1;
constexpr uint32_t kTokenActive = 1u << 7, kTokenHalted = 1u << 6;
constexpr uint32_t kToggle = 1u << 31;
enum { PID_OUT = 0, PID_IN = 1, PID_SETUP = 2 };
constexpr int kMaxTds = 32;
constexpr uint32_t kTdBytes = 16384;
constexpr uint32_t kChunkSectors = 128;
constexpr uint64_t kTimeoutMs = 8000;

struct Td {
    volatile uint32_t next, alt, token, buf[5], buf_hi[5];
    uint32_t pad[3];
};
struct Qh {
    volatile uint32_t link, ep1, ep2, current;
    volatile uint32_t next, alt, token, buf[5], buf_hi[5];
    uint32_t pad[15];
};
static_assert(sizeof(Td) == 64 && sizeof(Qh) == 128);

struct Dma {
    Qh control, bulk_in, bulk_out;
    Td td[kMaxTds];
    uint8_t setup[8];
    uint8_t cbw[32];
    uint8_t csw[16];
    uint8_t data[512];
};
alignas(4096) Dma g_dma;
static_assert(sizeof(Dma) <= 4096);

volatile uint32_t* g_ops;
int g_ports;
bool g_ready;
uint8_t g_addr, g_ep_in, g_ep_out;
uint16_t g_mps_in, g_mps_out;
uint32_t g_tag = 1;
uint64_t g_sectors;

uint32_t phys(const volatile void* p) { return (uint32_t)(uintptr_t)p; }
void barrier() { __sync_synchronize(); }
void delay(uint64_t ms) {
    uint64_t end = timer_ms() + ms;
    while (timer_ms() < end) cpu_relax();
}

void set_qh(Qh& q, uint8_t addr, uint8_t ep, uint16_t mps, bool control, bool head) {
    q.ep1 = (uint32_t)addr | ((uint32_t)ep << 8) | (2u << 12) | (control ? 1u << 14 : 0) | (head ? 1u << 15 : 0) | ((uint32_t)mps << 16);
    q.ep2 = 1u << 30;
}

void reset_overlay(Qh& q) {
    q.current = 0;
    q.next = kTerminate;
    q.alt = kTerminate;
    q.token = 0;
}

void fill_td(int i, int pid, bool toggle, const void* buf, uint32_t len, bool last) {
    Td& t = g_dma.td[i];
    t.next = last ? kTerminate : phys(&g_dma.td[i + 1]);
    t.alt = kTerminate;
    uint32_t a = phys(buf);
    t.buf[0] = a;
    for (int k = 1; k < 5; k++) t.buf[k] = (a & ~0xFFFu) + (uint32_t)k * 0x1000u;
    for (int k = 0; k < 5; k++) t.buf_hi[k] = 0;
    t.token = (toggle ? kToggle : 0) | (len << 16) | (3u << 10) | ((uint32_t)pid << 8) | kTokenActive;
}

// Runs tds 0..n-1 on q. Returns bytes moved, or -1 if the endpoint halted or timed out.
long run(Qh& q, int n, bool* stalled = nullptr) {
    if (stalled) *stalled = false;
    barrier();
    q.current = 0;
    q.alt = kTerminate;
    q.token &= kToggle;        // keep the data toggle, clear halted
    barrier();
    q.next = phys(&g_dma.td[0]);
    barrier();
    uint64_t end = timer_ms() + kTimeoutMs;
    for (;;) {
        bool pending = false, done_short = false;
        for (int i = 0; i < n && !pending && !done_short; i++) {
            uint32_t tok = g_dma.td[i].token;
            if (tok & kTokenActive) { pending = true; break; }
            if (tok & kTokenHalted) {
                if (stalled) *stalled = !(tok & 0x78);   // no transaction error bits: a real STALL
                q.next = kTerminate;
                return -1;
            }
            if ((tok >> 16) & 0x7FFF) done_short = true;
        }
        if (!pending) break;
        if (timer_ms() > end) {
            klog("usb: transfer timed out");
            q.next = kTerminate;
            return -1;
        }
        cpu_relax();
    }
    return 0;
}

// Bytes actually moved by a finished td list of the given requested lengths.
long td_remaining(int n) {
    long left = 0;
    for (int i = 0; i < n; i++) left += (g_dma.td[i].token >> 16) & 0x7FFF;
    return left;
}

long control(uint8_t addr, uint8_t rt, uint8_t req, uint16_t value, uint16_t index, void* data, uint16_t len) {
    set_qh(g_dma.control, addr, 0, 64, true, true);
    uint8_t* s = g_dma.setup;
    s[0] = rt; s[1] = req; s[2] = (uint8_t)value; s[3] = (uint8_t)(value >> 8);
    s[4] = (uint8_t)index; s[5] = (uint8_t)(index >> 8); s[6] = (uint8_t)len; s[7] = (uint8_t)(len >> 8);
    bool in = rt & 0x80;
    int n = 0;
    fill_td(n++, PID_SETUP, false, s, 8, false);
    if (len) fill_td(n++, in ? PID_IN : PID_OUT, true, data, len, false);
    fill_td(n, in && len ? PID_OUT : PID_IN, true, nullptr, 0, true);
    n++;
    if (run(g_dma.control, n) < 0) return -1;
    return len - td_remaining(n);
}

bool clear_halt(uint8_t ep, Qh& q) {
    bool ok = control(g_addr, 0x02, 1, 0, ep, nullptr, 0) >= 0;
    q.token = 0;               // endpoint restarts at DATA0
    return ok;
}

long bulk(bool in, void* buf, uint32_t len, bool* stalled) {
    Qh& q = in ? g_dma.bulk_in : g_dma.bulk_out;
    int n = 0;
    uint8_t* p = (uint8_t*)buf;
    uint32_t left = len;
    do {
        uint32_t part = left < kTdBytes ? left : kTdBytes;
        if (n >= kMaxTds) return -1;
        fill_td(n, in ? PID_IN : PID_OUT, false, p, part, left == part);
        n++;
        p += part;
        left -= part;
    } while (left);
    if (run(q, n, stalled) < 0) return -1;
    return (long)len - td_remaining(n);
}

void recover() {
    control(g_addr, 0x21, 0xFF, 0, 0, nullptr, 0);   // bulk-only mass storage reset
    delay(50);
    clear_halt(g_ep_in, g_dma.bulk_in);
    clear_halt(g_ep_out, g_dma.bulk_out);
}

// One SCSI command; true when the device reports success.
bool scsi_once(const uint8_t* cdb, int cdb_len, bool in, void* buf, uint32_t len) {
    uint8_t* c = g_dma.cbw;
    memset(c, 0, 31);
    memcpy(c, "USBC", 4);
    uint32_t tag = g_tag++;
    memcpy(c + 4, &tag, 4);
    memcpy(c + 8, &len, 4);
    c[12] = in ? 0x80 : 0;
    c[14] = (uint8_t)cdb_len;
    memcpy(c + 15, cdb, (size_t)cdb_len);
    bool stalled;
    if (bulk(false, c, 31, &stalled) != 31) return false;
    if (len) {
        long got = bulk(in, buf, len, &stalled);
        if (got < 0) {
            if (!stalled) return false;
            clear_halt(in ? g_ep_in : g_ep_out, in ? g_dma.bulk_in : g_dma.bulk_out);
        }
    }
    long r = bulk(true, g_dma.csw, 13, &stalled);
    if (r < 0 && stalled) {
        clear_halt(g_ep_in, g_dma.bulk_in);
        r = bulk(true, g_dma.csw, 13, &stalled);
    }
    if (r != 13 || memcmp(g_dma.csw, "USBS", 4) || memcmp(g_dma.csw + 4, &tag, 4)) return false;
    return g_dma.csw[12] == 0;
}

bool scsi(const uint8_t* cdb, int cdb_len, bool in, void* buf, uint32_t len) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (scsi_once(cdb, cdb_len, in, buf, len)) return true;
        recover();
    }
    return false;
}

bool wait_ready() {
    for (int i = 0; i < 40; i++) {
        uint8_t tur[6] = {0x00};
        if (scsi_once(tur, 6, true, nullptr, 0)) return true;
        uint8_t sense[6] = {0x03, 0, 0, 0, 18, 0};
        scsi_once(sense, 6, true, g_dma.data, 18);
        delay(100);
    }
    return false;
}

bool rw10(uint8_t op, bool in, uint64_t lba, uint32_t count, void* buf) {
    uint8_t cdb[10] = {op, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba, 0,
                       (uint8_t)(count >> 8), (uint8_t)count, 0};
    return scsi(cdb, 10, in, buf, count * 512);
}

// Parses the configuration for a bulk-only SCSI interface; configures the device.
bool configure(uint8_t addr) {
    uint8_t* d = g_dma.data;
    if (control(addr, 0x80, 6, 0x0200, 0, d, 9) < 9) return false;
    uint16_t total = (uint16_t)(d[2] | (d[3] << 8));
    if (total > sizeof(g_dma.data)) total = sizeof(g_dma.data);
    uint8_t config = d[5];
    if (control(addr, 0x80, 6, 0x0200, 0, d, total) < total) return false;
    g_ep_in = g_ep_out = 0;
    bool storage = false;
    for (uint32_t i = 0; i + 2 <= total && d[i];) {
        if (d[i + 1] == 4 && i + 8 <= total) {
            if (storage && g_ep_in && g_ep_out) break;
            storage = d[i + 5] == 8 && d[i + 6] == 6 && d[i + 7] == 0x50;
        } else if (d[i + 1] == 5 && storage && i + 7 <= total && (d[i + 3] & 3) == 2) {
            uint16_t mps = (uint16_t)(d[i + 4] | ((d[i + 5] & 7) << 8));
            if (d[i + 2] & 0x80) { g_ep_in = d[i + 2]; g_mps_in = mps; }
            else { g_ep_out = d[i + 2]; g_mps_out = mps; }
        }
        i += d[i];
    }
    if (!storage || !g_ep_in || !g_ep_out) return false;
    if (control(addr, 0x00, 9, config, 0, nullptr, 0) < 0) return false;
    delay(20);
    g_addr = addr;
    set_qh(g_dma.bulk_in, addr, g_ep_in & 15, g_mps_in, false, false);
    set_qh(g_dma.bulk_out, addr, g_ep_out & 15, g_mps_out, false, false);
    reset_overlay(g_dma.bulk_in);
    reset_overlay(g_dma.bulk_out);
    return true;
}

// The stick we booted from has a FAT32 first partition.
bool looks_like_system() {
    uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    if (!scsi(inq, 6, true, g_dma.data, 36)) return false;
    if (!wait_ready()) return false;
    uint8_t cap[10] = {0x25};
    if (!scsi(cap, 10, true, g_dma.data, 8)) return false;
    const uint8_t* c = g_dma.data;
    uint32_t last = ((uint32_t)c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
    uint32_t block = ((uint32_t)c[4] << 24) | (c[5] << 16) | (c[6] << 8) | c[7];
    if (block != 512) { klog("usb: %u-byte sectors are not supported", block); return false; }
    g_sectors = (uint64_t)last + 1;
    if (!rw10(0x28, true, 0, 1, g_dma.data)) return false;
    return c[510] == 0x55 && c[511] == 0xAA && (c[446 + 4] == 0x0C || c[446 + 4] == 0x0B);
}

bool try_port(int port, uint8_t addr) {
    volatile uint32_t& sc = g_ops[PORTSC + port];
    if (!(sc & PORT_CONNECT)) return false;
    sc = (sc & ~(kPortWriteClear | PORT_ENABLE)) | PORT_RESET;
    delay(55);
    sc = sc & ~(kPortWriteClear | PORT_ENABLE | PORT_RESET);
    uint64_t end = timer_ms() + 50;
    while ((sc & PORT_RESET) && timer_ms() < end) {}
    delay(20);
    if (!(sc & PORT_ENABLE)) return false;     // not high speed
    sc = (sc & ~kPortWriteClear) | kPortWriteClear;

    uint8_t* d = g_dma.data;
    if (control(0, 0x80, 6, 0x0100, 0, d, 8) < 8) return false;
    delay(2);
    if (control(0, 0x00, 5, addr, 0, nullptr, 0) < 0) return false;
    delay(10);
    return configure(addr) && looks_like_system();
}

void take_from_bios(const PciDevice* p, uint32_t hccparams) {
    uint8_t off = (hccparams >> 8) & 0xFF;
    int guard = 32;
    while (off >= 0x40 && guard--) {
        uint32_t cap = pci_read32(p->bus, p->slot, p->func, off);
        if ((cap & 0xFF) == 1) {
            pci_write32(p->bus, p->slot, p->func, off, cap | (1u << 24));
            uint64_t end = timer_ms() + 1000;
            while ((pci_read32(p->bus, p->slot, p->func, off) & (1u << 16)) && timer_ms() < end) {}
            pci_write32(p->bus, p->slot, p->func, off + 4, 0);
            return;
        }
        off = (cap >> 8) & 0xFF;
    }
}

bool start_controller(const PciDevice* p) {
    pci_enable(p);
    uintptr_t base = p->bar[0] & ~0xFu;
    if (!base) return false;
    volatile uint8_t* mmio = (volatile uint8_t*)base;
    volatile uint32_t* caps = (volatile uint32_t*)mmio;
    uint32_t hcs = caps[1], hcc = caps[2];
    g_ops = (volatile uint32_t*)(mmio + mmio[0]);
    g_ports = hcs & 15;
    take_from_bios(p, hcc);

    g_ops[USBCMD] = g_ops[USBCMD] & ~1u;
    uint64_t end = timer_ms() + 100;
    while (!(g_ops[USBSTS] & (1u << 12)) && timer_ms() < end) {}
    g_ops[USBCMD] = 2;
    end = timer_ms() + 250;
    while ((g_ops[USBCMD] & 2) && timer_ms() < end) {}
    if (g_ops[USBCMD] & 2) return false;

    Qh* qs[3] = {&g_dma.control, &g_dma.bulk_in, &g_dma.bulk_out};
    for (int i = 0; i < 3; i++) {
        qs[i]->link = phys(qs[(i + 1) % 3]) | 2;
        reset_overlay(*qs[i]);
    }
    set_qh(g_dma.control, 0, 0, 64, true, true);
    // bulk queue heads sit idle until a device is configured
    set_qh(g_dma.bulk_in, 127, 1, 512, false, false);
    set_qh(g_dma.bulk_out, 127, 1, 512, false, false);
    g_ops[CTRLDSSEGMENT] = 0;
    g_ops[USBINTR] = 0;
    g_ops[PERIODICBASE] = 0;
    g_ops[ASYNCLIST] = phys(&g_dma.control);
    barrier();
    g_ops[USBCMD] = (8u << 16) | (1u << 5) | 1u;   // interrupt threshold, async schedule, run
    g_ops[CONFIGFLAG] = 1;
    delay(10);
    if (hcs & (1u << 4))
        for (int i = 0; i < g_ports; i++) g_ops[PORTSC + i] = (g_ops[PORTSC + i] & ~kPortWriteClear) | PORT_POWER;
    return true;
}

}  // namespace

void usb_storage_init() {
    for (int i = 0; i < pci_count() && !g_ready; i++) {
        const PciDevice* p = pci_get(i);
        if (p->class_code != 0x0C || p->subclass != 0x03 || p->prog_if != 0x20) continue;
        if (!start_controller(p)) { klog("usb: EHCI controller did not start"); continue; }
        klog("usb: EHCI %04x:%04x, %d ports", p->vendor, p->device, g_ports);
        // sticks reconnect after the controller reset; give them a moment
        uint8_t addr = 1;
        for (int attempt = 0; attempt < 8 && !g_ready; attempt++) {
            delay(250);
            for (int port = 0; port < g_ports && !g_ready; port++) {
                if (try_port(port, addr)) g_ready = true;
                else if (g_ops[PORTSC + port] & PORT_ENABLE) addr = addr < 126 ? addr + 1 : 1;
            }
        }
    }
    if (g_ready) klog("usb: system drive found, %u MiB", (unsigned)(g_sectors >> 11));
    else klog("usb: no KleinOS drive found");
}

bool usb_storage_ready() { return g_ready; }

bool usb_storage_read(uint64_t lba, uint32_t count, void* buf) {
    if (!g_ready || lba + count > g_sectors) return false;
    uint8_t* p = (uint8_t*)buf;
    while (count) {
        uint32_t n = count < kChunkSectors ? count : kChunkSectors;
        if (!rw10(0x28, true, lba, n, p)) return false;
        lba += n; p += n * 512u; count -= n;
    }
    return true;
}

bool usb_storage_write(uint64_t lba, uint32_t count, const void* buf) {
    if (!g_ready || lba + count > g_sectors) return false;
    uint8_t* p = (uint8_t*)buf;
    while (count) {
        uint32_t n = count < kChunkSectors ? count : kChunkSectors;
        if (!rw10(0x2A, false, lba, n, p)) return false;
        lba += n; p += n * 512u; count -= n;
    }
    return true;
}

bool usb_storage_flush() {
    if (!g_ready) return false;
    uint8_t cdb[10] = {0x35};
    scsi_once(cdb, 10, true, nullptr, 0);      // many sticks don't implement it
    return true;
}
