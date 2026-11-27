// Host regression tests using the real configuration and password code, with an in-memory disk.
// g++ -std=gnu++20 -O2 -ffunction-sections -fdata-sections -Ikernel -Wl,--gc-sections \
//   tools/password_test.cpp kernel/sys/sysconfig.cpp kernel/net/crypto.cpp -o /tmp/password_test
#include "sys/sysconfig.h"
#include "fs/fat32.h"

extern "C" int puts(const char*);

static SysConfig disk;
static SysConfig pending;
static bool write_ok = true, flush_ok = true;
static int writes, failures;

namespace fat32 {
int drive() { return 0; }
bool mount(int) { return true; }
bool write_file(const char* path, const void* data, size_t size, uint64_t) {
    writes++;
    if (!write_ok || strcmp(path, "/KleinOS/config.bin") || size != sizeof(SysConfig)) return false;
    memcpy(&pending, data, size);
    return true;
}
bool flush() {
    if (!flush_ok) return false;
    disk = pending;
    return true;
}
}
uint64_t rtc_unix_time() { return 0; }
void kfree(void*) {} // Empty global vectors have nothing to release.

static void check(bool ok, const char* message) {
    if (!ok) { puts(message); failures++; }
}

int main() {
    using R = PasswordChangeResult;
    check(sysconfig_change_password("alice", "old", "new") == R::Unavailable, "FAIL: live session allowed a change");
    SysConfig cfg{};
    cfg.user_count = 2;
    strcpy(cfg.users[0].login, "alice");
    strcpy(cfg.users[1].login, "bob");
    sysconfig_hash_password("alice", "old", cfg.users[0].password_hash);
    sysconfig_hash_password("bob", "other", cfg.users[1].password_hash);
    check(sysconfig_init_on_drive(0, cfg), "FAIL: disk initialization");
    int before = writes;
    check(sysconfig_change_password("alice", "wrong", "new") == R::IncorrectPassword, "FAIL: wrong current password accepted");
    check(sysconfig_change_password("missing", "old", "new") == R::Unavailable, "FAIL: nonexistent account accepted");
    check(sysconfig_change_password(nullptr, "old", "new") == R::Unavailable, "FAIL: null login accepted");
    check(writes == before, "FAIL: rejected change wrote to disk");
    check(sysconfig_change_password("alice", "old", "new") == R::Saved, "FAIL: password change");
    check(sysconfig_check_password(sysconfig().users[0], "new"), "FAIL: new password rejected");
    check(!sysconfig_check_password(sysconfig().users[0], "old"), "FAIL: old password still accepted");
    check(sysconfig_check_password(disk.users[0], "new"), "FAIL: new password not persisted");
    check(!memcmp(&disk.users[1], &cfg.users[1], sizeof(UserAccount)), "FAIL: other account modified");
    check(sysconfig_init_on_drive(0, disk) && sysconfig_check_password(sysconfig().users[0], "new"), "FAIL: password lost after reload");
    write_ok = false;
    check(sysconfig_change_password("alice", "new", "lost") == R::SaveFailed, "FAIL: write failure not reported");
    check(sysconfig_check_password(sysconfig().users[0], "new"), "FAIL: write failure changed live password");
    write_ok = true;
    flush_ok = false;
    check(sysconfig_change_password("alice", "new", "lost") == R::SaveFailed, "FAIL: flush failure not reported");
    check(sysconfig_check_password(sysconfig().users[0], "new"), "FAIL: flush failure changed live password");
    flush_ok = true;
    check(sysconfig_change_password("alice", "new", "") == R::Saved, "FAIL: password removal");
    check(!disk.users[0].password_hash[0], "FAIL: removed password persisted");
    check(sysconfig_change_password("alice", "", "first") == R::Saved, "FAIL: setting a password on an unprotected account");
    check(sysconfig_check_password(disk.users[0], "first"), "FAIL: first password not persisted");
    puts(failures ? "Password regression tests FAILED" : "Password regression tests passed");
    return failures ? 1 : 0;
}
