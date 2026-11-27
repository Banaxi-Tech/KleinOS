// virtio-blk, exposed through the same ata_* interface the x86 build uses for IDE disks (the FAT32 driver,
// installer and system settings all talk to disks through it). Up to four disks, polled, 512-byte sectors.
#include "drivers/ata.h"
#include "drivers/usb_storage.h"
#include "drivers/arm/virtio.h"
#include "lib/heap.h"
#include "lib/log.h"
#include "lib/string.h"

namespace {

constexpr uint32_t kFeatFlush = 1u << 9;
constexpr uint32_t kTypeIn = 0, kTypeOut = 1, kTypeFlush = 4;
constexpr uint32_t kMaxChunk = 256;     // sectors per request

struct [[gnu::packed]] Request { uint32_t type, reserved; uint64_t sector; };

struct Disk {
    virtio::Device* dev = nullptr;
    virtio::Queue queue;
    bool flush = false;
};

AtaDrive g_drives[4];
Disk g_disks[4];
int g_count;

bool transfer(Disk& d, uint32_t type, uint64_t lba, uint32_t sectors, void* buf) {
    alignas(16) static Request req;
    alignas(16) static volatile uint8_t status;
    req = {type, 0, lba};
    status = 0xFF;
    virtio::Segment segs[3] = {
        {&req, sizeof(req), false},
        {buf, sectors * 512, type == kTypeIn},
        {(const void*)&status, 1, true},
    };
    int n = 3;
    if (type == kTypeFlush) { segs[1] = segs[2]; n = 2; }
    int head = virtio::submit(d.queue, segs, n);
    if (head < 0) return false;
    uint16_t done;
    uint32_t len;
    for (uint64_t spin = 0; spin < 200000000ull; spin++) {
        if (virtio::reap(d.queue, &done, &len)) {
            virtio::release(d.queue, done);
            return status == 0;
        }
        asm volatile("yield");
    }
    klog("virtio-blk: request timed out (lba %llu)", (unsigned long long)lba);
    return false;
}

}  // namespace

void ata_init() {
    virtio::scan();
    while (g_count < 4) {
        virtio::Device* dev = virtio::find(virtio::Block);
        if (!dev) break;
        Disk& d = g_disks[g_count];
        if (!virtio::begin(*dev, kFeatFlush) || !virtio::queue_init(*dev, d.queue, 0, 32)) continue;
        virtio::driver_ok(*dev);
        d.dev = dev;
        d.flush = dev->features & kFeatFlush;
        AtaDrive& a = g_drives[g_count];
        a.present = true;
        a.atapi = false;
        a.sectors = dev->cfg64(0);
        strcpy(a.model, "virtio block device");
        snprintf(a.serial, sizeof(a.serial), "virtio%d", g_count);
        klog("virtio-blk: disk %d, %llu MiB", g_count, (unsigned long long)(a.sectors / 2048));
        g_count++;
    }
}

int ata_drive_count() { return 4; }
AtaDrive* ata_drive(int i) { return i >= 0 && i < 4 ? &g_drives[i] : nullptr; }

static bool rw(int drive, uint64_t lba, uint32_t count, void* buf, bool write) {
    if (drive == kUsbDrive) return write ? usb_storage_write(lba, count, buf) : usb_storage_read(lba, count, buf);
    if (drive < 0 || drive >= g_count || !g_drives[drive].present) return false;
    if (lba + count > g_drives[drive].sectors) return false;
    uint8_t* p = (uint8_t*)buf;
    while (count) {
        uint32_t n = count < kMaxChunk ? count : kMaxChunk;
        if (!transfer(g_disks[drive], write ? kTypeOut : kTypeIn, lba, n, p)) return false;
        lba += n; count -= n; p += n * 512;
    }
    return true;
}

bool ata_read(int drive, uint64_t lba, uint32_t count, void* buf) { return rw(drive, lba, count, buf, false); }
bool ata_write(int drive, uint64_t lba, uint32_t count, const void* buf) { return rw(drive, lba, count, (void*)buf, true); }

bool ata_flush(int drive) {
    if (drive == kUsbDrive) return usb_storage_flush();
    if (drive < 0 || drive >= g_count) return false;
    if (!g_disks[drive].flush) return true;
    return transfer(g_disks[drive], kTypeFlush, 0, 0, nullptr);
}
