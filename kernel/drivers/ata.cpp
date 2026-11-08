#include "drivers/ata.h"
#include "arch/cpu.h"
#include "lib/log.h"
#include "lib/string.h"

static AtaDrive g_drives[4];

enum : uint8_t {
    ST_ERR = 0x01, ST_DRQ = 0x08, ST_DF = 0x20, ST_BSY = 0x80,
    CMD_READ_PIO = 0x20, CMD_READ_PIO_EXT = 0x24, CMD_WRITE_PIO = 0x30, CMD_WRITE_PIO_EXT = 0x34,
    CMD_FLUSH = 0xE7, CMD_FLUSH_EXT = 0xEA, CMD_IDENTIFY = 0xEC,
};

static bool wait_ready(AtaDrive& d) {
    for (int i = 0; i < 4000000; i++) {
        uint8_t s = inb(d.io_base + 7);
        if (s & ST_BSY) continue;
        if (s & (ST_ERR | ST_DF)) return false;
        return true;
    }
    return false;
}

static bool wait_drq(AtaDrive& d) {
    for (int i = 0; i < 4000000; i++) {
        uint8_t s = inb(d.io_base + 7);
        if (s & ST_BSY) continue;
        if (s & (ST_ERR | ST_DF)) return false;
        if (s & ST_DRQ) return true;
    }
    return false;
}

static void delay400(AtaDrive& d) { for (int i = 0; i < 4; i++) inb(d.ctrl_base); }

static void identify(int idx, uint16_t io, uint16_t ctrl, bool slave) {
    AtaDrive& d = g_drives[idx];
    d = AtaDrive{};
    d.io_base = io;
    d.ctrl_base = ctrl;
    d.slave = slave;
    if (inb(io + 7) == 0xFF) return;   // floating bus
    outb(ctrl, 0x02);                  // nIEN: no interrupts, we poll
    outb(io + 6, slave ? 0xB0 : 0xA0);
    delay400(d);
    outb(io + 2, 0); outb(io + 3, 0); outb(io + 4, 0); outb(io + 5, 0);
    outb(io + 7, CMD_IDENTIFY);
    if (inb(io + 7) == 0) return;
    for (int i = 0; i < 1000000 && (inb(io + 7) & ST_BSY); i++) {}
    uint8_t lm = inb(io + 4), hm = inb(io + 5);
    if (lm == 0x14 && hm == 0xEB) { d.atapi = true; d.present = true; strcpy(d.model, "CD/DVD drive"); return; }
    if (lm || hm) return;
    if (!wait_drq(d)) return;
    uint16_t id[256];
    insw(io, id, 256);
    d.present = true;
    for (int i = 0; i < 20; i++) { d.model[i * 2] = id[27 + i] >> 8; d.model[i * 2 + 1] = id[27 + i] & 0xFF; }
    d.model[40] = 0;
    for (int i = 0; i < 10; i++) { d.serial[i * 2] = id[10 + i] >> 8; d.serial[i * 2 + 1] = id[10 + i] & 0xFF; }
    d.serial[20] = 0;
    for (int i = 39; i >= 0 && d.model[i] == ' '; i--) d.model[i] = 0;
    for (int i = 19; i >= 0 && d.serial[i] == ' '; i--) d.serial[i] = 0;
    if (id[83] & (1 << 10))
        d.sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    else
        d.sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    klog("ata%d: %s, %llu MiB", idx, d.model, (unsigned long long)(d.sectors / 2048));
}

void ata_init() {
    identify(0, 0x1F0, 0x3F6, false);
    identify(1, 0x1F0, 0x3F6, true);
    identify(2, 0x170, 0x376, false);
    identify(3, 0x170, 0x376, true);
}

int ata_drive_count() { return 4; }
AtaDrive* ata_drive(int i) { return (i >= 0 && i < 4) ? &g_drives[i] : nullptr; }

static bool setup(AtaDrive& d, uint64_t lba, uint32_t count, bool ext) {
    if (!wait_ready(d)) return false;
    if (ext) {
        outb(d.io_base + 6, d.slave ? 0x50 : 0x40);
        outb(d.io_base + 2, (count >> 8) & 0xFF);
        outb(d.io_base + 3, (lba >> 24) & 0xFF);
        outb(d.io_base + 4, (lba >> 32) & 0xFF);
        outb(d.io_base + 5, (lba >> 40) & 0xFF);
        outb(d.io_base + 2, count & 0xFF);
        outb(d.io_base + 3, lba & 0xFF);
        outb(d.io_base + 4, (lba >> 8) & 0xFF);
        outb(d.io_base + 5, (lba >> 16) & 0xFF);
    } else {
        outb(d.io_base + 6, (d.slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
        outb(d.io_base + 2, count & 0xFF);
        outb(d.io_base + 3, lba & 0xFF);
        outb(d.io_base + 4, (lba >> 8) & 0xFF);
        outb(d.io_base + 5, (lba >> 16) & 0xFF);
    }
    return true;
}

bool ata_read(int drive, uint64_t lba, uint32_t count, void* buf) {
    AtaDrive* d = ata_drive(drive);
    if (!d || !d->present || d->atapi) return false;
    uint8_t* p = (uint8_t*)buf;
    while (count) {
        uint32_t n = count > 128 ? 128 : count;
        bool ext = lba + n > 0x0FFFFFFF;
        if (!setup(*d, lba, n, ext)) return false;
        outb(d->io_base + 7, ext ? CMD_READ_PIO_EXT : CMD_READ_PIO);
        for (uint32_t i = 0; i < n; i++) {
            delay400(*d);
            if (!wait_drq(*d)) return false;
            insw(d->io_base, p, 256);
            p += 512;
        }
        lba += n;
        count -= n;
    }
    return true;
}

bool ata_write(int drive, uint64_t lba, uint32_t count, const void* buf) {
    AtaDrive* d = ata_drive(drive);
    if (!d || !d->present || d->atapi) return false;
    const uint8_t* p = (const uint8_t*)buf;
    while (count) {
        uint32_t n = count > 128 ? 128 : count;
        bool ext = lba + n > 0x0FFFFFFF;
        if (!setup(*d, lba, n, ext)) return false;
        outb(d->io_base + 7, ext ? CMD_WRITE_PIO_EXT : CMD_WRITE_PIO);
        for (uint32_t i = 0; i < n; i++) {
            delay400(*d);
            if (!wait_drq(*d)) return false;
            outsw(d->io_base, p, 256);
            p += 512;
        }
        lba += n;
        count -= n;
    }
    return ata_flush(drive);
}

bool ata_flush(int drive) {
    AtaDrive* d = ata_drive(drive);
    if (!d || !d->present) return false;
    if (!wait_ready(*d)) return false;
    outb(d->io_base + 6, d->slave ? 0xB0 : 0xA0);
    outb(d->io_base + 7, d->sectors > 0x0FFFFFFF ? CMD_FLUSH_EXT : CMD_FLUSH);
    return wait_ready(*d);
}
