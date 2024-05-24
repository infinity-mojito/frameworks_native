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

#include "InjectionState.h"
#include "InputTargetFlags.h"
#include "trace/EventTrackerInterface.h"

#include <gui/InputApplication.h>
#include <input/Input.h>
#include <stdint.h>
#include <utils/Timers.h>
#include <functional>
#include <ostream>
#include <string>

namespace android::inputdispatcher {

struct EventEntry {
    enum class Type {
        CONFIGURATION_CHANGED,
        DEVICE_RESET,
        FOCUS,
        KEY,
        MOTION,
        SENSOR,
        POINTER_CAPTURE_CHANGED,
        DRAG,
        TOUCH_MODE_CHANGED,

        ftl_last = TOUCH_MODE_CHANGED
    };

    int32_t id;
    Type type;
    nsecs_t eventTime;
    uint32_t policyFlags;
    std::shared_ptr<InjectionState> injectionState;

    mutable bool dispatchInProgress; // initially false, set to true while dispatching

    /**
     * Injected keys are events from an external (probably untrusted) application
     * and are not related to real hardware state. They come in via
     * InputDispatcher::injectInputEvent, which sets policy flag POLICY_FLAG_INJECTED.
     */
    inline bool isInjected() const { return injectionState != nullptr; }

    /**
     * Synthesized events are either injected events, or events that come
     * from real hardware, but aren't directly attributable to a specific hardware event.
     * Key repeat is a synthesized event, because it is related to an actual hardware state
     * (a key is currently pressed), but the repeat itself is generated by the framework.
     */
    inline bool isSynthesized() const {
        return isInjected() || IdGenerator::getSource(id) != IdGenerator::Source::INPUT_READER;
    }

    virtual std::string getDescription() const = 0;

    EventEntry(int32_t id, Type type, nsecs_t eventTime, uint32_t policyFlags);
    EventEntry(const EventEntry&) = delete;
    EventEntry& operator=(const EventEntry&) = delete;
    virtual ~EventEntry() = default;
};

struct ConfigurationChangedEntry : EventEntry {
    explicit ConfigurationChangedEntry(int32_t id, nsecs_t eventTime);
    std::string getDescription() const override;
};

struct DeviceResetEntry : EventEntry {
    int32_t deviceId;

    DeviceResetEntry(int32_t id, nsecs_t eventTime, int32_t deviceId);
    std::string getDescription() const override;
};

struct FocusEntry : EventEntry {
    sp<IBinder> connectionToken;
    bool hasFocus;
    std::string reason;

    FocusEntry(int32_t id, nsecs_t eventTime, sp<IBinder> connectionToken, bool hasFocus,
               const std::string& reason);
    std::string getDescription() const override;
};

struct PointerCaptureChangedEntry : EventEntry {
    const PointerCaptureRequest pointerCaptureRequest;

    PointerCaptureChangedEntry(int32_t id, nsecs_t eventTime, const PointerCaptureRequest&);
    std::string getDescription() const override;
};

struct DragEntry : EventEntry {
    sp<IBinder> connectionToken;
    bool isExiting;
    float x, y;

    DragEntry(int32_t id, nsecs_t eventTime, sp<IBinder> connectionToken, bool isExiting, float x,
              float y);
    std::string getDescription() const override;
};

struct KeyEntry : EventEntry {
    int32_t deviceId;
    uint32_t source;
    int32_t displayId;
    int32_t action;
    int32_t keyCode;
    int32_t scanCode;
    int32_t metaState;
    nsecs_t downTime;
    std::unique_ptr<trace::EventTrackerInterface> traceTracker;

    bool syntheticRepeat; // set to true for synthetic key repeats

    enum class InterceptKeyResult {
        UNKNOWN,
        SKIP,
        CONTINUE,
        TRY_AGAIN_LATER,
    };
    // These are special fields that may need to be modified while the event is being dispatched.
    mutable InterceptKeyResult interceptKeyResult; // set based on the interception result
    mutable nsecs_t interceptKeyWakeupTime;        // used with INTERCEPT_KEY_RESULT_TRY_AGAIN_LATER
    mutable int32_t flags;
    mutable int32_t repeatCount;

    KeyEntry(int32_t id, std::shared_ptr<InjectionState> injectionState, nsecs_t eventTime,
             int32_t deviceId, uint32_t source, int32_t displayId, uint32_t policyFlags,
             int32_t action, int32_t flags, int32_t keyCode, int32_t scanCode, int32_t metaState,
             int32_t repeatCount, nsecs_t downTime);
    std::string getDescription() const override;
};

std::ostream& operator<<(std::ostream& out, const KeyEntry& motionEntry);

struct MotionEntry : EventEntry {
    int32_t deviceId;
    uint32_t source;
    int32_t displayId;
    int32_t action;
    int32_t actionButton;
    int32_t flags;
    int32_t metaState;
    int32_t buttonState;
    MotionClassification classification;
    int32_t edgeFlags;
    float xPrecision;
    float yPrecision;
    float xCursorPosition;
    float yCursorPosition;
    nsecs_t downTime;
    std::vector<PointerProperties> pointerProperties;
    std::vector<PointerCoords> pointerCoords;
    std::unique_ptr<trace::EventTrackerInterface> traceTracker;

    size_t getPointerCount() const { return pointerProperties.size(); }

    MotionEntry(int32_t id, std::shared_ptr<InjectionState> injectionState, nsecs_t eventTime,
                int32_t deviceId, uint32_t source, int32_t displayId, uint32_t policyFlags,
                int32_t action, int32_t actionButton, int32_t flags, int32_t metaState,
                int32_t buttonState, MotionClassification classification, int32_t edgeFlags,
                float xPrecision, float yPrecision, float xCursorPosition, float yCursorPosition,
                nsecs_t downTime, const std::vector<PointerProperties>& pointerProperties,
                const std::vector<PointerCoords>& pointerCoords);
    std::string getDescription() const override;
};

std::ostream& operator<<(std::ostream& out, const MotionEntry& motionEntry);

struct SensorEntry : EventEntry {
    int32_t deviceId;
    uint32_t source;
    InputDeviceSensorType sensorType;
    InputDeviceSensorAccuracy accuracy;
    bool accuracyChanged;
    nsecs_t hwTimestamp;

    std::vector<float> values;

    SensorEntry(int32_t id, nsecs_t eventTime, int32_t deviceId, uint32_t source,
                uint32_t policyFlags, nsecs_t hwTimestamp, InputDeviceSensorType sensorType,
                InputDeviceSensorAccuracy accuracy, bool accuracyChanged,
                std::vector<float> values);
    std::string getDescription() const override;
};

struct TouchModeEntry : EventEntry {
    bool inTouchMode;
    int32_t displayId;

    TouchModeEntry(int32_t id, nsecs_t eventTime, bool inTouchMode, int32_t displayId);
    std::string getDescription() const override;
};

// Tracks the progress of dispatching a particular event to a particular connection.
struct DispatchEntry {
    const uint32_t seq; // unique sequence number, never 0

    std::shared_ptr<const EventEntry> eventEntry; // the event to dispatch
    const ftl::Flags<InputTargetFlags> targetFlags;
    ui::Transform transform;
    ui::Transform rawTransform;
    float globalScaleFactor;
    // Both deliveryTime and timeoutTime are only populated when the entry is sent to the app,
    // and will be undefined before that.
    nsecs_t deliveryTime; // time when the event was actually delivered
    // An ANR will be triggered if a response for this entry is not received by timeoutTime
    nsecs_t timeoutTime;

    int32_t resolvedFlags;

    // Information about the dispatch window used for tracing. We avoid holding a window handle
    // here because information in a window handle may be dynamically updated within the lifespan
    // of this dispatch entry.
    gui::Uid targetUid;
    int64_t vsyncId;
    // The window that this event is targeting. The only case when this windowId is not populated
    // is when dispatching an event to a global monitor.
    std::optional<int32_t> windowId;

    DispatchEntry(std::shared_ptr<const EventEntry> eventEntry,
                  ftl::Flags<InputTargetFlags> targetFlags, const ui::Transform& transform,
                  const ui::Transform& rawTransform, float globalScaleFactor, gui::Uid targetUid,
                  int64_t vsyncId, std::optional<int32_t> windowId);
    DispatchEntry(const DispatchEntry&) = delete;
    DispatchEntry& operator=(const DispatchEntry&) = delete;

    inline bool hasForegroundTarget() const {
        return targetFlags.test(InputTargetFlags::FOREGROUND);
    }

    inline bool isSplit() const { return targetFlags.test(InputTargetFlags::SPLIT); }

private:
    static volatile int32_t sNextSeqAtomic;

    static uint32_t nextSeq();
};

std::ostream& operator<<(std::ostream& out, const DispatchEntry& entry);

VerifiedKeyEvent verifiedKeyEventFromKeyEntry(const KeyEntry& entry);
VerifiedMotionEvent verifiedMotionEventFromMotionEntry(const MotionEntry& entry,
                                                       const ui::Transform& rawTransform);

} // namespace android::inputdispatcher
