/*
 * Copyright 2023 The Android Open Source Project
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

#include <android-base/thread_annotations.h>
#include <sys/types.h>
#include <optional>
#include <unordered_map>
#include <vector>

namespace android::scheduler {
class SmallAreaDetectionAllowMappings {
    using AppIdThresholdMap = std::unordered_map<int32_t, float>;

public:
    void update(std::vector<std::pair<int32_t, float>>& appIdThresholdMappings);
    void setThesholdForAppId(int32_t appId, float threshold) EXCLUDES(mLock);
    std::optional<float> getThresholdForAppId(int32_t uid) EXCLUDES(mLock);

private:
    static bool isValidThreshold(float threshold) { return threshold >= 0.0f && threshold <= 1.0f; }
    mutable std::mutex mLock;
    AppIdThresholdMap mMap GUARDED_BY(mLock);
};
} // namespace android::scheduler
