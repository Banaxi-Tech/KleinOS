#include "sys/driver.h"
#include "sys/dl.h"
#include "fs/vfs.h"
#include "lib/log.h"
#include "lib/vector.h"

namespace {

struct Binding {
    const PciDevice* dev;
    const char* name;
    bool fallback;
};
struct Fallback {
    const DriverInfo* info;
    const DriverFallback* match;
};

Vector<Binding> g_bindings;
Vector<Fallback> g_fallbacks;

Binding* binding_of(const PciDevice* d) {
    for (Binding& b : g_bindings)
        if (b.dev == d) return &b;
    return nullptr;
}

void bind(const DriverInfo* info, const PciDevice* d, bool fallback) {
    Binding* b = binding_of(d);
    if (b) { b->name = info->name; b->fallback = fallback; }
    else g_bindings.push({d, info->name, fallback});
    klog("driver: %s bound to %04x:%04x", info->name, d->vendor, d->device);
}

// the e1000 in net/stack.cpp starts after the drivers and is not one of them
bool is_builtin_nic(const PciDevice* d) {
    return d->vendor == 0x8086 && (d->device == 0x100E || d->device == 0x100F || d->device == 0x10D3);
}

void bind_fallbacks() {
    for (Fallback& f : g_fallbacks)
        for (int i = 0; i < pci_count(); i++) {
            const PciDevice* d = pci_get(i);
            if (binding_of(d) || is_builtin_nic(d) || d->class_code != f.match->class_code) continue;
            if (f.match->subclass != kPciAnySubclass && d->subclass != f.match->subclass) continue;
            if (f.info->probe(d)) bind(f.info, d, true);
        }
}

bool id_matches(const DriverInfo* info, const PciDevice* d) {
    for (int i = 0; i < info->id_count; i++)
        if (info->ids[i].vendor == d->vendor && (info->ids[i].device == kPciAnyDevice || info->ids[i].device == d->device)) return true;
    return false;
}

}  // namespace

const char* driver_for_device(const PciDevice* d) {
    Binding* b = binding_of(d);
    if (b) return b->name;
    return is_builtin_nic(d) ? "Intel PRO/1000 (built in)" : nullptr;
}

bool driver_is_fallback(const PciDevice* d) {
    Binding* b = binding_of(d);
    return b && b->fallback;
}

bool driver_load(const char* path, String* error) {
    auto fail = [&](const char* msg) {
        klog("driver: %s: %s", path, msg);
        if (error) *error = msg;
        return false;
    };
    DlModule* m = dl_open(path, false, error);
    if (!m) return false;
    const DriverInfo* info = (const DriverInfo*)dl_sym(m, "klein_driver");
    if (!info) return fail("no klein_driver symbol");
    if (info->abi != KLEIN_DRIVER_ABI) return fail("built for a different driver ABI");
    if (!info->name || !info->probe || (info->id_count && !info->ids)) return fail("incomplete DriverInfo");
    if (auto* match = (const DriverFallback*)dl_sym(m, "klein_driver_fallback")) {
        g_fallbacks.push({info, match});
        klog("driver: %s loaded as a fallback", info->name);
        return true;
    }
    int bound = 0;
    for (int i = 0; i < pci_count(); i++) {
        const PciDevice* d = pci_get(i);
        Binding* b = binding_of(d);
        if ((b && !b->fallback) || !id_matches(info, d)) continue;
        if (!info->probe(d)) continue;
        bind(info, d, false);
        bound++;
    }
    klog("driver: %s loaded, %d device(s)", info->name, bound);
    return true;
}

int driver_load_dir(const char* dir) {
    vfs::Node* d = vfs::lookup(dir);
    if (!d || !d->is_dir) return 0;
    Vector<String> names;
    for (vfs::Node* c : d->children)
        if (!c->is_dir && vfs::extension(c->name.c_str()) == "dri") names.push(c->name);
    for (size_t i = 1; i < names.size(); i++)
        for (size_t j = i; j > 0 && names[j] < names[j - 1]; j--) {
            String t = names[j]; names[j] = names[j - 1]; names[j - 1] = t;
        }
    int n = 0;
    for (String& name : names)
        if (driver_load(vfs::join(dir, name.c_str()).c_str())) n++;
    bind_fallbacks();
    return n;
}
