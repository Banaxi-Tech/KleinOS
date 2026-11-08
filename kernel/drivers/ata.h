#pragma once
// ATA PIO driver (legacy IDE ports, LBA28/LBA48). Up to 4 drives.
#include <stdint.h>
#include <stddef.h>

struct AtaDrive {
    bool present;
    bool atapi;
    uint16_t io_base, ctrl_base;
    bool slave;
    uint64_t sectors;
    char model[41];
    char serial[21];
};

void ata_init();
int ata_drive_count();                 // always 4 slots; check present
AtaDrive* ata_drive(int index);        // 0 = primary master ... 3 = secondary slave
bool ata_read(int drive, uint64_t lba, uint32_t count, void* buf);
bool ata_write(int drive, uint64_t lba, uint32_t count, const void* buf);
bool ata_flush(int drive);
