/*
 * Copyright 2022 The Android Open Source Project
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

#include <array>
#include <list>
#include <memory>

#include <PointerControllerInterface.h>
#include <utils/Timers.h>

#include "EventHub.h"
#include "InputDevice.h"
#include "InputReaderContext.h"
#include "NotifyArgs.h"
#include "ui/Rotation.h"

#include "include/gestures.h"

namespace android {

// Converts Gesture structs from the gestures library into NotifyArgs and the appropriate
// PointerController calls.
class GestureConverter {
public:
    GestureConverter(InputReaderContext& readerContext, const InputDeviceContext& deviceContext,
                     int32_t deviceId);

    void setOrientation(ui::Rotation orientation) { mOrientation = orientation; }
    void reset();

    [[nodiscard]] std::list<NotifyArgs> handleGesture(nsecs_t when, nsecs_t readTime,
                                                      const Gesture& gesture);

private:
    NotifyArgs handleMove(nsecs_t when, nsecs_t readTime, const Gesture& gesture);
    [[nodiscard]] std::list<NotifyArgs> handleButtonsChange(nsecs_t when, nsecs_t readTime,
                                                            const Gesture& gesture);
    [[nodiscard]] std::list<NotifyArgs> handleMultiFingerSwipe(nsecs_t when, nsecs_t readTime,
                                                               uint32_t fingerCount, float dx,
                                                               float dy);
    [[nodiscard]] std::list<NotifyArgs> handleMultiFingerSwipeLift(nsecs_t when, nsecs_t readTime);

    NotifyMotionArgs makeMotionArgs(nsecs_t when, nsecs_t readTime, int32_t action,
                                    int32_t actionButton, int32_t buttonState,
                                    uint32_t pointerCount,
                                    const PointerProperties* pointerProperties,
                                    const PointerCoords* pointerCoords, float xCursorPosition,
                                    float yCursorPosition);

    const int32_t mDeviceId;
    InputReaderContext& mReaderContext;
    std::shared_ptr<PointerControllerInterface> mPointerController;

    ui::Rotation mOrientation = ui::ROTATION_0;
    RawAbsoluteAxisInfo mXAxisInfo;
    RawAbsoluteAxisInfo mYAxisInfo;

    // The current button state according to the gestures library, but converted into MotionEvent
    // button values (AMOTION_EVENT_BUTTON_...).
    uint32_t mButtonState = 0;
    nsecs_t mDownTime = 0;

    MotionClassification mCurrentClassification = MotionClassification::NONE;
    uint32_t mSwipeFingerCount = 0;
    static constexpr size_t MAX_FAKE_FINGERS = 4;
    // We never need any PointerProperties other than the finger tool type, so we can just keep a
    // const array of them.
    const std::array<PointerProperties, MAX_FAKE_FINGERS> mFingerProps = {{
            {.id = 0, .toolType = AMOTION_EVENT_TOOL_TYPE_FINGER},
            {.id = 1, .toolType = AMOTION_EVENT_TOOL_TYPE_FINGER},
            {.id = 2, .toolType = AMOTION_EVENT_TOOL_TYPE_FINGER},
            {.id = 3, .toolType = AMOTION_EVENT_TOOL_TYPE_FINGER},
    }};
    std::array<PointerCoords, MAX_FAKE_FINGERS> mFakeFingerCoords = {};
};

} // namespace android
