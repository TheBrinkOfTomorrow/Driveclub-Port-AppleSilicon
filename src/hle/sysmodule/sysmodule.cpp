// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/sysmodule/sysmodule.h"

#include <mutex>
#include <set>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::Sysmodule {

namespace {

constexpr s32 ORBIS_SYSMODULE_INVALID_ID = static_cast<s32>(0x805A1000);
constexpr s32 ORBIS_SYSMODULE_NOT_LOADED = static_cast<s32>(0x805A1001);

// System modules are HLE'd and always available; this only tracks what the
// game asked for, so IsLoaded answers like the real one.
std::mutex g_lock;
std::set<u32> g_loaded;

s32 sceSysmoduleLoadModule(u32 id) {
    if (id == 0) {
        return ORBIS_SYSMODULE_INVALID_ID;
    }
    std::scoped_lock lk(g_lock);
    if (g_loaded.insert(id).second) {
        DC_INFO("sceSysmoduleLoadModule(%#x)", id);
    }
    return 0;
}

s32 sceSysmoduleUnloadModule(u32 id) {
    DC_INFO("sceSysmoduleUnloadModule(%#x)", id);
    std::scoped_lock lk(g_lock);
    g_loaded.erase(id);
    return 0;
}

// 0 (ORBIS_OK) when loaded. It returned 1, which games read as "not loaded":
// v1.28 then reloaded libSceNpSignaling in a loop after "Start Event".
s32 sceSysmoduleIsLoaded(u32 id) {
    if (id == 0) {
        return ORBIS_SYSMODULE_INVALID_ID;
    }
    std::scoped_lock lk(g_lock);
    return g_loaded.count(id) ? 0 : ORBIS_SYSMODULE_NOT_LOADED;
}

} // namespace

void RegisterSysmodule() {
    constexpr const char* lib = "libSceSysmodule";
    DC_HLE(lib, sceSysmoduleLoadModule);
    DC_HLE(lib, sceSysmoduleUnloadModule);
    DC_HLE(lib, sceSysmoduleIsLoaded);
}

} // namespace Hle::Sysmodule
