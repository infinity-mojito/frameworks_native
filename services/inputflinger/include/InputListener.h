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

#pragma once

#include <vector>

#include <input/Input.h>
#include <input/InputDevice.h>
#include <input/TouchVideoFrame.h>

namespace android {

class InputListenerInterface;

/* Describes a configuration change event. */
struct NotifyConfigurationChangedArgs {
    int32_t id;
    nsecs_t eventTime;

    inline NotifyConfigurationChangedArgs() { }

    NotifyConfigurationChangedArgs(int32_t id, nsecs_t eventTime);

    bool operator==(const NotifyConfigurationChangedArgs& rhs) const = default;

    NotifyConfigurationChangedArgs(const NotifyConfigurationChangedArgs& other) = default;
};

/* Describes a key event. */
struct NotifyKeyArgs {
    int32_t id;
    nsecs_t eventTime;

    int32_t deviceId;
    uint32_t source;
    int32_t displayId;
    uint32_t policyFlags;
    int32_t action;
    int32_t flags;
    int32_t keyCode;
    int32_t scanCode;
    int32_t metaState;
    nsecs_t downTime;
    nsecs_t readTime;

    inline NotifyKeyArgs() { }

    NotifyKeyArgs(int32_t id, nsecs_t eventTime, nsecs_t readTime, int32_t deviceId,
                  uint32_t source, int32_t displayId, uint32_t policyFlags, int32_t action,
                  int32_t flags, int32_t keyCode, int32_t scanCode, int32_t metaState,
                  nsecs_t downTime);

    bool operator==(const NotifyKeyArgs& rhs) const = default;

    NotifyKeyArgs(const NotifyKeyArgs& other) = default;
};

/* Describes a motion event. */
struct NotifyMotionArgs {
    int32_t id;
    nsecs_t eventTime;

    int32_t deviceId;
    uint32_t source;
    int32_t displayId;
    uint32_t policyFlags;
    int32_t action;
    int32_t actionButton;
    int32_t flags;
    int32_t metaState;
    int32_t buttonState;
    /**
     * Classification of the current touch gesture
     */
    MotionClassification classification;
    int32_t edgeFlags;

    uint32_t pointerCount;
    PointerProperties pointerProperties[MAX_POINTERS];
    PointerCoords pointerCoords[MAX_POINTERS];
    float xPrecision;
    float yPrecision;
    /**
     * Mouse cursor position when this event is reported relative to the origin of the specified
     * display. Only valid if this is a mouse event (originates from a mouse or from a trackpad in
     * gestures enabled mode.
     */
    float xCursorPosition;
    float yCursorPosition;
    nsecs_t downTime;
    nsecs_t readTime;
    std::vector<TouchVideoFrame> videoFrames;

    inline NotifyMotionArgs() { }

    NotifyMotionArgs(int32_t id, nsecs_t eventTime, nsecs_t readTime, int32_t deviceId,
                     uint32_t source, int32_t displayId, uint32_t policyFlags, int32_t action,
                     int32_t actionButton, int32_t flags, int32_t metaState, int32_t buttonState,
                     MotionClassification classification, int32_t edgeFlags, uint32_t pointerCount,
                     const PointerProperties* pointerProperties, const PointerCoords* pointerCoords,
                     float xPrecision, float yPrecision, float xCursorPosition,
                     float yCursorPosition, nsecs_t downTime,
                     const std::vector<TouchVideoFrame>& videoFrames);

    NotifyMotionArgs(const NotifyMotionArgs& other);

    bool operator==(const NotifyMotionArgs& rhs) const;

    std::string dump() const;
};

/* Describes a sensor event. */
struct NotifySensorArgs {
    int32_t id;
    nsecs_t eventTime;

    int32_t deviceId;
    uint32_t source;
    InputDeviceSensorType sensorType;
    InputDeviceSensorAccuracy accuracy;
    bool accuracyChanged;
    nsecs_t hwTimestamp;
    std::vector<float> values;

    inline NotifySensorArgs() {}

    NotifySensorArgs(int32_t id, nsecs_t eventTime, int32_t deviceId, uint32_t source,
                     InputDeviceSensorType sensorType, InputDeviceSensorAccuracy accuracy,
                     bool accuracyChanged, nsecs_t hwTimestamp, std::vector<float> values);

    NotifySensorArgs(const NotifySensorArgs& other) = default;
};

/* Describes a switch event. */
struct NotifySwitchArgs {
    int32_t id;
    nsecs_t eventTime;

    uint32_t policyFlags;
    uint32_t switchValues;
    uint32_t switchMask;

    inline NotifySwitchArgs() { }

    NotifySwitchArgs(int32_t id, nsecs_t eventTime, uint32_t policyFlags, uint32_t switchValues,
                     uint32_t switchMask);

    NotifySwitchArgs(const NotifySwitchArgs& other) = default;

    bool operator==(const NotifySwitchArgs& rhs) const = default;
};

/* Describes a device reset event, such as when a device is added,
 * reconfigured, or removed. */
struct NotifyDeviceResetArgs {
    int32_t id;
    nsecs_t eventTime;

    int32_t deviceId;

    inline NotifyDeviceResetArgs() { }

    NotifyDeviceResetArgs(int32_t id, nsecs_t eventTime, int32_t deviceId);

    NotifyDeviceResetArgs(const NotifyDeviceResetArgs& other) = default;

    bool operator==(const NotifyDeviceResetArgs& rhs) const = default;
};

/* Describes a change in the state of Pointer Capture. */
struct NotifyPointerCaptureChangedArgs {
    // The sequence number of the Pointer Capture request, if enabled.
    int32_t id;
    nsecs_t eventTime;

    PointerCaptureRequest request;

    inline NotifyPointerCaptureChangedArgs() {}

    NotifyPointerCaptureChangedArgs(int32_t id, nsecs_t eventTime, const PointerCaptureRequest&);

    NotifyPointerCaptureChangedArgs(const NotifyPointerCaptureChangedArgs& other) = default;
};

/* Describes a vibrator state event. */
struct NotifyVibratorStateArgs {
    int32_t id;
    nsecs_t eventTime;

    int32_t deviceId;
    bool isOn;

    inline NotifyVibratorStateArgs() {}

    NotifyVibratorStateArgs(int32_t id, nsecs_t eventTIme, int32_t deviceId, bool isOn);

    NotifyVibratorStateArgs(const NotifyVibratorStateArgs& other);
};

using NotifyArgs = std::variant<NotifyConfigurationChangedArgs, NotifyKeyArgs, NotifyMotionArgs,
                                NotifySensorArgs, NotifySwitchArgs, NotifyDeviceResetArgs,
                                NotifyPointerCaptureChangedArgs, NotifyVibratorStateArgs>;

/*
 * The interface used by the InputReader to notify the InputListener about input events.
 */
class InputListenerInterface {
public:
    InputListenerInterface() { }
    InputListenerInterface(const InputListenerInterface&) = delete;
    InputListenerInterface& operator=(const InputListenerInterface&) = delete;
    virtual ~InputListenerInterface() { }

    virtual void notifyConfigurationChanged(const NotifyConfigurationChangedArgs* args) = 0;
    virtual void notifyKey(const NotifyKeyArgs* args) = 0;
    virtual void notifyMotion(const NotifyMotionArgs* args) = 0;
    virtual void notifySwitch(const NotifySwitchArgs* args) = 0;
    virtual void notifySensor(const NotifySensorArgs* args) = 0;
    virtual void notifyVibratorState(const NotifyVibratorStateArgs* args) = 0;
    virtual void notifyDeviceReset(const NotifyDeviceResetArgs* args) = 0;
    virtual void notifyPointerCaptureChanged(const NotifyPointerCaptureChangedArgs* args) = 0;
};

/*
 * An implementation of the listener interface that queues up and defers dispatch
 * of decoded events until flushed.
 */
class QueuedInputListener : public InputListenerInterface {

public:
    explicit QueuedInputListener(InputListenerInterface& innerListener);

    virtual void notifyConfigurationChanged(const NotifyConfigurationChangedArgs* args) override;
    virtual void notifyKey(const NotifyKeyArgs* args) override;
    virtual void notifyMotion(const NotifyMotionArgs* args) override;
    virtual void notifySwitch(const NotifySwitchArgs* args) override;
    virtual void notifySensor(const NotifySensorArgs* args) override;
    virtual void notifyDeviceReset(const NotifyDeviceResetArgs* args) override;
    void notifyVibratorState(const NotifyVibratorStateArgs* args) override;
    void notifyPointerCaptureChanged(const NotifyPointerCaptureChangedArgs* args) override;

    void flush();

private:
    InputListenerInterface& mInnerListener;
    std::vector<NotifyArgs> mArgsQueue;
};

} // namespace android
