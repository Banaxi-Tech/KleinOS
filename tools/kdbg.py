#!/usr/bin/env python3
"""Client for KleinOS Wi-Fi Debugging (Settings > Wi-Fi Debugging).

  kdbg.py HOST info | log | modules | status | regs | cont | step | reboot
  kdbg.py HOST sym NAME
  kdbg.py HOST peek ADDR LEN
  kdbg.py HOST poke ADDR HEXBYTES
  kdbg.py HOST break ADDR_OR_SYMBOL | unbreak ADDR_OR_SYMBOL
  kdbg.py HOST update [--reboot]          install the newest release from GitHub
  kdbg.py HOST install FILE.kupd [--reboot]   install a signed package from this PC
  kdbg.py HOST shell                      type the commands above at a prompt

The password comes from $KDBG_PASSWORD or a prompt. While the kernel is stopped at
a breakpoint the whole machine waits. Don't set breakpoints in the network stack or
the heap allocator: the debugger needs them to keep talking to you.
"""
import getpass, hashlib, hmac, os, shlex, socket, struct, sys, time
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives import serialization

PORT = 5555
CMD = dict(info=1, log=2, peek=3, poke=4, sym=5, modules=6, brk=7, unbrk=8, cont=9, step=10, regs=11, status=12,
           reboot=20, ucheck=30, ustatus=31, uinstall=32, ubegin=33, uchunk=34, ucommit=35)
CHUNK = 60000
STATE_READY, STATE_FAILED = None, None


def expand(prk, info, n):
    out, block, i = b"", b"", 1
    while len(out) < n:
        block = hmac.new(prk, block + info + bytes([i]), hashlib.sha256).digest()
        out += block
        i += 1
    return out[:n]


class Session:
    def __init__(self, host, port, password):
        self.s = socket.create_connection((host, port), timeout=30)
        try:
            hello = self.recv(70)
        except (ConnectionError, socket.timeout):
            sys.exit("the server refused the connection: locked out after failed logins, or a debugger is already connected")
        if hello[:6] != b"KDBG1\n":
            sys.exit("not a KleinOS debug server")
        salt, server_pub = hello[6:22], hello[22:54]
        psk = hashlib.pbkdf2_hmac("sha1", password.encode(), salt, 4096, 32)
        priv = X25519PrivateKey.generate()
        pub = priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        nonce = os.urandom(16)
        th = hashlib.sha256(hello + pub + nonce).digest()
        shared = priv.exchange(X25519PublicKey.from_public_bytes(server_pub))
        prk = hmac.new(shared, psk, hashlib.sha256).digest()
        mac_key = expand(prk, b"mac", 32)
        self.s.sendall(pub + nonce + hmac.new(mac_key, b"client" + th, hashlib.sha256).digest())
        try:
            answer = self.recv(32)
        except (ConnectionError, socket.timeout):
            sys.exit("login failed: wrong password, or locked out after failed attempts")
        if not hmac.compare_digest(answer, hmac.new(mac_key, b"server" + th, hashlib.sha256).digest()):
            sys.exit("the server could not prove it knows the password")
        self.tx, self.rx = AESGCM(expand(prk, b"c2s", 16)), AESGCM(expand(prk, b"s2c", 16))
        self.tx_n = self.rx_n = 0

    def recv(self, n):
        data = b""
        while len(data) < n:
            part = self.s.recv(n - len(data))
            if not part:
                raise ConnectionError("connection closed")
            data += part
        return data

    def call(self, name, payload=b""):
        ct = self.tx.encrypt(b"\0" * 4 + struct.pack("<Q", self.tx_n), bytes([CMD[name]]) + payload, None)
        self.tx_n += 1
        self.s.sendall(struct.pack("<I", len(ct)) + ct)
        n = struct.unpack("<I", self.recv(4))[0]
        pt = self.rx.decrypt(b"\0" * 4 + struct.pack("<Q", self.rx_n), self.recv(n), None)
        self.rx_n += 1
        if pt[0]:
            raise RuntimeError(pt[1:].decode(errors="replace"))
        return pt[1:]

    def text(self, name, payload=b""):
        return self.call(name, payload).decode(errors="replace")

    def address(self, spec):
        try:
            return int(spec, 0)
        except ValueError:
            return struct.unpack("<Q", self.call("sym", spec.encode()))[0]


def wait_for_update(sess):
    while True:
        info = dict(l.split(" ", 1) for l in sess.text("ustatus").splitlines() if " " in l)
        print("\rstate %s, %s%%" % (info.get("state"), info.get("progress")), end="", flush=True)
        if info.get("error", "").strip():
            print()
            sys.exit("update failed: " + info["error"])
        if info.get("state") == "8":   # update::State::Ready
            print()
            return
        time.sleep(1)


def finish(sess, args):
    wait_for_update(sess)
    if "--reboot" in args:
        sess.call("reboot")
        print("rebooting")
    else:
        print("installed; restart to use it")


def run(sess, args):
    op = args[0]
    if op in ("info", "log", "modules", "status", "regs"):
        print(sess.text(op))
    elif op in ("cont", "step", "reboot"):
        print(sess.text(op))
    elif op == "sym":
        print(hex(struct.unpack("<Q", sess.call("sym", args[1].encode()))[0]))
    elif op == "peek":
        addr, n = sess.address(args[1]), int(args[2], 0)
        data = sess.call("peek", struct.pack("<QI", addr, n))
        for i in range(0, len(data), 16):
            print("%016x  %s" % (addr + i, data[i:i + 16].hex(" ")))
    elif op == "poke":
        print(sess.text("poke", struct.pack("<Q", sess.address(args[1])) + bytes.fromhex(args[2])))
    elif op == "break":
        print(sess.text("brk", struct.pack("<Q", sess.address(args[1]))))
    elif op == "unbreak":
        print(sess.text("unbrk", struct.pack("<Q", sess.address(args[1]))))
    elif op == "update":
        sess.call("ucheck")
        time.sleep(4)
        sess.call("uinstall")
        finish(sess, args)
    elif op == "install":
        data = open(args[1], "rb").read()
        sess.call("ubegin", struct.pack("<I", len(data)))
        for i in range(0, len(data), CHUNK):
            sess.call("uchunk", data[i:i + CHUNK])
            print("\rsent %d%%" % ((i + CHUNK) * 100 // len(data) if i + CHUNK < len(data) else 100), end="", flush=True)
        print()
        sess.call("ucommit")
        finish(sess, args)
    else:
        sys.exit(__doc__)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    host, _, port = sys.argv[1].partition(":")
    password = os.environ.get("KDBG_PASSWORD") or getpass.getpass("Debug password: ")
    sess = Session(host, int(port or PORT), password)
    args = sys.argv[2:]
    if args[0] != "shell":
        try:
            run(sess, args)
        except RuntimeError as e:
            sys.exit("error: %s" % e)
        return
    while True:
        try:
            line = input("kdbg> ").strip()
        except EOFError:
            return
        if not line:
            continue
        if line in ("quit", "exit"):
            return
        try:
            run(sess, shlex.split(line))
        except (RuntimeError, IndexError, ValueError) as e:
            print("error:", e)


if __name__ == "__main__":
    main()
