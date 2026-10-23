// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>
#include "hle/pad/pad.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

#include "common/log.h"
#include "core/loader/linker.h"
#include "hle/hle.h"

namespace Hle::Pad {

namespace {

constexpr s32 kErrorInvalidArg = static_cast<s32>(0x80920001);
constexpr s32 kErrorAlreadyOpened = static_cast<s32>(0x80920004);
constexpr s32 kErrorInvalidUser = static_cast<s32>(0x809B0001); // device service
constexpr s32 kPortTypeRemoteControl = 16;
constexpr s32 kUserSystem = 0xFF;

// One handle per (user, port type, index), all fed by the one controller, as
// in shadPS4. v1.28 opens a special port (type 2) first, then the standard
// one; it got "invalid port type" for the first and ignored the pad's input.
std::mutex g_handles_lock;
std::map<std::tuple<s32, s32, s32>, s32> g_handles;
s32 g_next_handle = 1;

s32 OpenPad(const char* fn, s32 userId, s32 type, s32 index) {
    s32 result;
    if (userId < 0 || userId == kUserSystem) {
        result = kErrorInvalidUser;
    } else if (type == kPortTypeRemoteControl) {
        result = kErrorInvalidArg;
    } else {
        std::scoped_lock lk(g_handles_lock);
        auto [it, added] = g_handles.try_emplace({userId, type, index}, 0);
        result = added ? (it->second = g_next_handle++) : kErrorAlreadyOpened;
    }
    DC_INFO("%s(userId=%#x, type=%d, index=%d) -> %d", fn, userId, type, index, result);
    return result;
}

s32 scePadInit() {
    DC_INFO("scePadInit()");
    return 0;
}

s32 scePadOpen(s32 userId, s32 type, s32 index, const void* param) {
    return OpenPad("scePadOpen", userId, type, index);
}

s32 scePadOpenExt(s32 userId, s32 type, s32 index, const void* param) {
    return OpenPad("scePadOpenExt", userId, type, index);
}

s32 scePadClose(s32 handle) {
    DC_INFO("scePadClose(handle=%d)", handle);
    {
        std::scoped_lock lk(g_handles_lock);
        std::erase_if(g_handles, [&](const auto& e) { return e.second == handle; });
    }
    return 0;
}

static std::atomic<u32> g_buttons{0};
static std::atomic<u8> g_left_x{128};
static std::atomic<u8> g_left_y{128};
static std::atomic<u8> g_right_x{128};
static std::atomic<u8> g_right_y{128};
static std::atomic<u8> g_l2{0};
static std::atomic<u8> g_r2{0};

// Logs the first call per (function, handle, count) with its caller.
void LogCaller(const char* fn, s32 handle, s32 count, void* ret) {
    static std::mutex lock;
    static std::set<std::tuple<std::string, s32, s32>> seen;
    std::scoped_lock lk(lock);
    if (seen.emplace(fn, handle, count).second) {
        DC_INFO("pad: %s(handle=%d, count=%d) from %s", fn, handle, count,
                Core::Loader::Linker::Instance()->Describe(reinterpret_cast<u64>(ret)).c_str());
    }
}

// Logs what the game reads whenever it changes (input debugging).
void LogRead(const char* fn, const OrbisPadData& data) {
    static std::atomic<u64> last{~0ull};
    const u64 state = u64(data.buttons) | u64(data.analogButtons.l2) << 32 | u64(data.analogButtons.r2) << 40 |
                      u64(data.leftStick.x) << 48 | u64(data.leftStick.y) << 56;
    if (last.exchange(state) != state) {
        DC_INFO("pad: %s -> buttons %#x l2 %u r2 %u left (%u, %u)", fn, data.buttons, data.analogButtons.l2,
                data.analogButtons.r2, data.leftStick.x, data.leftStick.y);
    }
}

u64 NowMicros() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}

// Current state, stamped with the time of its last change.
void Fill(OrbisPadData& data, u64 timestamp) {
    std::memset(&data, 0, sizeof(data));
    data.buttons = g_buttons.load();
    data.leftStick = {g_left_x.load(), g_left_y.load()};
    data.rightStick = {g_right_x.load(), g_right_y.load()};
    data.analogButtons.l2 = g_l2.load();
    data.analogButtons.r2 = g_r2.load();
    data.orientation[3] = 1.0f;
    data.acceleration[2] = 9.8f;
    data.connected = true;
    data.connectedCount = 1;
    data.timestamp = timestamp;
}

// Like the real pad (and shadPS4), every input change is a sample with its own
// timestamp; scePadRead with room for several returns the samples queued since
// the last read, so games see each press and release.
// Each reader (handle) gets its own queue so no reader misses a change.
constexpr size_t kMaxSamples = 64;
std::mutex g_samples_lock;
std::map<s32, std::deque<OrbisPadData>> g_samples;
std::atomic<u64> g_changed_at{0};

void PushSample() {
    const u64 now = NowMicros();
    g_changed_at.store(now);
    OrbisPadData sample;
    Fill(sample, now);
    std::scoped_lock lk(g_samples_lock);
    for (auto& [handle, queue] : g_samples) {
        if (queue.size() == kMaxSamples) {
            queue.pop_front();
        }
        queue.push_back(sample);
    }
}

s32 scePadReadState(s32 handle, OrbisPadData* data) {
    LogCaller("scePadReadState", handle, 1, __builtin_return_address(0));
    if (data) {
        Fill(*data, g_changed_at.load());
        LogRead("scePadReadState", *data);
    }
    return 0;
}

s32 scePadRead(s32 handle, OrbisPadData* data, s32 count) {
    LogCaller("scePadRead", handle, count, __builtin_return_address(0));
    if (!data || count <= 0) {
        return 0;
    }
    if (count == 1) {
        Fill(data[0], g_changed_at.load());
        LogRead("scePadRead", data[0]);
        return 1;
    }
    std::scoped_lock lk(g_samples_lock);
    auto [it, first] = g_samples.try_emplace(handle);
    if (first) {
        Fill(it->second.emplace_back(), g_changed_at.load()); // start from the current state
    }
    auto& queue = it->second;
    s32 n = 0;
    while (n < count && !queue.empty()) {
        data[n++] = queue.front();
        queue.pop_front();
    }
    if (n > 0) {
        LogRead("scePadRead", data[n - 1]);
    }
    return n;
}

s32 scePadGetControllerInformation(s32 handle, OrbisPadControllerInformation* info) {
    if (info) {
        std::memset(info, 0, sizeof(*info));
        info->touchPadInfo.pixelDensity = 1.0f;
        info->touchPadInfo.resX = 1920;
        info->touchPadInfo.resY = 942;
        info->connected = true;
        info->connectedCount = 1;
    }
    return 0;
}

s32 scePadGetExtControllerInformation(s32 handle, void* pInfo) {
    return scePadGetControllerInformation(handle, reinterpret_cast<OrbisPadControllerInformation*>(pInfo));
}

s32 scePadSetTiltCorrectionState(s32 handle, bool bEnable) {
    return 0;
}

s32 scePadSetVibration(s32 handle, const void* param) {
    return 0;
}

s32 scePadResetOrientation(s32 handle) {
    return 0;
}

} // namespace

void SetButton(u32 button, bool pressed) {
    const u32 before = pressed ? g_buttons.fetch_or(button) : g_buttons.fetch_and(~button);
    if ((before & button) != (pressed ? button : 0u)) {
        PushSample();
    }
}

void SetStick(bool right, u8 x, u8 y) {
    if (right) {
        g_right_x.store(x);
        g_right_y.store(y);
    } else {
        g_left_x.store(x);
        g_left_y.store(y);
    }
    PushSample();
}

void SetTrigger(bool right, u8 value) {
    // The DS4 also reports a pressed trigger as the L2/R2 button.
    (right ? g_r2 : g_l2).store(value);
    if (const u32 b = right ? kR2 : kL2; ((g_buttons.load() & b) != 0) == (value > 0)) {
        PushSample(); // analog change only
    }
    SetButton(right ? kR2 : kL2, value > 0);
}

// DC_AUTOPRESS=<sec>,<sec>,...: presses Cross at those times after start
// (unattended runs that need to get past "Start Event").
static void StartAutoPress() {
    const char* e = std::getenv("DC_AUTOPRESS");
    if (!e) {
        return;
    }
    std::vector<double> times;
    for (const char* p = e; *p;) {
        char* q = nullptr;
        times.push_back(std::strtod(p, &q));
        p = *q == ',' ? q + 1 : q;
        if (q == p && *p) break;
    }
    std::thread([times] {
        const auto t0 = std::chrono::steady_clock::now();
        for (double t : times) {
            std::this_thread::sleep_until(t0 + std::chrono::milliseconds(static_cast<s64>(t * 1000)));
            DC_INFO("pad: auto-press Cross at %.1f s", t);
            SetButton(0x4000, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            SetButton(0x4000, false);
        }
    }).detach();
}

// DC_AUTODRIVE=<sec>: holds R2 (throttle) from that time after start
// (unattended runs that should drive once a race begins).
static void StartAutoDrive() {
    const char* e = std::getenv("DC_AUTODRIVE");
    if (!e) {
        return;
    }
    const double at = std::strtod(e, nullptr);
    std::thread([at] {
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<s64>(at * 1000)));
        DC_INFO("pad: auto-drive holds R2 from %.1f s", at);
        SetTrigger(true, 255);
    }).detach();
}

void RegisterPad() {
    StartAutoPress();
    StartAutoDrive();
    constexpr const char* lib = "libScePad";
    DC_HLE(lib, scePadInit);
    DC_HLE(lib, scePadOpen);
    DC_HLE(lib, scePadOpenExt);
    DC_HLE(lib, scePadClose);
    DC_HLE(lib, scePadReadState);
    DC_HLE(lib, scePadRead);
    DC_HLE(lib, scePadGetControllerInformation);
    DC_HLE(lib, scePadGetExtControllerInformation);
    DC_HLE(lib, scePadSetTiltCorrectionState);
    DC_HLE(lib, scePadSetVibration);
    DC_HLE(lib, scePadResetOrientation);
}

} // namespace Hle::Pad
