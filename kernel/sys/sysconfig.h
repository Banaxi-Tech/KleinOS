#pragma once
// Persistent system configuration + user data on the install disk.
//
// Disk layout (written by Setup, see Makefile "system.img"):
//   MBR + GRUB core, partition 1 FAT16 (kernel, initrd), partition 2 type 0x7f:
//     sector 0      : SysConfig header (this struct)
//     sector 8..    : serialized /Users tree (vfs::serialize), up to the end
#include <stdint.h>
#include <stddef.h>

struct UserAccount {
    char full_name[64];
    char login[32];
    char password_hash[65];   // hex SHA-256 of (login + ":" + password), empty = no password
    uint8_t avatar_color;     // index into a palette
    uint8_t is_admin;
    uint8_t pad[6];
};

struct SysConfig {
    char magic[8];            // "KLEINCFG"
    uint32_t version;
    uint32_t setup_complete;  // OOBE finished
    char product_key[32];     // as typed, normalized "XXXXX-..." or empty
    uint32_t edition;         // productkey::Edition
    uint32_t activated;
    uint64_t install_time;    // unix
    char computer_name[32];
    char language[16];        // "en-US"
    uint32_t keyboard_layout; // KeyboardLayout
    int32_t utc_offset_min;
    uint32_t dark_mode;
    uint32_t accent;          // Color
    uint32_t wallpaper;
    uint32_t user_count;
    UserAccount users[4];
    uint32_t data_bytes;      // size of serialized /Users blob
    uint32_t data_checksum;
};

// Probe drives for an installed KleinOS data partition. Returns false if none.
bool sysconfig_load();
bool sysconfig_present();          // a data partition was found
SysConfig& sysconfig();            // current (in-memory) configuration
bool sysconfig_save();             // write header to disk
int sysconfig_drive();             // ATA drive index of the install, -1 if none
// Hash a password for UserAccount::password_hash.
void sysconfig_hash_password(const char* login, const char* password, char out[65]);
bool sysconfig_check_password(const UserAccount& u, const char* password);

// User file persistence: load /Users from disk; save it (debounced) on change.
void userdata_load();
void userdata_enable_autosave();
bool userdata_save_now();

// Used by Setup: write a fresh config into the data partition of `drive`
// (whose MBR was just written from system.img).
bool sysconfig_init_on_drive(int drive, const SysConfig& cfg);
// Partition 2 (data) start LBA / size of a drive, from its MBR. False if absent.
bool sysconfig_find_data_partition(int drive, uint64_t* lba, uint64_t* sectors);
