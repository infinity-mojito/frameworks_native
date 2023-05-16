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

#include <NotifyArgs.h>
#include <android/input.h>
#include <attestation/HmacKeyManager.h>
#include <input/Input.h>
#include <vector>

namespace android {

// An arbitrary device id.
static constexpr uint32_t DEFAULT_DEVICE_ID = 1;

// The default policy flags to use for event injection by tests.
static constexpr uint32_t DEFAULT_POLICY_FLAGS = POLICY_FLAG_FILTERED | POLICY_FLAG_PASS_TO_USER;

class PointerBuilder {
public:
    PointerBuilder(int32_t id, ToolType toolType) {
        mProperties.clear();
        mProperties.id = id;
        mProperties.toolType = toolType;
        mCoords.clear();
    }

    PointerBuilder& x(float x) { return axis(AMOTION_EVENT_AXIS_X, x); }

    PointerBuilder& y(float y) { return axis(AMOTION_EVENT_AXIS_Y, y); }

    PointerBuilder& axis(int32_t axis, float value) {
        mCoords.setAxisValue(axis, value);
        return *this;
    }

    PointerProperties buildProperties() const { return mProperties; }

    PointerCoords buildCoords() const { return mCoords; }

private:
    PointerProperties mProperties;
    PointerCoords mCoords;
};

class MotionEventBuilder {
public:
    MotionEventBuilder(int32_t action, int32_t source) {
        mAction = action;
        mSource = source;
        mEventTime = systemTime(SYSTEM_TIME_MONOTONIC);
        mDownTime = mEventTime;
    }

    MotionEventBuilder& deviceId(int32_t deviceId) {
        mDeviceId = deviceId;
        return *this;
    }

    MotionEventBuilder& downTime(nsecs_t downTime) {
        mDownTime = downTime;
        return *this;
    }

    MotionEventBuilder& eventTime(nsecs_t eventTime) {
        mEventTime = eventTime;
        return *this;
    }

    MotionEventBuilder& displayId(int32_t displayId) {
        mDisplayId = displayId;
        return *this;
    }

    MotionEventBuilder& actionButton(int32_t actionButton) {
        mActionButton = actionButton;
        return *this;
    }

    MotionEventBuilder& buttonState(int32_t buttonState) {
        mButtonState = buttonState;
        return *this;
    }

    MotionEventBuilder& rawXCursorPosition(float rawXCursorPosition) {
        mRawXCursorPosition = rawXCursorPosition;
        return *this;
    }

    MotionEventBuilder& rawYCursorPosition(float rawYCursorPosition) {
        mRawYCursorPosition = rawYCursorPosition;
        return *this;
    }

    MotionEventBuilder& pointer(PointerBuilder pointer) {
        mPointers.push_back(pointer);
        return *this;
    }

    MotionEventBuilder& addFlag(uint32_t flags) {
        mFlags |= flags;
        return *this;
    }

    MotionEvent build() {
        std::vector<PointerProperties> pointerProperties;
        std::vector<PointerCoords> pointerCoords;
        for (const PointerBuilder& pointer : mPointers) {
            pointerProperties.push_back(pointer.buildProperties());
            pointerCoords.push_back(pointer.buildCoords());
        }

        // Set mouse cursor position for the most common cases to avoid boilerplate.
        if (mSource == AINPUT_SOURCE_MOUSE &&
            !MotionEvent::isValidCursorPosition(mRawXCursorPosition, mRawYCursorPosition)) {
            mRawXCursorPosition = pointerCoords[0].getX();
            mRawYCursorPosition = pointerCoords[0].getY();
        }

        MotionEvent event;
        static const ui::Transform kIdentityTransform;
        event.initialize(InputEvent::nextId(), mDeviceId, mSource, mDisplayId, INVALID_HMAC,
                         mAction, mActionButton, mFlags, /*edgeFlags=*/0, AMETA_NONE, mButtonState,
                         MotionClassification::NONE, kIdentityTransform,
                         /*xPrecision=*/0, /*yPrecision=*/0, mRawXCursorPosition,
                         mRawYCursorPosition, kIdentityTransform, mDownTime, mEventTime,
                         mPointers.size(), pointerProperties.data(), pointerCoords.data());
        return event;
    }

private:
    int32_t mAction;
    int32_t mDeviceId{DEFAULT_DEVICE_ID};
    int32_t mSource;
    nsecs_t mDownTime;
    nsecs_t mEventTime;
    int32_t mDisplayId{ADISPLAY_ID_DEFAULT};
    int32_t mActionButton{0};
    int32_t mButtonState{0};
    int32_t mFlags{0};
    float mRawXCursorPosition{AMOTION_EVENT_INVALID_CURSOR_POSITION};
    float mRawYCursorPosition{AMOTION_EVENT_INVALID_CURSOR_POSITION};

    std::vector<PointerBuilder> mPointers;
};

class MotionArgsBuilder {
public:
    MotionArgsBuilder(int32_t action, int32_t source) {
        mAction = action;
        mSource = source;
        mEventTime = systemTime(SYSTEM_TIME_MONOTONIC);
        mDownTime = mEventTime;
    }

    MotionArgsBuilder& deviceId(int32_t deviceId) {
        mDeviceId = deviceId;
        return *this;
    }

    MotionArgsBuilder& downTime(nsecs_t downTime) {
        mDownTime = downTime;
        return *this;
    }

    MotionArgsBuilder& eventTime(nsecs_t eventTime) {
        mEventTime = eventTime;
        return *this;
    }

    MotionArgsBuilder& displayId(int32_t displayId) {
        mDisplayId = displayId;
        return *this;
    }

    MotionArgsBuilder& policyFlags(int32_t policyFlags) {
        mPolicyFlags = policyFlags;
        return *this;
    }

    MotionArgsBuilder& actionButton(int32_t actionButton) {
        mActionButton = actionButton;
        return *this;
    }

    MotionArgsBuilder& buttonState(int32_t buttonState) {
        mButtonState = buttonState;
        return *this;
    }

    MotionArgsBuilder& rawXCursorPosition(float rawXCursorPosition) {
        mRawXCursorPosition = rawXCursorPosition;
        return *this;
    }

    MotionArgsBuilder& rawYCursorPosition(float rawYCursorPosition) {
        mRawYCursorPosition = rawYCursorPosition;
        return *this;
    }

    MotionArgsBuilder& pointer(PointerBuilder pointer) {
        mPointers.push_back(pointer);
        return *this;
    }

    MotionArgsBuilder& addFlag(uint32_t flags) {
        mFlags |= flags;
        return *this;
    }

    MotionArgsBuilder& classification(MotionClassification classification) {
        mClassification = classification;
        return *this;
    }

    NotifyMotionArgs build() {
        std::vector<PointerProperties> pointerProperties;
        std::vector<PointerCoords> pointerCoords;
        for (const PointerBuilder& pointer : mPointers) {
            pointerProperties.push_back(pointer.buildProperties());
            pointerCoords.push_back(pointer.buildCoords());
        }

        // Set mouse cursor position for the most common cases to avoid boilerplate.
        if (mSource == AINPUT_SOURCE_MOUSE &&
            !MotionEvent::isValidCursorPosition(mRawXCursorPosition, mRawYCursorPosition)) {
            mRawXCursorPosition = pointerCoords[0].getX();
            mRawYCursorPosition = pointerCoords[0].getY();
        }

        return {InputEvent::nextId(),
                mEventTime,
                /*readTime=*/mEventTime,
                mDeviceId,
                mSource,
                mDisplayId,
                mPolicyFlags,
                mAction,
                mActionButton,
                mFlags,
                AMETA_NONE,
                mButtonState,
                mClassification,
                /*edgeFlags=*/0,
                static_cast<uint32_t>(mPointers.size()),
                pointerProperties.data(),
                pointerCoords.data(),
                /*xPrecision=*/0,
                /*yPrecision=*/0,
                mRawXCursorPosition,
                mRawYCursorPosition,
                mDownTime,
                /*videoFrames=*/{}};
    }

private:
    int32_t mAction;
    int32_t mDeviceId{DEFAULT_DEVICE_ID};
    uint32_t mSource;
    nsecs_t mDownTime;
    nsecs_t mEventTime;
    int32_t mDisplayId{ADISPLAY_ID_DEFAULT};
    uint32_t mPolicyFlags = DEFAULT_POLICY_FLAGS;
    int32_t mActionButton{0};
    int32_t mButtonState{0};
    int32_t mFlags{0};
    MotionClassification mClassification{MotionClassification::NONE};
    float mRawXCursorPosition{AMOTION_EVENT_INVALID_CURSOR_POSITION};
    float mRawYCursorPosition{AMOTION_EVENT_INVALID_CURSOR_POSITION};

    std::vector<PointerBuilder> mPointers;
};

} // namespace android
