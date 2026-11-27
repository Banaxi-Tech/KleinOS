#pragma once
// virtio over MMIO (legacy v1 and modern v2 transports) with split virtqueues. Used on the ARM boards, where
// there is no PCI bus to find devices on: QEMU's "virt" machine exposes block, network, input and GPU
// devices this way. All queues are polled (no interrupts needed).
#include <stdint.h>
#include <stddef.h>

namespace virtio {

enum DeviceId : uint32_t { Net = 1, Block = 2, Input = 18 };

struct Device {
    volatile uint8_t* regs = nullptr;
    uint32_t version = 0;               // 1 = legacy, 2 = modern
    uint32_t id = 0;
    uint32_t features = 0;              // negotiated, low 32 bits
    int irq = -1;
    bool claimed = false;

    uint32_t read(uint32_t off) const { return *(volatile uint32_t*)(regs + off); }
    void write(uint32_t off, uint32_t v) const { *(volatile uint32_t*)(regs + off) = v; }
    // device-specific configuration space
    uint8_t cfg8(uint32_t off) const { return *(volatile uint8_t*)(regs + 0x100 + off); }
    uint16_t cfg16(uint32_t off) const { return *(volatile uint16_t*)(regs + 0x100 + off); }
    uint32_t cfg32(uint32_t off) const { return *(volatile uint32_t*)(regs + 0x100 + off); }
    uint64_t cfg64(uint32_t off) const { return cfg32(off) | ((uint64_t)cfg32(off + 4) << 32); }
    void cfg_write8(uint32_t off, uint8_t v) const { *(volatile uint8_t*)(regs + 0x100 + off) = v; }
};

struct Segment {
    const void* addr;
    uint32_t len;
    bool device_writes;                 // buffer is filled by the device
};

struct Queue {
    Device* dev = nullptr;
    int index = 0;
    uint16_t size = 0;
    struct Desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
    Desc* desc = nullptr;
    volatile uint16_t* avail = nullptr;     // flags, idx, ring[]
    volatile uint8_t* used = nullptr;       // flags, idx, ring[] {id, len}
    uint16_t free_head = 0, num_free = 0, last_used = 0;
    uint16_t* next_free = nullptr;
};

// Enumerate transports from the device tree (falling back to the QEMU virt addresses).
int scan();
// First unclaimed device with this id, or nullptr.
Device* find(uint32_t device_id);

// Reset + negotiate. `want` are the low 32 feature bits the driver can use; the modern transport always
// also negotiates VIRTIO_F_VERSION_1. Returns false if the device refuses.
bool begin(Device& d, uint32_t want);
bool queue_init(Device& d, Queue& q, int index, uint16_t max_entries);
void driver_ok(Device& d);

// Queue a buffer chain and kick the device; returns the head descriptor (or -1 when full).
int submit(Queue& q, const Segment* segs, int count);
// Pop one finished chain: its head descriptor and the number of bytes the device wrote.
bool reap(Queue& q, uint16_t* head, uint32_t* written);
// Return a chain's descriptors to the free list.
void release(Queue& q, uint16_t head);
void notify(Queue& q);

}  // namespace virtio
