/*
 * Copyright (C) 2019 The Android Open Source Project
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

#pragma once

#include <gui/WindowInfo.h>
#include <utils/BitSet.h>

namespace android {

namespace inputdispatcher {

// Focus tracking for touch.
struct TouchedWindow {
    sp<gui::WindowInfoHandle> windowHandle;
    int32_t targetFlags;
    BitSet32 pointerIds;
    bool isPilferingPointers = false;
    // Time at which the first action down occurred on this window.
    // NOTE: This is not initialized in case of HOVER entry/exit and DISPATCH_AS_OUTSIDE scenario.
    std::optional<nsecs_t> firstDownTimeInTarget;
    std::string dump() const;
};

} // namespace inputdispatcher
} // namespace android
