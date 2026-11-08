# KleinOS

A hobby operating system written from scratch in C++ for x86-64. It has a macOS-inspired desktop with
**Liquid Glass**: real-time refraction, dispersion and frosted blur, after
[glisse](https://github.com/0kzh/glisse). It also has a web browser with JavaScript, a Windows-style installer
with product keys, and about 25 apps.

## Build & run

Requirements (Arch): `gcc make grub xorriso mtools qemu`.

```sh
make                 # build/KleinOS.iso  (installer ISO, BIOS + UEFI)
make install-vm      # fresh 8 GB virtual disk, boots the ISO once -> KleinOS Setup
make run             # boot the installed virtual disk
make live            # boot straight into a live desktop (no install)
make shot APP=browser EXTRA_CMDLINE=url=https://example.com/   # headless screenshot -> build/shot.png
```

The build number in `BUILD_NUMBER` goes up with every rebuild, but only in the development checkout, which has a
git-ignored `development.cfg` containing `development_version=true`.

## Installing

1. Boot the ISO and pick **Install KleinOS**.
2. Choose language/region/keyboard, then **Install now**.
3. Enter a product key, or click *I don't have a product key* and pick an edition.
4. Accept the license, choose **Custom**, and select the disk. Setup copies the system and restarts.
5. On first boot, the setup assistant creates your account. After that you get the login window.

Generic product keys (generated with `tools/keygen.cpp`):

| Edition    | Key                             |
|------------|---------------------------------|
| Home       | `PDJJF-234C3-HPR97-TMB2W-WPHP7` |
| Pro        | `439J2-XY8FX-74RF9-DJGQV-YRXH3` |
| Enterprise | `H6JP7-CVQHF-R43KF-KJPFW-FWV8X` |
| Education  | `W3KGR-VRCKT-748RH-XJ267-RYCKT` |

To build the key generator: `g++ -std=c++17 -O2 -o keygen tools/keygen.cpp && ./keygen pro 5`

## Architecture

- **Boot:** GRUB Multiboot2, then a 32-bit trampoline into 64-bit long mode (`kernel/arch/boot.S`). The first 4 GiB
  are identity-mapped.
- **Kernel:** IDT/PIC/PIT, a boundary-tag heap, and drivers for PS/2 keyboard and mouse, the VMware absolute
  pointer, RTC, PCI, ATA PIO, Intel e1000 and the PC speaker.
- **Graphics:** premultiplied-ARGB canvas, anti-aliased shapes, TrueType text (stb_truetype),
  PNG/JPEG (stb_image) and SVG.
- **Compositor:** damage-tracked, with glass surfaces (`kernel/gfx/glass.cpp`) for the menubar, Dock, window
  titlebars, sidebars, menus, Spotlight, Control Center and notifications.
- **`.dl` dynamic libraries:** apps (`apps/<Name>/` → `/System/Apps/<Name>.dl`) and libraries
  (`libs/<Name>/` → `/System/Libraries/<Name>.dl`) are ELF shared objects. The kernel loader in
  `kernel/sys/dl.cpp` links them at boot against `/System/kernel.sym`.
- **Network:** DHCP, DNS, ARP, IPv4, ICMP, UDP and TCP, plus an HTTP/1.1 client and a TLS 1.3 client
  (X25519 + AES-128-GCM). Certificates are **not** verified.
- **Browser (Voyager):** `HTMLEngine.dl` does HTML parsing, CSS (selectors, variables, calc(), media queries,
  Tailwind-style escapes), flexbox/grid/table layout, scroll containers and forms. JavaScript runs on
  QuickJS (`ECMAScript.dl`) with a DOM bridge (`libs/HTMLEngine/js.cpp`). ES modules, canvas and WebGL are not
  supported.
- **Installer:** `apps/Setup` writes `system.img` to the disk: MBR + GRUB core, a FAT16 boot partition and a
  KleinOS data partition. The data partition holds the configuration and the persisted `/Users`.

See `docs/APP_GUIDE.md` to write your own app.

## Third-party components
GRUB (GPL, bootloader), QuickJS-ng (MIT), stb_truetype / stb_image (public domain), Noto Sans (OFL),
DejaVu Sans Mono (free license).
