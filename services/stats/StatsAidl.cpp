/*
 * Copyright (C) 2021 The Android Open Source Project
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

#define DEBUG false // STOPSHIP if true
#define LOG_TAG "StatsAidl"

#include <log/log.h>
#include <statslog.h>

#include "StatsAidl.h"

namespace aidl {
namespace android {
namespace frameworks {
namespace stats {

StatsHal::StatsHal() {}

ndk::ScopedAStatus StatsHal::reportVendorAtom(const VendorAtom& vendorAtom) {
    if (vendorAtom.atomId < 100000 || vendorAtom.atomId >= 200000) {
        ALOGE("Atom ID %ld is not a valid vendor atom ID", (long) vendorAtom.atomId);
        return ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(
            -1, "Not a valid vendor atom ID");
    }
    if (vendorAtom.reverseDomainName.length() > 50) {
        ALOGE("Vendor atom reverse domain name %s is too long.",
            vendorAtom.reverseDomainName.c_str());
        return ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(
            -1, "Vendor atom reverse domain name is too long");
    }
    AStatsEvent* event = AStatsEvent_obtain();
    AStatsEvent_setAtomId(event, vendorAtom.atomId);
    AStatsEvent_writeString(event, vendorAtom.reverseDomainName.c_str());
    for (const auto& atomValue : vendorAtom.values) {
        switch (atomValue.getTag()) {
            case VendorAtomValue::intValue:
                AStatsEvent_writeInt32(event,
                    atomValue.get<VendorAtomValue::intValue>());
                break;
            case VendorAtomValue::longValue:
                AStatsEvent_writeInt64(event,
                    atomValue.get<VendorAtomValue::longValue>());
                break;
            case VendorAtomValue::floatValue:
                AStatsEvent_writeFloat(event,
                    atomValue.get<VendorAtomValue::floatValue>());
                break;
            case VendorAtomValue::stringValue:
                AStatsEvent_writeString(event,
                    atomValue.get<VendorAtomValue::stringValue>().c_str());
                break;
            case VendorAtomValue::boolValue:
                AStatsEvent_writeBool(event,
                    atomValue.get<VendorAtomValue::boolValue>());
                break;
            case VendorAtomValue::repeatedIntValue: {
                const std::optional<std::vector<int>>& repeatedIntValue =
                        atomValue.get<VendorAtomValue::repeatedIntValue>();
                AStatsEvent_writeInt32Array(event, repeatedIntValue->data(),
                                            repeatedIntValue->size());
                break;
            }
            case VendorAtomValue::repeatedLongValue: {
                const std::optional<std::vector<int64_t>>& repeatedLongValue =
                        atomValue.get<VendorAtomValue::repeatedLongValue>();
                AStatsEvent_writeInt64Array(event, repeatedLongValue->data(),
                                            repeatedLongValue->size());
                break;
            }
            case VendorAtomValue::repeatedFloatValue: {
                const std::optional<std::vector<float>>& repeatedFloatValue =
                        atomValue.get<VendorAtomValue::repeatedFloatValue>();
                AStatsEvent_writeFloatArray(event, repeatedFloatValue->data(),
                                            repeatedFloatValue->size());
                break;
            }
            case VendorAtomValue::repeatedStringValue: {
                const std::optional<std::vector<std::optional<std::string>>>& repeatedStringValue =
                        atomValue.get<VendorAtomValue::repeatedStringValue>();
                const std::vector<std::optional<std::string>>& repeatedStringVector =
                        *repeatedStringValue;
                const char* cStringArray[repeatedStringVector.size()];

                for (int i = 0; i < repeatedStringVector.size(); ++i) {
                    cStringArray[i] = repeatedStringVector[i]->c_str();
                }

                AStatsEvent_writeStringArray(event, cStringArray, repeatedStringVector.size());
                break;
            }
            case VendorAtomValue::repeatedBoolValue: {
                const std::optional<std::vector<bool>>& repeatedBoolValue =
                        atomValue.get<VendorAtomValue::repeatedBoolValue>();
                const std::vector<bool>& repeatedBoolVector = *repeatedBoolValue;
                bool boolArray[repeatedBoolValue->size()];

                for (int i = 0; i < repeatedBoolVector.size(); ++i) {
                    boolArray[i] = repeatedBoolVector[i];
                }

                AStatsEvent_writeBoolArray(event, boolArray, repeatedBoolVector.size());
                break;
            }
            case VendorAtomValue::byteArrayValue: {
                const std::optional<std::vector<uint8_t>>& byteArrayValue =
                        atomValue.get<VendorAtomValue::byteArrayValue>();

                AStatsEvent_writeByteArray(event, byteArrayValue->data(), byteArrayValue->size());
                break;
            }
        }
    }
    AStatsEvent_build(event);
    const int ret = AStatsEvent_write(event);
    AStatsEvent_release(event);

    return ret <= 0 ?
            ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(ret, "report atom failed") :
            ndk::ScopedAStatus::ok();
}

}  // namespace stats
}  // namespace frameworks
}  // namespace android
}  // namespace aidl
