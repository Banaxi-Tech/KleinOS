#include "ui/app.h"
#include "ui/window.h"
#include "ui/wm.h"
#include "lib/string.h"
#include "lib/log.h"

namespace {
constexpr int kMaxApps = 128;
AppInfo g_apps[kMaxApps];
const char* g_modules[kMaxApps];
int g_count;
const char* g_loading_module = "kernel";
}

AppRegistrar::AppRegistrar(const AppInfo& info) {
    if (g_count >= kMaxApps) return;
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_apps[i].id, info.id) == 0) {
            klog("app: duplicate id '%s' ignored", info.id);
            return;
        }
    g_modules[g_count] = g_loading_module;
    g_apps[g_count++] = info;
}

void app_set_loading_module(const char* name) { g_loading_module = name ? strdup(name) : "kernel"; }
int app_count() { return g_count; }
const AppInfo* app_get(int i) { return (i >= 0 && i < g_count) ? &g_apps[i] : nullptr; }

const AppInfo* app_find(const char* id) {
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_apps[i].id, id) == 0) return &g_apps[i];
    return nullptr;
}

const char* app_module(const char* id) {
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_apps[i].id, id) == 0) return g_modules[i];
    return "";
}

Window* app_launch(const char* id) {
    const AppInfo* a = app_find(id);
    if (!a) {
        klog("app: unknown app '%s'", id);
        return nullptr;
    }
    if (a->single_instance) {
        const Vector<Window*>& ws = wm::windows();
        for (size_t i = ws.size(); i > 0; i--)
            if (strcmp(ws[i - 1]->app_id(), id) == 0 && !ws[i - 1]->closing_) {
                wm::restore(ws[i - 1]);
                wm::focus(ws[i - 1]);
                return ws[i - 1];
            }
    }
    klog("app: launching %s", id);
    Window* w = a->create();
    if (w) {
        w->set_app_id(id);
        wm::focus(w);
    }
    return w;
}
