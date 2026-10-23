// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/system_service/system_service.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::SystemService {

namespace {

constexpr s32 ORBIS_SYSTEM_SERVICE_ERROR_NO_EVENT = static_cast<s32>(0x80A10004);
constexpr s32 ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER = static_cast<s32>(0x80A10003);

s32 sceSystemServiceGetStatus(OrbisSystemServiceStatus* status) {
    if (status) {
        std::memset(status, 0, sizeof(*status));
        status->event_num = 0;
        status->is_system_ui_overlaid = false;
        status->is_in_background_execution = false;
        status->is_cpu_mode7_cpu_normal = true;
        status->is_game_live_streaming_on_air = false;
        status->is_out_of_vr_play_area = false;
    }
    return 0;
}

s32 sceSystemServiceReceiveEvent(OrbisSystemServiceEvent* event) {
    return ORBIS_SYSTEM_SERVICE_ERROR_NO_EVENT;
}

s32 sceSystemServiceGetDisplaySafeAreaInfo(OrbisSystemServiceDisplaySafeAreaInfo* info) {
    if (info) {
        std::memset(info, 0, sizeof(*info));
        info->ratio = 1.0f;
    }
    return 0;
}

s32 sceSystemServiceHideSplashScreen() {
    return 0;
}

// System settings as a US/EU console reports them. Returning 0 for all of
// them meant Japanese (LANG 0) and Circle as the confirm button.
s32 sceSystemServiceParamGetInt(s32 paramId, s32* value) {
    if (!value) {
        return ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER;
    }
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    switch (paramId) {
    case 1: { // LANG: DC_LANG=<id>, default 1 (English, US)
        const char* lang = std::getenv("DC_LANG");
        *value = lang ? std::atoi(lang) : 1;
        break;
    }
    case 2: *value = 1; break; // DATE_FORMAT: DD/MM/YYYY
    case 3: *value = 1; break; // TIME_FORMAT: 24 hour
    case 4: // TIME_ZONE: minutes from UTC, without DST
        *value = static_cast<s32>((local.tm_gmtoff - (local.tm_isdst > 0 ? 3600 : 0)) / 60);
        break;
    case 5: *value = local.tm_isdst > 0 ? 1 : 0; break; // SUMMERTIME
    case 7: *value = 0; break;                          // GAME_PARENTAL_LEVEL: off
    case 1000: // ENTER_BUTTON_ASSIGN: 1 Cross (DC_CIRCLE_ENTER: 0 Circle)
        *value = std::getenv("DC_CIRCLE_ENTER") ? 0 : 1;
        break;
    default:
        *value = 0;
        break;
    }
    static std::atomic<u64> logged{0};
    const u64 bit = 1ull << (paramId == 1000 ? 63 : paramId & 63);
    if (!(logged.fetch_or(bit) & bit)) {
        DC_INFO("sceSystemServiceParamGetInt(%d) -> %d", paramId, *value);
    }
    return 0;
}

} // namespace

void RegisterSystemService() {
    constexpr const char* lib = "libSceSystemService";
    DC_HLE(lib, sceSystemServiceGetStatus);
    DC_HLE(lib, sceSystemServiceReceiveEvent);
    DC_HLE(lib, sceSystemServiceGetDisplaySafeAreaInfo);
    DC_HLE(lib, sceSystemServiceHideSplashScreen);
    DC_HLE(lib, sceSystemServiceParamGetInt);
}

} // namespace Hle::SystemService
