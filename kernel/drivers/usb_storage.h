#pragma once
// USB flash drives through EHCI (bulk-only SCSI, polled). Only started when
// the system itself runs from USB. Other code reaches it as drive kUsbDrive
// through the ata_* functions.
#include <stdint.h>

constexpr int kUsbDrive = 4;

void usb_storage_init();
bool usb_storage_ready();
bool usb_storage_read(uint64_t lba, uint32_t count, void* buf);
bool usb_storage_write(uint64_t lba, uint32_t count, const void* buf);
bool usb_storage_flush();
