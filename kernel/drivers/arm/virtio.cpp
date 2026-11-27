#include "drivers/arm/virtio.h"
#include "boot/dtb.h"
#include "lib/heap.h"
#include "lib/log.h"
#include "lib/string.h"

namespace virtio {

namespace {

enum Reg : uint32_t {
    MAGIC = 0x000, VERSION = 0x004, DEVICE_ID = 0x008, DEVICE_FEATURES = 0x010, DEVICE_FEATURES_SEL = 0x014,
    DRIVER_FEATURES = 0x020, DRIVER_FEATURES_SEL = 0x024, GUEST_PAGE_SIZE = 0x028, QUEUE_SEL = 0x030,
    QUEUE_NUM_MAX = 0x034, QUEUE_NUM = 0x038, QUEUE_ALIGN = 0x03C, QUEUE_PFN = 0x040, QUEUE_READY = 0x044,
    QUEUE_NOTIFY = 0x050, STATUS = 0x070, QUEUE_DESC_LO = 0x080, QUEUE_DESC_HI = 0x084,
    QUEUE_DRIVER_LO = 0x090, QUEUE_DRIVER_HI = 0x094, QUEUE_DEVICE_LO = 0x0A0, QUEUE_DEVICE_HI = 0x0A4,
};
enum Status : uint32_t { ACK = 1, DRIVER = 2, DRIVER_OK = 4, FEATURES_OK = 8, FAILED = 128 };
constexpr uint16_t kDescNext = 1, kDescWrite = 2;

Device g_devices[32];
int g_count;

void probe(uint64_t base, int irq) {
    if (g_count >= 32) return;
    volatile uint32_t* r = (volatile uint32_t*)base;
    if (r[MAGIC / 4] != 0x74726976) return;                 // "virt"
    uint32_t version = r[VERSION / 4], id = r[DEVICE_ID / 4];
    if ((version != 1 && version != 2) || id == 0) return;  // id 0 = empty slot
    Device& d = g_devices[g_count++];
    d.regs = (volatile uint8_t*)base;
    d.version = version;
    d.id = id;
    d.irq = irq;
}

inline void barrier() { asm volatile("dmb ish" ::: "memory"); }

}  // namespace

int scan() {
    if (g_count) return g_count;
    DtbNode n;
    bool any = false;
    for (int i = 0; dtb_find_compatible("virtio,mmio", i, &n); i++) {
        probe(n.reg_addr(0), n.gic_irq(0));
        any = true;
    }
    if (!any)
        for (int i = 0; i < 32; i++) probe(0x0A000000ull + i * 0x200, 48 + i);   // QEMU virt layout
    klog("virtio: %d device(s)", g_count);
    for (int i = 0; i < g_count; i++)
        klog("virtio:   %p id=%u v%u irq=%d", (void*)g_devices[i].regs, g_devices[i].id, g_devices[i].version, g_devices[i].irq);
    return g_count;
}

Device* find(uint32_t device_id) {
    for (int i = 0; i < g_count; i++)
        if (g_devices[i].id == device_id && !g_devices[i].claimed) return &g_devices[i];
    return nullptr;
}

bool begin(Device& d, uint32_t want) {
    d.write(STATUS, 0);
    d.write(STATUS, ACK);
    d.write(STATUS, ACK | DRIVER);
    d.write(DEVICE_FEATURES_SEL, 0);
    uint32_t offered = d.read(DEVICE_FEATURES);
    d.features = offered & want;
    d.write(DRIVER_FEATURES_SEL, 0);
    d.write(DRIVER_FEATURES, d.features);
    if (d.version == 2) {
        d.write(DEVICE_FEATURES_SEL, 1);
        if (!(d.read(DEVICE_FEATURES) & 1)) { d.write(STATUS, FAILED); return false; }   // no VIRTIO_F_VERSION_1
        d.write(DRIVER_FEATURES_SEL, 1);
        d.write(DRIVER_FEATURES, 1);
        d.write(STATUS, ACK | DRIVER | FEATURES_OK);
        if (!(d.read(STATUS) & FEATURES_OK)) { d.write(STATUS, FAILED); return false; }
    } else {
        d.write(GUEST_PAGE_SIZE, 4096);
    }
    d.claimed = true;
    return true;
}

void driver_ok(Device& d) {
    d.write(STATUS, d.read(STATUS) | DRIVER_OK);
}

bool queue_init(Device& d, Queue& q, int index, uint16_t max_entries) {
    d.write(QUEUE_SEL, index);
    uint32_t max = d.read(QUEUE_NUM_MAX);
    if (!max) return false;
    uint16_t n = max < max_entries ? (uint16_t)max : max_entries;
    while (n & (n - 1)) n &= n - 1;                          // power of two
    if (d.version == 2 && d.read(QUEUE_READY)) return false;

    // desc table, avail ring, then (page aligned) the used ring
    size_t desc_bytes = 16u * n, avail_bytes = 6u + 2u * n;
    size_t used_off = (desc_bytes + avail_bytes + 4095) & ~(size_t)4095;
    size_t total = used_off + 6u + 8u * n;
    uint8_t* raw = (uint8_t*)kcalloc(1, total + 4096);
    if (!raw) return false;
    uint8_t* mem = (uint8_t*)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);

    q.dev = &d;
    q.index = index;
    q.size = n;
    q.desc = (Queue::Desc*)mem;
    q.avail = (volatile uint16_t*)(mem + desc_bytes);
    q.used = mem + used_off;
    q.last_used = 0;
    q.next_free = (uint16_t*)kcalloc(n, sizeof(uint16_t));
    for (uint16_t i = 0; i < n; i++) q.next_free[i] = i + 1;
    q.free_head = 0;
    q.num_free = n;

    d.write(QUEUE_NUM, n);
    if (d.version == 1) {
        d.write(QUEUE_ALIGN, 4096);
        d.write(QUEUE_PFN, (uint32_t)((uintptr_t)mem >> 12));
    } else {
        uint64_t desc = (uint64_t)(uintptr_t)mem, avail = (uint64_t)(uintptr_t)q.avail, used = (uint64_t)(uintptr_t)q.used;
        d.write(QUEUE_DESC_LO, (uint32_t)desc);   d.write(QUEUE_DESC_HI, (uint32_t)(desc >> 32));
        d.write(QUEUE_DRIVER_LO, (uint32_t)avail); d.write(QUEUE_DRIVER_HI, (uint32_t)(avail >> 32));
        d.write(QUEUE_DEVICE_LO, (uint32_t)used);  d.write(QUEUE_DEVICE_HI, (uint32_t)(used >> 32));
        d.write(QUEUE_READY, 1);
    }
    return true;
}

int submit(Queue& q, const Segment* segs, int count) {
    if (count <= 0 || q.num_free < count) return -1;
    uint16_t head = q.free_head, cur = head;
    for (int i = 0; i < count; i++) {
        Queue::Desc& ds = q.desc[cur];
        ds.addr = (uint64_t)(uintptr_t)segs[i].addr;
        ds.len = segs[i].len;
        ds.flags = (segs[i].device_writes ? kDescWrite : 0) | (i + 1 < count ? kDescNext : 0);
        uint16_t next = q.next_free[cur];
        ds.next = i + 1 < count ? next : 0;
        if (i + 1 < count) cur = next;
        else q.free_head = next;
    }
    q.num_free -= count;
    uint16_t idx = q.avail[1];
    ((volatile uint16_t*)q.avail)[2 + (idx % q.size)] = head;
    barrier();
    q.avail[1] = idx + 1;
    barrier();
    notify(q);
    return head;
}

void notify(Queue& q) { q.dev->write(QUEUE_NOTIFY, q.index); }

bool reap(Queue& q, uint16_t* head, uint32_t* written) {
    barrier();
    uint16_t idx = *(volatile uint16_t*)(q.used + 2);
    if (q.last_used == idx) return false;
    volatile uint32_t* e = (volatile uint32_t*)(q.used + 4 + 8 * (q.last_used % q.size));
    *head = (uint16_t)e[0];
    *written = e[1];
    q.last_used++;
    return true;
}

void release(Queue& q, uint16_t head) {
    // walk the chain to its tail, then splice it in front of the free list
    uint16_t cur = head;
    int n = 1;
    while (q.desc[cur].flags & kDescNext) { cur = q.desc[cur].next; n++; }
    q.next_free[cur] = q.free_head;
    q.free_head = head;
    q.num_free += n;
}

}  // namespace virtio
