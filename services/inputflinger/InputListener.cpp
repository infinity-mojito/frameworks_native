/*
 * Copyright (C) 2011 The Android Open Source Project
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

#define LOG_TAG "InputListener"

#define ATRACE_TAG ATRACE_TAG_INPUT

//#define LOG_NDEBUG 0

#include "InputListener.h"

#include <android-base/stringprintf.h>
#include <android/log.h>
#include <math.h>
#include <utils/Trace.h>

using android::base::StringPrintf;

namespace android {

// --- NotifyConfigurationChangedArgs ---

NotifyConfigurationChangedArgs::NotifyConfigurationChangedArgs(int32_t id, nsecs_t eventTime)
      : id(id), eventTime(eventTime) {}

// --- NotifyKeyArgs ---

NotifyKeyArgs::NotifyKeyArgs(int32_t id, nsecs_t eventTime, nsecs_t readTime, int32_t deviceId,
                             uint32_t source, int32_t displayId, uint32_t policyFlags,
                             int32_t action, int32_t flags, int32_t keyCode, int32_t scanCode,
                             int32_t metaState, nsecs_t downTime)
      : id(id),
        eventTime(eventTime),
        deviceId(deviceId),
        source(source),
        displayId(displayId),
        policyFlags(policyFlags),
        action(action),
        flags(flags),
        keyCode(keyCode),
        scanCode(scanCode),
        metaState(metaState),
        downTime(downTime),
        readTime(readTime) {}

// --- NotifyMotionArgs ---

NotifyMotionArgs::NotifyMotionArgs(
        int32_t id, nsecs_t eventTime, nsecs_t readTime, int32_t deviceId, uint32_t source,
        int32_t displayId, uint32_t policyFlags, int32_t action, int32_t actionButton,
        int32_t flags, int32_t metaState, int32_t buttonState, MotionClassification classification,
        int32_t edgeFlags, uint32_t pointerCount, const PointerProperties* pointerProperties,
        const PointerCoords* pointerCoords, float xPrecision, float yPrecision,
        float xCursorPosition, float yCursorPosition, nsecs_t downTime,
        const std::vector<TouchVideoFrame>& videoFrames)
      : id(id),
        eventTime(eventTime),
        deviceId(deviceId),
        source(source),
        displayId(displayId),
        policyFlags(policyFlags),
        action(action),
        actionButton(actionButton),
        flags(flags),
        metaState(metaState),
        buttonState(buttonState),
        classification(classification),
        edgeFlags(edgeFlags),
        pointerCount(pointerCount),
        xPrecision(xPrecision),
        yPrecision(yPrecision),
        xCursorPosition(xCursorPosition),
        yCursorPosition(yCursorPosition),
        downTime(downTime),
        readTime(readTime),
        videoFrames(videoFrames) {
    for (uint32_t i = 0; i < pointerCount; i++) {
        this->pointerProperties[i].copyFrom(pointerProperties[i]);
        this->pointerCoords[i].copyFrom(pointerCoords[i]);
    }
}

NotifyMotionArgs::NotifyMotionArgs(const NotifyMotionArgs& other)
      : id(other.id),
        eventTime(other.eventTime),
        deviceId(other.deviceId),
        source(other.source),
        displayId(other.displayId),
        policyFlags(other.policyFlags),
        action(other.action),
        actionButton(other.actionButton),
        flags(other.flags),
        metaState(other.metaState),
        buttonState(other.buttonState),
        classification(other.classification),
        edgeFlags(other.edgeFlags),
        pointerCount(other.pointerCount),
        xPrecision(other.xPrecision),
        yPrecision(other.yPrecision),
        xCursorPosition(other.xCursorPosition),
        yCursorPosition(other.yCursorPosition),
        downTime(other.downTime),
        readTime(other.readTime),
        videoFrames(other.videoFrames) {
    for (uint32_t i = 0; i < pointerCount; i++) {
        pointerProperties[i].copyFrom(other.pointerProperties[i]);
        pointerCoords[i].copyFrom(other.pointerCoords[i]);
    }
}

static inline bool isCursorPositionEqual(float lhs, float rhs) {
    return (isnan(lhs) && isnan(rhs)) || lhs == rhs;
}

bool NotifyMotionArgs::operator==(const NotifyMotionArgs& rhs) const {
    bool equal = id == rhs.id && eventTime == rhs.eventTime && readTime == rhs.readTime &&
            deviceId == rhs.deviceId && source == rhs.source && displayId == rhs.displayId &&
            policyFlags == rhs.policyFlags && action == rhs.action &&
            actionButton == rhs.actionButton && flags == rhs.flags && metaState == rhs.metaState &&
            buttonState == rhs.buttonState && classification == rhs.classification &&
            edgeFlags == rhs.edgeFlags &&
            pointerCount == rhs.pointerCount
            // PointerProperties and PointerCoords are compared separately below
            && xPrecision == rhs.xPrecision && yPrecision == rhs.yPrecision &&
            isCursorPositionEqual(xCursorPosition, rhs.xCursorPosition) &&
            isCursorPositionEqual(yCursorPosition, rhs.yCursorPosition) &&
            downTime == rhs.downTime && videoFrames == rhs.videoFrames;
    if (!equal) {
        return false;
    }

    for (size_t i = 0; i < pointerCount; i++) {
        equal =
                pointerProperties[i] == rhs.pointerProperties[i]
                && pointerCoords[i] == rhs.pointerCoords[i];
        if (!equal) {
            return false;
        }
    }
    return true;
}

std::string NotifyMotionArgs::dump() const {
    std::string coords;
    for (uint32_t i = 0; i < pointerCount; i++) {
        if (!coords.empty()) {
            coords += ", ";
        }
        coords += StringPrintf("{%" PRIu32 ": ", i);
        coords +=
                StringPrintf("id=%" PRIu32 " x=%.1f y=%.1f pressure=%.1f", pointerProperties[i].id,
                             pointerCoords[i].getX(), pointerCoords[i].getY(),
                             pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_PRESSURE));
        const int32_t toolType = pointerProperties[i].toolType;
        if (toolType != AMOTION_EVENT_TOOL_TYPE_FINGER) {
            coords += StringPrintf(" toolType=%s", motionToolTypeToString(toolType));
        }
        const float major = pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_TOUCH_MAJOR);
        const float minor = pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_TOUCH_MINOR);
        const float orientation = pointerCoords[i].getAxisValue(AMOTION_EVENT_AXIS_ORIENTATION);
        if (major != 0 || minor != 0) {
            coords += StringPrintf(" major=%.1f minor=%.1f orientation=%.1f", major, minor,
                                   orientation);
        }
        coords += "}";
    }
    return StringPrintf("NotifyMotionArgs(id=%" PRId32 ", eventTime=%" PRId64 ", deviceId=%" PRId32
                        ", source=%s, action=%s, pointerCount=%" PRIu32
                        " pointers=%s, flags=0x%08x)",
                        id, eventTime, deviceId, inputEventSourceToString(source).c_str(),
                        MotionEvent::actionToString(action).c_str(), pointerCount, coords.c_str(),
                        flags);
}

// --- NotifySwitchArgs ---

NotifySwitchArgs::NotifySwitchArgs(int32_t id, nsecs_t eventTime, uint32_t policyFlags,
                                   uint32_t switchValues, uint32_t switchMask)
      : id(id),
        eventTime(eventTime),
        policyFlags(policyFlags),
        switchValues(switchValues),
        switchMask(switchMask) {}

// --- NotifySensorArgs ---

NotifySensorArgs::NotifySensorArgs(int32_t id, nsecs_t eventTime, int32_t deviceId, uint32_t source,
                                   InputDeviceSensorType sensorType,
                                   InputDeviceSensorAccuracy accuracy, bool accuracyChanged,
                                   nsecs_t hwTimestamp, std::vector<float> values)
      : id(id),
        eventTime(eventTime),
        deviceId(deviceId),
        source(source),
        sensorType(sensorType),
        accuracy(accuracy),
        accuracyChanged(accuracyChanged),
        hwTimestamp(hwTimestamp),
        values(std::move(values)) {}

// --- NotifyVibratorStateArgs ---

NotifyVibratorStateArgs::NotifyVibratorStateArgs(int32_t id, nsecs_t eventTime, int32_t deviceId,
                                                 bool isOn)
      : id(id), eventTime(eventTime), deviceId(deviceId), isOn(isOn) {}

NotifyVibratorStateArgs::NotifyVibratorStateArgs(const NotifyVibratorStateArgs& other)
      : id(other.id), eventTime(other.eventTime), deviceId(other.deviceId), isOn(other.isOn) {}

// --- NotifyDeviceResetArgs ---

NotifyDeviceResetArgs::NotifyDeviceResetArgs(int32_t id, nsecs_t eventTime, int32_t deviceId)
      : id(id), eventTime(eventTime), deviceId(deviceId) {}

// --- NotifyPointerCaptureChangedArgs ---

NotifyPointerCaptureChangedArgs::NotifyPointerCaptureChangedArgs(
        int32_t id, nsecs_t eventTime, const PointerCaptureRequest& request)
      : id(id), eventTime(eventTime), request(request) {}

// --- InputListenerInterface ---

// Helper to std::visit with lambdas.
template <typename... V>
struct Visitor : V... {};
// explicit deduction guide (not needed as of C++20)
template <typename... V>
Visitor(V...) -> Visitor<V...>;

void InputListenerInterface::notify(const NotifyArgs& generalArgs) {
    Visitor v{
            [&](const NotifyConfigurationChangedArgs& args) { notifyConfigurationChanged(&args); },
            [&](const NotifyKeyArgs& args) { notifyKey(&args); },
            [&](const NotifyMotionArgs& args) { notifyMotion(&args); },
            [&](const NotifySwitchArgs& args) { notifySwitch(&args); },
            [&](const NotifySensorArgs& args) { notifySensor(&args); },
            [&](const NotifyVibratorStateArgs& args) { notifyVibratorState(&args); },
            [&](const NotifyDeviceResetArgs& args) { notifyDeviceReset(&args); },
            [&](const NotifyPointerCaptureChangedArgs& args) {
                notifyPointerCaptureChanged(&args);
            },
    };
    std::visit(v, generalArgs);
}

// --- QueuedInputListener ---

static inline void traceEvent(const char* functionName, int32_t id) {
    if (ATRACE_ENABLED()) {
        std::string message = StringPrintf("%s(id=0x%" PRIx32 ")", functionName, id);
        ATRACE_NAME(message.c_str());
    }
}

QueuedInputListener::QueuedInputListener(InputListenerInterface& innerListener)
      : mInnerListener(innerListener) {}

void QueuedInputListener::notifyConfigurationChanged(
        const NotifyConfigurationChangedArgs* args) {
    traceEvent(__func__, args->id);
    mArgsQueue.emplace_back(*args);
}

void QueuedInputListener::notifyKey(const NotifyKeyArgs* args) {
    traceEvent(__func__, args->id);
    mArgsQueue.emplace_back(*args);
}

void QueuedInputListener::notifyMotion(const NotifyMotionArgs* args) {
    traceEvent(__func__, args->id);
    mArgsQueue.emplace_back(*args);
}

void QueuedInputListener::notifySwitch(const NotifySwitchArgs* args) {
    traceEvent(__func__, args->id);
    mArgsQueue.emplace_back(*args);
}

void QueuedInputListener::notifySensor(const NotifySensorArgs* args) {
    traceEvent(__func__, args->id);
    mArgsQueue.emplace_back(*args);
}

void QueuedInputListener::notifyVibratorState(const NotifyVibratorStateArgs* args) {
    traceEvent(__func__, args->id);
    mArgsQueue.emplace_back(*args);
}

void QueuedInputListener::notifyDeviceReset(const NotifyDeviceResetArgs* args) {
    traceEvent(__func__, args->id);
    mArgsQueue.emplace_back(*args);
}

void QueuedInputListener::notifyPointerCaptureChanged(const NotifyPointerCaptureChangedArgs* args) {
    traceEvent(__func__, args->id);
    mArgsQueue.emplace_back(*args);
}

void QueuedInputListener::flush() {
    for (const NotifyArgs& args : mArgsQueue) {
        mInnerListener.notify(args);
    }
    mArgsQueue.clear();
}

} // namespace android
