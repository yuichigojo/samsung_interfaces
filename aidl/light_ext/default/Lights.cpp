/*
 * Copyright (C) 2021 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.samsung_ext.hardware.lights-service"

#include <android-base/stringprintf.h>
#include <android-base/properties.h>

#include <fstream>
#include <mutex>

#include "Lights.h"

constexpr const int COLOR_MASK = 0x00ffffff;
constexpr const int MAX_INPUT_BRIGHTNESS = 255;
constexpr const float SUNLIGHT_RATIO = 0.8f;

static const char SUNLIGHT_ENABLED_PROP[] = "persist.vendor.ext.sunlight.on";

namespace aidl {
namespace android {
namespace hardware {
namespace light {

/*
 * Write value to path and close file.
 */
template <typename T>
static void set(const std::string& path, const T& value) {
    std::ofstream file(path);
    file << value << std::endl;
}

template <typename T>
static T get(const std::string& path, const T& def) {
    std::ifstream file(path);
    T result;

    file >> result;
    return file.fail() ? def : result;
}

using ::android::base::GetBoolProperty;
using ::android::base::SetProperty;

Lights::Lights() {
    mLights.emplace(LightType::BACKLIGHT,
                    std::bind(&Lights::handleBacklight, this, std::placeholders::_1));
#ifdef BUTTON_BRIGHTNESS_NODE
    mLights.emplace(LightType::BUTTONS, std::bind(&Lights::handleButtons, this, std::placeholders::_1));
#endif /* BUTTON_BRIGHTNESS_NODE */
#ifdef LED_BLINK_NODE
    mLights.emplace(LightType::BATTERY, std::bind(&Lights::handleBattery, this, std::placeholders::_1));
    mLights.emplace(LightType::NOTIFICATIONS,
                    std::bind(&Lights::handleNotifications, this, std::placeholders::_1));
    mLights.emplace(LightType::ATTENTION,
                    std::bind(&Lights::handleAttention, this, std::placeholders::_1));
#endif /* LED_BLINK_NODE */
}

ndk::ScopedAStatus Lights::setLightState(int32_t id, const HwLightState& state) {
    LightType type = static_cast<LightType>(id);
    auto it = mLights.find(type);

    if (it == mLights.end()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    /*
     * Lock global mutex until light state is updated.
     */
    std::lock_guard<std::mutex> lock(mLock);

    it->second(state);

    return ndk::ScopedAStatus::ok();
}

void Lights::handleBacklight_brightness(const bool fromExtHal, const uint32_t brightness_s) {
    static int32_t max_brightness;
    static std::once_flag once;
    static bool need_conversion;
    int32_t brightness;

    std::call_once(once, [this]{ 
         max_brightness = get(PANEL_MAX_BRIGHTNESS_NODE, MAX_INPUT_BRIGHTNESS);
         need_conversion = max_brightness != MAX_INPUT_BRIGHTNESS;
         sunlight_data.enabled = GetBoolProperty(SUNLIGHT_ENABLED_PROP, false);
    });

    if (!fromExtHal) {
        // If it wasn't called from ExtHAL...
        brightness = brightness_s;
        if (need_conversion) {
             brightness = brightness * max_brightness / MAX_INPUT_BRIGHTNESS;
        }
        sunlight_data.requested_brightness = brightness;
    } else {
        // New enabled data from onPropsChanged()
        sunlight_data.enabled = GetBoolProperty(SUNLIGHT_ENABLED_PROP, false);
        // If the call was from ExtHAL, brightness is from cache
        brightness = sunlight_data.requested_brightness;
        if (brightness == -1) {
            // If brightness is -1 (Meaning not initialized), then better not set backlight to negative
            // cuz that... Just read it from sysfs
            brightness = get(PANEL_BRIGHTNESS_NODE, -1);
            if (brightness == -1) {
                // OK Kys
                return;
            }
        }
    }
    if (sunlight_data.enabled) {
        // If enabled, apply ratio.
        brightness *= SUNLIGHT_RATIO;
    }

    set(PANEL_BRIGHTNESS_NODE, brightness);
}

void Lights::handleBacklight(const HwLightState& state) {
    handleBacklight_brightness(false, rgbToBrightness(state));
}

#ifdef BUTTON_BRIGHTNESS_NODE
void Lights::handleButtons(const HwLightState& state) {
#ifdef VAR_BUTTON_BRIGHTNESS
    uint32_t brightness = rgbToBrightness(state);
#else
    uint32_t brightness = (state.color & COLOR_MASK) ? 1 : 0;
#endif

    set(BUTTON_BRIGHTNESS_NODE, brightness);
}
#endif

#ifdef LED_BLINK_NODE
void Lights::handleBattery(const HwLightState& state) {
    mBatteryState = state;
    setNotificationLED();
}

void Lights::handleNotifications(const HwLightState& state) {
    mNotificationState = state;
    setNotificationLED();
}

void Lights::handleAttention(const HwLightState& state) {
    mAttentionState = state;
    setNotificationLED();
}

void Lights::setNotificationLED() {
    int32_t adjusted_brightness = MAX_INPUT_BRIGHTNESS;
    HwLightState state;
#ifdef LED_BLN_NODE
    bool bln = false;
#endif /* LED_BLN_NODE */

    if (mNotificationState.color & COLOR_MASK) {
        adjusted_brightness = LED_BRIGHTNESS_NOTIFICATION;
        state = mNotificationState;
#ifdef LED_BLN_NODE
        bln = true;
#endif /* LED_BLN_NODE */
    } else if (mAttentionState.color & COLOR_MASK) {
        adjusted_brightness = LED_BRIGHTNESS_ATTENTION;
        state = mAttentionState;
        if (state.flashMode == FlashMode::HARDWARE) {
            if (state.flashOnMs > 0 && state.flashOffMs == 0) state.flashMode = FlashMode::NONE;
            state.color = 0x000000ff;
        }
        if (state.flashMode == FlashMode::NONE) {
            state.color = 0;
        }
    } else if (mBatteryState.color & COLOR_MASK) {
        adjusted_brightness = LED_BRIGHTNESS_BATTERY;
        state = mBatteryState;
    } else {
        set(LED_BLINK_NODE, "0x00000000 0 0");
        return;
    }

    if (state.flashMode == FlashMode::NONE) {
        state.flashOnMs = 0;
        state.flashOffMs = 0;
    }

    state.color = calibrateColor(state.color & COLOR_MASK, adjusted_brightness);
    set(LED_BLINK_NODE, ::android::base::StringPrintf("0x%08x %d %d", state.color, state.flashOnMs,
                                                    state.flashOffMs));

#ifdef LED_BLN_NODE
    if (bln) {
        set(LED_BLN_NODE, (state.color & COLOR_MASK) ? 1 : 0);
    }
#endif /* LED_BLN_NODE */
}

uint32_t Lights::calibrateColor(uint32_t color, int32_t brightness) {
    uint32_t red = ((color >> 16) & 0xFF) * LED_ADJUSTMENT_R;
    uint32_t green = ((color >> 8) & 0xFF) * LED_ADJUSTMENT_G;
    uint32_t blue = (color & 0xFF) * LED_ADJUSTMENT_B;

    return (((red * brightness) / 255) << 16) + (((green * brightness) / 255) << 8) +
           ((blue * brightness) / 255);
}
#endif /* LED_BLINK_NODE */

#define AutoHwLight(light) {.id = (int32_t)light, .type = light, .ordinal = 0}

ndk::ScopedAStatus Lights::getLights(std::vector<HwLight> *_aidl_return) {
    for (auto const& light : mLights) {
        _aidl_return->push_back(AutoHwLight(light.first));
    }

    return ndk::ScopedAStatus::ok();
}

uint32_t Lights::rgbToBrightness(const HwLightState& state) {
    uint32_t color = state.color & COLOR_MASK;

    return ((77 * ((color >> 16) & 0xff)) + (150 * ((color >> 8) & 0xff)) + (29 * (color & 0xff))) >>
           8;
}

} // namespace light
} // namespace hardware
} // namespace android
} // namespace aidl
