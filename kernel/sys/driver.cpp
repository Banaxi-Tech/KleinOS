#include "sys/driver.h"
#include "sys/dl.h"
#include "fs/vfs.h"
#include "lib/log.h"
#include "lib/vector.h"

namespace {

Vector<const PciDevice*> g_claimed;

bool id_matches(const DriverInfo* info, const PciDevice* d) {
    for (int i = 0; i < info->id_count; i++)
        if (info->ids[i].vendor == d->vendor && (info->ids[i].device == kPciAnyDevice || info->ids[i].device == d->device)) return true;
    return false;
}

}  // namespace

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
    int bound = 0;
    for (int i = 0; i < pci_count(); i++) {
        const PciDevice* d = pci_get(i);
        if (g_claimed.contains(d) || !id_matches(info, d)) continue;
        if (!info->probe(d)) continue;
        g_claimed.push(d);
        bound++;
        klog("driver: %s bound to %04x:%04x", info->name, d->vendor, d->device);
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
    return n;
}
