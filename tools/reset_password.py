#!/usr/bin/env python3
"""Reset a forgotten KleinOS account password from another computer.

KleinOS keeps accounts in /KleinOS/config.bin on the install's FAT32 partition
(which starts 1 MiB into the disk). This tool lists the accounts, and can clear
one account's password (you log in with an empty password, then set a new one
in Settings) or set a new one directly. Needs mtools (mcopy).

  tools/reset_password.py build/disk.img                    list accounts
  tools/reset_password.py build/disk.img alice              remove alice's password
  tools/reset_password.py build/disk.img alice --set NEW    give alice the password NEW
  sudo tools/reset_password.py /dev/sdX alice               a real disk / USB stick (unmounted!)

The disk is not encrypted, so anyone holding it can do this: keep the machine
physically safe if that matters.
"""
import argparse, hashlib, os, shutil, subprocess, sys, tempfile

PART = "@@1M"
CONFIG = "::/KleinOS/config.bin"
USERS_OFF = 136                 # offsetof(SysConfig, users), see kernel/sys/sysconfig.h
USER_COUNT_OFF = 132
USER_SIZE = 169                 # sizeof(UserAccount)
NAME_OFF, LOGIN_OFF, HASH_OFF = 0, 64, 96
MAX_USERS = 4


def cstr(b):
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disk", help="disk image or block device holding an installed KleinOS")
    ap.add_argument("login", nargs="?", help="account to reset (omit to list accounts)")
    ap.add_argument("--set", metavar="PASSWORD", help="new password instead of none")
    a = ap.parse_args()
    if not shutil.which("mcopy"):
        sys.exit("needs mtools (mcopy): pacman -S mtools / apt install mtools")

    with tempfile.TemporaryDirectory() as tmp:
        local = os.path.join(tmp, "config.bin")
        r = subprocess.run(["mcopy", "-n", "-i", a.disk + PART, CONFIG, local], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"no KleinOS configuration on {a.disk}: {r.stderr.strip()}")
        cfg = bytearray(open(local, "rb").read())
        if cfg[:8] != b"KLEINCFG":
            sys.exit("config.bin is not a KleinOS configuration")
        count = min(int.from_bytes(cfg[USER_COUNT_OFF:USER_COUNT_OFF + 4], "little"), MAX_USERS)
        users = []
        for i in range(count):
            base = USERS_OFF + i * USER_SIZE
            users.append((base, cstr(cfg[base + LOGIN_OFF:base + LOGIN_OFF + 32]),
                          cstr(cfg[base + NAME_OFF:base + NAME_OFF + 64]),
                          bool(cfg[base + HASH_OFF])))
        if not a.login:
            for _, login, name, has_pw in users:
                print(f"{login:<16} {name:<32} {'password set' if has_pw else 'no password'}")
            return
        match = [u for u in users if u[1] == a.login]
        if not match:
            sys.exit(f"no account '{a.login}' (have: {', '.join(u[1] for u in users) or 'none'})")
        base = match[0][0]
        field = bytearray(65)
        if a.set:
            field[:64] = hashlib.sha256(f"KleinOS-salt:{a.login}:{a.set}".encode()).hexdigest().encode()
        cfg[base + HASH_OFF:base + HASH_OFF + 65] = field
        open(local, "wb").write(cfg)
        r = subprocess.run(["mcopy", "-o", "-i", a.disk + PART, local, CONFIG], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"couldn't write the configuration back: {r.stderr.strip()}")
        print(f"{a.login}: password {'changed' if a.set else 'removed, log in with an empty password'}")


if __name__ == "__main__":
    main()
