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

#include "TouchButtonAccumulator.h"

#include "EventHub.h"
#include "InputDevice.h"

namespace android {

void TouchButtonAccumulator::configure() {
    mHaveBtnTouch = mDeviceContext.hasScanCode(BTN_TOUCH);
    mHaveStylus = mDeviceContext.hasScanCode(BTN_TOOL_PEN) ||
            mDeviceContext.hasScanCode(BTN_TOOL_RUBBER) ||
            mDeviceContext.hasScanCode(BTN_TOOL_BRUSH) ||
            mDeviceContext.hasScanCode(BTN_TOOL_PENCIL) ||
            mDeviceContext.hasScanCode(BTN_TOOL_AIRBRUSH);
}

void TouchButtonAccumulator::reset() {
    mBtnTouch = mDeviceContext.isKeyPressed(BTN_TOUCH);
    mBtnStylus = mDeviceContext.isKeyPressed(BTN_STYLUS) ||
            mDeviceContext.isKeyCodePressed(AKEYCODE_STYLUS_BUTTON_PRIMARY);
    // BTN_0 is what gets mapped for the HID usage Digitizers.SecondaryBarrelSwitch
    mBtnStylus2 = mDeviceContext.isKeyPressed(BTN_STYLUS2) || mDeviceContext.isKeyPressed(BTN_0) ||
            mDeviceContext.isKeyCodePressed(AKEYCODE_STYLUS_BUTTON_SECONDARY);
    mBtnToolFinger = mDeviceContext.isKeyPressed(BTN_TOOL_FINGER);
    mBtnToolPen = mDeviceContext.isKeyPressed(BTN_TOOL_PEN);
    mBtnToolRubber = mDeviceContext.isKeyPressed(BTN_TOOL_RUBBER);
    mBtnToolBrush = mDeviceContext.isKeyPressed(BTN_TOOL_BRUSH);
    mBtnToolPencil = mDeviceContext.isKeyPressed(BTN_TOOL_PENCIL);
    mBtnToolAirbrush = mDeviceContext.isKeyPressed(BTN_TOOL_AIRBRUSH);
    mBtnToolMouse = mDeviceContext.isKeyPressed(BTN_TOOL_MOUSE);
    mBtnToolLens = mDeviceContext.isKeyPressed(BTN_TOOL_LENS);
    mBtnToolDoubleTap = mDeviceContext.isKeyPressed(BTN_TOOL_DOUBLETAP);
    mBtnToolTripleTap = mDeviceContext.isKeyPressed(BTN_TOOL_TRIPLETAP);
    mBtnToolQuadTap = mDeviceContext.isKeyPressed(BTN_TOOL_QUADTAP);
    mHidUsageAccumulator.reset();
}

void TouchButtonAccumulator::process(const RawEvent* rawEvent) {
    mHidUsageAccumulator.process(*rawEvent);

    if (rawEvent->type == EV_KEY) {
        switch (rawEvent->code) {
            case BTN_TOUCH:
                mBtnTouch = rawEvent->value;
                break;
            case BTN_STYLUS:
                mBtnStylus = rawEvent->value;
                break;
            case BTN_STYLUS2:
            case BTN_0: // BTN_0 is what gets mapped for the HID usage
                        // Digitizers.SecondaryBarrelSwitch
                mBtnStylus2 = rawEvent->value;
                break;
            case BTN_TOOL_FINGER:
                mBtnToolFinger = rawEvent->value;
                break;
            case BTN_TOOL_PEN:
                mBtnToolPen = rawEvent->value;
                break;
            case BTN_TOOL_RUBBER:
                mBtnToolRubber = rawEvent->value;
                break;
            case BTN_TOOL_BRUSH:
                mBtnToolBrush = rawEvent->value;
                break;
            case BTN_TOOL_PENCIL:
                mBtnToolPencil = rawEvent->value;
                break;
            case BTN_TOOL_AIRBRUSH:
                mBtnToolAirbrush = rawEvent->value;
                break;
            case BTN_TOOL_MOUSE:
                mBtnToolMouse = rawEvent->value;
                break;
            case BTN_TOOL_LENS:
                mBtnToolLens = rawEvent->value;
                break;
            case BTN_TOOL_DOUBLETAP:
                mBtnToolDoubleTap = rawEvent->value;
                break;
            case BTN_TOOL_TRIPLETAP:
                mBtnToolTripleTap = rawEvent->value;
                break;
            case BTN_TOOL_QUADTAP:
                mBtnToolQuadTap = rawEvent->value;
                break;
            default:
                processMappedKey(rawEvent->code, rawEvent->value);
        }
        return;
    }
}

void TouchButtonAccumulator::processMappedKey(int32_t scanCode, bool down) {
    int32_t outKeyCode, outMetaState;
    uint32_t outFlags;
    if (mDeviceContext.mapKey(scanCode, mHidUsageAccumulator.consumeCurrentHidUsage(),
                              0 /*metaState*/, &outKeyCode, &outMetaState, &outFlags) != OK) {
        return;
    }
    switch (outKeyCode) {
        case AKEYCODE_STYLUS_BUTTON_PRIMARY:
            mBtnStylus = down;
            break;
        case AKEYCODE_STYLUS_BUTTON_SECONDARY:
            mBtnStylus2 = down;
            break;
        default:
            break;
    }
}

uint32_t TouchButtonAccumulator::getButtonState() const {
    uint32_t result = 0;
    if (mBtnStylus) {
        result |= AMOTION_EVENT_BUTTON_STYLUS_PRIMARY;
    }
    if (mBtnStylus2) {
        result |= AMOTION_EVENT_BUTTON_STYLUS_SECONDARY;
    }
    return result;
}

int32_t TouchButtonAccumulator::getToolType() const {
    if (mBtnToolMouse || mBtnToolLens) {
        return AMOTION_EVENT_TOOL_TYPE_MOUSE;
    }
    if (mBtnToolRubber) {
        return AMOTION_EVENT_TOOL_TYPE_ERASER;
    }
    if (mBtnToolPen || mBtnToolBrush || mBtnToolPencil || mBtnToolAirbrush) {
        return AMOTION_EVENT_TOOL_TYPE_STYLUS;
    }
    if (mBtnToolFinger || mBtnToolDoubleTap || mBtnToolTripleTap || mBtnToolQuadTap) {
        return AMOTION_EVENT_TOOL_TYPE_FINGER;
    }
    return AMOTION_EVENT_TOOL_TYPE_UNKNOWN;
}

bool TouchButtonAccumulator::isToolActive() const {
    return mBtnTouch || mBtnToolFinger || mBtnToolPen || mBtnToolRubber || mBtnToolBrush ||
            mBtnToolPencil || mBtnToolAirbrush || mBtnToolMouse || mBtnToolLens ||
            mBtnToolDoubleTap || mBtnToolTripleTap || mBtnToolQuadTap;
}

bool TouchButtonAccumulator::isHovering() const {
    return mHaveBtnTouch && !mBtnTouch;
}

bool TouchButtonAccumulator::hasStylus() const {
    return mHaveStylus;
}

} // namespace android
