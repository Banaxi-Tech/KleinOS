#!/usr/bin/env python3
"""KleinOS update packages (.kupd): signing key and packing.

  kupd.py selftest
  kupd.py genkey                 create the private key and kernel/sys/update_key.h
  kupd.py pack --arch i386 --version 1.0 --build 170 --kernel K --initrd I --tag v1.0.170 --outdir DIR [--patch-from OLD.kupd ...]

The private key lives outside the repository ($KUPD_KEY or
~/.config/kleinos/update-signing.key). Layout of a .kupd, little endian:
  "KUPD", format, arch, main, sub, build, min_main, min_sub, min_build, reserved   (u32 x 10)
  2 x { compressed size, raw size (u32), sha256(compressed), sha256(raw) }         kernel, initrd
  Ed25519 signature over everything above (64 bytes), then the zlib payloads.
A patch is the same layout with format 2 and min_* set to the one version it applies to:
each payload is a zlib-compressed patch stream, and raw size/sha256 describe the rebuilt file.
"""
import argparse, hashlib, json, os, re, struct, sys, zlib

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEY_HEADER = os.path.join(REPO_ROOT, "kernel", "sys", "update_key.h")
ARCH_IDS = {"i386": 1, "x86_64": 2, "aarch64": 3}
KERNEL_SLOT = 8 << 20
INITRD_SLOT = 40 << 20
FORMAT = 1
PATCH_FORMAT = 2
PATCH_EXACT = 0x80000000
MANIFEST_LIMIT = 2047
SEED = 8
MIN_MATCH = 32

# ---- Ed25519 (RFC 8032)
P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)
def sha512(b): return hashlib.sha512(b).digest()
def recover_x(y, sign):
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P) % P
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P: x = x * SQRT_M1 % P
    if (x & 1) != sign: x = P - x
    return x
By = 4 * pow(5, P - 2, P) % P
Bx = recover_x(By, 0)
B = (Bx, By, 1, Bx * By % P)
def add(a, b):
    A = (a[1] - a[0]) * (b[1] - b[0]) % P
    C = (a[1] + a[0]) * (b[1] + b[0]) % P
    E = 2 * a[3] * b[3] * D % P
    F = 2 * a[2] * b[2] % P
    G, H, I, J = C - A, F - E, F + E, C + A
    return (G * H % P, J * I % P, I * H % P, G * J % P)
def mul(s, p):
    q = (0, 1, 1, 0)
    while s:
        if s & 1: q = add(q, p)
        p = add(p, p)
        s >>= 1
    return q
def compress(p):
    zi = pow(p[2], P - 2, P)
    x, y = p[0] * zi % P, p[1] * zi % P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")
def expand(seed):
    h = sha512(seed)
    a = int.from_bytes(h[:32], "little") & ((1 << 254) - 8) | (1 << 254)
    return a, h[32:]
def public_key(seed): return compress(mul(expand(seed)[0], B))
def sign(seed, msg):
    a, prefix = expand(seed)
    A = compress(mul(a, B))
    r = int.from_bytes(sha512(prefix + msg), "little") % L
    R = compress(mul(r, B))
    k = int.from_bytes(sha512(R + A + msg), "little") % L
    return R + int.to_bytes((r + k * a) % L, 32, "little")

def selftest():
    # RFC 8032 section 7.1, TEST 2
    seed = bytes.fromhex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb")
    want = ("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
            "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00")
    assert public_key(seed).hex() == "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"
    assert sign(seed, b"\x72").hex() == want
    print("ed25519 ok")

# ---- key handling
def key_path():
    return os.environ.get("KUPD_KEY") or os.path.expanduser("~/.config/kleinos/update-signing.key")
def load_key():
    with open(key_path()) as f: seed = bytes.fromhex(f.read().strip())
    if len(seed) != 32: sys.exit("bad signing key")
    return seed
def write_header(pub):
    bytes_c = ", ".join("0x%02x" % b for b in pub)
    with open(KEY_HEADER, "w") as f:
        f.write("#pragma once\n// Public half of the update signing key (tools/kupd.py genkey).\n"
                "#include <stdint.h>\n\nstatic const uint8_t kUpdatePublicKey[32] = {\n    %s\n};\n" % bytes_c)
def genkey():
    path = key_path()
    if os.path.exists(path): sys.exit("refusing to replace existing key: " + path)
    os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
    seed = os.urandom(32)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f: f.write(seed.hex() + "\n")
    write_header(public_key(seed))
    print("private key:", path)
    print("public key :", KEY_HEADER)

# ---- patches
def extent(new, old, i, d):
    """How far new[i:] follows old at offset d, allowing scattered changes (moved code changes only its addresses)."""
    s, n, m = i, len(new), len(old)
    while s < n and s + d >= 0 and s + d < m:
        e = min(s + 4096, n)
        if e + d <= m and new[s:e] == old[s + d:e + d]:
            s = e
            continue
        b = min(16, n - s, m - (s + d))
        if sum(x == y for x, y in zip(new[s:s + b], old[s + d:s + d + b])) * 2 < b: break
        s += b
    return s - i

def find_segments(old, new):
    new, old = memoryview(new), memoryview(old)
    step = max(4, len(old) // 1000000)
    index = {}
    for p in range(0, len(old) - SEED, step): index[bytes(old[p:p + SEED])] = p
    segments, i, d, n = [], 0, 0, len(new)
    while i < n:
        length = extent(new, old, i, d)
        if length >= MIN_MATCH:
            segments.append((i, i + d, length))
            i += length
            continue
        j = i
        while j + SEED <= n:
            p = index.get(bytes(new[j:j + SEED]))
            if p is not None and extent(new, old, j, p - j) >= MIN_MATCH:
                d = p - j
                break
            j += 1
        else:
            break
        i = j
    return segments

def make_patch(old, new):
    ops, diffs, extras = [], [], []
    pos, oldpos = 0, 0
    def add_op(add, extra, seek): ops.append(struct.pack("<IIi", add, extra, seek))
    segments = find_segments(old, new)
    if not segments or segments[0][0] > 0 or segments[0][1] > 0:
        gap = segments[0][0] if segments else len(new)
        extras.append(new[:gap])
        add_op(0, gap, segments[0][1] if segments else 0)
    for k, (start, ostart, length) in enumerate(segments):
        delta = bytes((x - y) & 255 for x, y in zip(new[start:start + length], old[ostart:ostart + length])) \
            if new[start:start + length] != old[ostart:ostart + length] else bytes(length)
        p = 0
        pieces = []
        for m in re.finditer(rb"\0{256,}", delta):
            if m.start() > p: pieces.append((p, m.start(), False))
            pieces.append((m.start(), m.end(), True))
            p = m.end()
        if p < length: pieces.append((p, length, False))
        for a, b, exact in pieces[:-1]:
            add_op((b - a) | (PATCH_EXACT if exact else 0), 0, 0)
            if not exact: diffs.append(delta[a:b])
        a, b, exact = pieces[-1]
        if not exact: diffs.append(delta[a:b])
        end = start + length
        next_start = segments[k + 1][0] if k + 1 < len(segments) else len(new)
        next_old = segments[k + 1][1] if k + 1 < len(segments) else ostart + length
        extras.append(new[end:next_start])
        add_op((b - a) | (PATCH_EXACT if exact else 0), next_start - end, next_old - (ostart + length))
    diff, extra = b"".join(diffs), b"".join(extras)
    return struct.pack("<III", len(ops), len(diff), len(extra)) + b"".join(ops) + diff + extra

def apply_patch(old, patch):
    """The tool's own copy of the updater's apply_patch, used to check every patch before it is signed."""
    count, dlen, elen = struct.unpack_from("<III", patch)
    base = 12 + count * 12
    dp, ep, op, out = base, base + dlen, 0, bytearray()
    for i in range(count):
        add, ext, seek = struct.unpack_from("<IIi", patch, 12 + i * 12)
        exact = add & PATCH_EXACT
        add &= ~PATCH_EXACT
        if exact: out += old[op:op + add]
        else:
            out += bytes((x + y) & 255 for x, y in zip(old[op:op + add], patch[dp:dp + add]))
            dp += add
        op += add
        out += patch[ep:ep + ext]
        ep += ext
        op += seek
    return bytes(out)

def read_full(path, arch):
    """Version and raw payloads of an earlier full package."""
    d = open(path, "rb").read()
    magic, fmt, aid, main, sub, build = struct.unpack_from("<4s5I", d)
    if magic != b"KUPD" or fmt != FORMAT or aid != ARCH_IDS[arch]: sys.exit("not a full %s package: %s" % (arch, path))
    offs, raws = 248, []
    for i in range(2):
        comp, raw = struct.unpack_from("<II", d, 40 + i * 72)
        data = zlib.decompress(d[offs:offs + comp])
        if len(data) != raw or hashlib.sha256(data).digest() != d[40 + i * 72 + 40:40 + i * 72 + 72]: sys.exit("damaged package: " + path)
        raws.append(data)
        offs += comp
    return "%d.%d.%d" % (main, sub, build), (main, sub, build), raws

def pack_patch(seed, a, main, sub, targets, full_size, path):
    frm, low, bases = read_full(path, a.arch)
    head = struct.pack("<4s9I", b"KUPD", PATCH_FORMAT, ARCH_IDS[a.arch], main, sub, a.build, *low, 0)
    blobs = []
    for base, target in zip(bases, targets):
        patch = make_patch(base, target)
        if apply_patch(base, patch) != target: sys.exit("patch from %s does not rebuild the target" % frm)
        comp = zlib.compress(patch, 9)
        blobs.append(comp)
        head += struct.pack("<II", len(comp), len(target)) + hashlib.sha256(comp).digest() + hashlib.sha256(target).digest()
    data = head + sign(seed, head) + b"".join(blobs)
    if len(data) * 2 >= full_size:
        print("  PATCH  from %s skipped (%d bytes is not worth it)" % (frm, len(data)))
        return None
    name = "KleinOS-%s.%d-%s-from-%s.kupd" % (a.version, a.build, a.arch, frm)
    with open(os.path.join(a.outdir, name), "wb") as f: f.write(data)
    print("  PATCH  %s (%d bytes)" % (name, len(data)))
    return frm, name

# ---- packing
def pack(a):
    seed = load_key()
    with open(KEY_HEADER) as f: text = f.read()
    if ", ".join("0x%02x" % b for b in public_key(seed)) not in text:
        sys.exit("signing key does not match kernel/sys/update_key.h")
    kernel = open(a.kernel, "rb").read()
    initrd = open(a.initrd, "rb").read()
    if len(kernel) > KERNEL_SLOT or len(initrd) > INITRD_SLOT: sys.exit("payload is too large for an update")
    main, sub = (int(x) for x in a.version.split("."))
    low = [int(x) for x in a.min.split(".")]
    head = struct.pack("<4s9I", b"KUPD", FORMAT, ARCH_IDS[a.arch], main, sub, a.build, *low, 0)
    blobs = []
    for raw in (kernel, initrd):
        comp = zlib.compress(raw, 9)
        blobs.append(comp)
        head += struct.pack("<II", len(comp), len(raw)) + hashlib.sha256(comp).digest() + hashlib.sha256(raw).digest()
    data = head + sign(seed, head) + b"".join(blobs)
    full = "%s.%d" % (a.version, a.build)
    name = "KleinOS-%s-%s.kupd" % (full, a.arch)
    os.makedirs(a.outdir, exist_ok=True)
    with open(os.path.join(a.outdir, name), "wb") as f: f.write(data)
    manifest = {"version": full, "arch": a.arch, "size": len(data),
                "url": "https://github.com/%s/releases/download/%s/%s" % (a.repo, a.tag, name)}
    for path in a.patch_from:
        made = pack_patch(seed, a, main, sub, (kernel, initrd), len(data), path)
        if made: manifest["patch-" + made[0]] = "https://github.com/%s/releases/download/%s/%s" % (a.repo, a.tag, made[1])
    text = json.dumps(manifest)
    if len(text) > MANIFEST_LIMIT: sys.exit("manifest too large for the updater; use fewer --patch-from")
    with open(os.path.join(a.outdir, "latest-%s.json" % a.arch), "w") as f:
        f.write(text)
    print("  KUPD   %s (%d bytes)" % (name, len(data)))

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("selftest")
    sub.add_parser("genkey")
    p = sub.add_parser("pack")
    p.add_argument("--arch", choices=ARCH_IDS, required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--build", type=int, required=True)
    p.add_argument("--min", default="0.0.0")
    p.add_argument("--kernel", required=True)
    p.add_argument("--initrd", required=True)
    p.add_argument("--tag", required=True)
    p.add_argument("--repo", default="Banaxi-Tech/KleinOS")
    p.add_argument("--outdir", required=True)
    p.add_argument("--patch-from", action="append", default=[])
    a = ap.parse_args()
    if a.cmd == "selftest": selftest()
    elif a.cmd == "genkey": genkey()
    else: pack(a)

if __name__ == "__main__":
    main()
