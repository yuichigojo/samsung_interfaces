/*
 * Copyright (C) 2019 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fstream>
#include <mutex>

#include <android-base/file.h>

#include "TouchscreenGesture.h"

namespace vendor {
namespace lineage {
namespace touch {
namespace V1_0 {
namespace samsung {

static constexpr const char* kTspPath = TSP_CMD_NODE;

const std::map<int32_t, TouchscreenGesture::GestureInfo> TouchscreenGesture::kGestureInfoMap = {
    // clang-format off
    {0, {0x1c7, "Single Tap"}},
    // clang-format on
};

bool TouchscreenGesture::isSupported() {
    static bool kSupported = false;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string cmds;
	android::base::ReadFileToString(TSP_CMD_LIST_NODE, &cmds);
	kSupported = cmds.find("singletap_enable") != std::string::npos;
    });
    return kSupported;
}

// Methods from ::vendor::lineage::touch::V1_0::ITouchscreenGesture follow.
Return<void> TouchscreenGesture::getSupportedGestures(getSupportedGestures_cb resultCb) {
    std::vector<Gesture> gestures;

    if (isSupported()) {
        for (const auto& entry : kGestureInfoMap) {
            gestures.push_back({entry.first, entry.second.name, entry.second.keycode});
        }
    } else {
        LOG(ERROR) << __func__ << ": Unsupported";
    }
    resultCb(gestures);

    return Void();
}

Return<bool> TouchscreenGesture::setGestureEnabled(
    const ::vendor::lineage::touch::V1_0::Gesture&, bool enabled) {

    if (isSupported()) {
        std::ofstream file(kTspPath);
        if (file.is_open()) {
            file << "singletap_enable," << enabled;
            return true;
        }
    } else {
        LOG(ERROR) << __func__ << ": Unsupported";
    }
    return false;
}

// Methods from ::android::hidl::base::V1_0::IBase follow.

}  // namespace samsung
}  // namespace V1_0
}  // namespace touch
}  // namespace lineage
}  // namespace vendor
