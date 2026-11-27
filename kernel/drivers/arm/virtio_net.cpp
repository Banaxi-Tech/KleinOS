// virtio-net, registered with the network stack as an external NIC (the same hook .dri drivers use).
#include "drivers/arm/virtio.h"
#include "net/net.h"
#include "lib/heap.h"
#include "lib/log.h"
#include "lib/string.h"

void virtio_net_init();

namespace {

constexpr uint32_t kFeatMac = 1u << 5, kFeatStatus = 1u << 16;
constexpr int kRxBuffers = 32, kTxBuffers = 16;
constexpr size_t kBufSize = 2048;

virtio::Device* g_dev;
virtio::Queue g_rx, g_tx;
uint8_t* g_rx_mem;                  // kRxBuffers x kBufSize
uint8_t* g_tx_mem;                  // kTxBuffers x kBufSize
int16_t g_rx_slot[256], g_tx_slot[256];   // descriptor head -> buffer slot
bool g_tx_busy[kTxBuffers];
size_t g_hdr;                       // virtio_net_hdr: 10 bytes (legacy) or 12 (modern, with num_buffers)
net::NicDriver g_nic;

void rx_post(int slot) {
    virtio::Segment s{g_rx_mem + (size_t)slot * kBufSize, (uint32_t)kBufSize, true};
    int head = virtio::submit(g_rx, &s, 1);
    if (head >= 0) g_rx_slot[head] = (int16_t)slot;
}

void reclaim_tx() {
    uint16_t head;
    uint32_t len;
    while (virtio::reap(g_tx, &head, &len)) {
        g_tx_busy[g_tx_slot[head]] = false;
        virtio::release(g_tx, head);
    }
}

void send(const uint8_t* frame, size_t len) {
    if (len + g_hdr > kBufSize) return;
    reclaim_tx();
    int slot = -1;
    for (int i = 0; i < kTxBuffers; i++) if (!g_tx_busy[i]) { slot = i; break; }
    if (slot < 0) return;                          // out of buffers: drop, TCP retransmits
    uint8_t* b = g_tx_mem + (size_t)slot * kBufSize;
    memset(b, 0, g_hdr);
    memcpy(b + g_hdr, frame, len);
    virtio::Segment s{b, (uint32_t)(g_hdr + len), false};
    int head = virtio::submit(g_tx, &s, 1);
    if (head < 0) return;
    g_tx_slot[head] = (int16_t)slot;
    g_tx_busy[slot] = true;
}

bool link_up() {
    if (!(g_dev->features & kFeatStatus)) return true;
    return g_dev->cfg16(6) & 1;
}

void poll(void (*deliver)(const uint8_t* frame, size_t len)) {
    reclaim_tx();
    uint16_t head;
    uint32_t written;
    while (virtio::reap(g_rx, &head, &written)) {
        int slot = g_rx_slot[head];
        uint8_t* b = g_rx_mem + (size_t)slot * kBufSize;
        if (written > g_hdr) deliver(b + g_hdr, written - g_hdr);
        virtio::release(g_rx, head);
        rx_post(slot);
    }
}

}  // namespace

void virtio_net_init() {
    virtio::scan();
    virtio::Device* dev = virtio::find(virtio::Net);
    if (!dev) { klog("virtio-net: no device"); return; }
    if (!virtio::begin(*dev, kFeatMac | kFeatStatus)) return;
    if (!virtio::queue_init(*dev, g_rx, 0, 64) || !virtio::queue_init(*dev, g_tx, 1, 32)) return;
    g_dev = dev;
    g_hdr = dev->version == 2 ? 12 : 10;
    g_rx_mem = (uint8_t*)kmalloc(kRxBuffers * kBufSize);
    g_tx_mem = (uint8_t*)kmalloc(kTxBuffers * kBufSize);
    for (int i = 0; i < kRxBuffers; i++) rx_post(i);
    virtio::driver_ok(*dev);

    g_nic.name = "virtio network device";
    if (dev->features & kFeatMac) for (int i = 0; i < 6; i++) g_nic.mac[i] = dev->cfg8(i);
    else { uint8_t def[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56}; memcpy(g_nic.mac, def, 6); }
    g_nic.send = send;
    g_nic.link_up = link_up;
    g_nic.poll = poll;
    net::register_nic(&g_nic);
}
