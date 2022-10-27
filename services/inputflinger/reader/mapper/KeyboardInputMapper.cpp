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

// clang-format off
#include "../Macros.h"
// clang-format on

#include "KeyboardInputMapper.h"

namespace android {

// --- Static Definitions ---

static int32_t rotateValueUsingRotationMap(int32_t value, int32_t orientation,
                                           const int32_t map[][4], size_t mapSize) {
    if (orientation != DISPLAY_ORIENTATION_0) {
        for (size_t i = 0; i < mapSize; i++) {
            if (value == map[i][0]) {
                return map[i][orientation];
            }
        }
    }
    return value;
}

static const int32_t keyCodeRotationMap[][4] = {
        // key codes enumerated counter-clockwise with the original (unrotated) key first
        // no rotation,        90 degree rotation,  180 degree rotation, 270 degree rotation
        {AKEYCODE_DPAD_DOWN, AKEYCODE_DPAD_RIGHT, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_LEFT},
        {AKEYCODE_DPAD_RIGHT, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_LEFT, AKEYCODE_DPAD_DOWN},
        {AKEYCODE_DPAD_UP, AKEYCODE_DPAD_LEFT, AKEYCODE_DPAD_DOWN, AKEYCODE_DPAD_RIGHT},
        {AKEYCODE_DPAD_LEFT, AKEYCODE_DPAD_DOWN, AKEYCODE_DPAD_RIGHT, AKEYCODE_DPAD_UP},
        {AKEYCODE_SYSTEM_NAVIGATION_DOWN, AKEYCODE_SYSTEM_NAVIGATION_RIGHT,
         AKEYCODE_SYSTEM_NAVIGATION_UP, AKEYCODE_SYSTEM_NAVIGATION_LEFT},
        {AKEYCODE_SYSTEM_NAVIGATION_RIGHT, AKEYCODE_SYSTEM_NAVIGATION_UP,
         AKEYCODE_SYSTEM_NAVIGATION_LEFT, AKEYCODE_SYSTEM_NAVIGATION_DOWN},
        {AKEYCODE_SYSTEM_NAVIGATION_UP, AKEYCODE_SYSTEM_NAVIGATION_LEFT,
         AKEYCODE_SYSTEM_NAVIGATION_DOWN, AKEYCODE_SYSTEM_NAVIGATION_RIGHT},
        {AKEYCODE_SYSTEM_NAVIGATION_LEFT, AKEYCODE_SYSTEM_NAVIGATION_DOWN,
         AKEYCODE_SYSTEM_NAVIGATION_RIGHT, AKEYCODE_SYSTEM_NAVIGATION_UP},
};

static const size_t keyCodeRotationMapSize =
        sizeof(keyCodeRotationMap) / sizeof(keyCodeRotationMap[0]);

static int32_t rotateKeyCode(int32_t keyCode, int32_t orientation) {
    return rotateValueUsingRotationMap(keyCode, orientation, keyCodeRotationMap,
                                       keyCodeRotationMapSize);
}

// --- KeyboardInputMapper ---

KeyboardInputMapper::KeyboardInputMapper(InputDeviceContext& deviceContext, uint32_t source,
                                         int32_t keyboardType)
      : InputMapper(deviceContext), mSource(source), mKeyboardType(keyboardType) {}

KeyboardInputMapper::~KeyboardInputMapper() {}

uint32_t KeyboardInputMapper::getSources() const {
    return mSource;
}

int32_t KeyboardInputMapper::getOrientation() {
    if (mViewport) {
        return mViewport->orientation;
    }
    return DISPLAY_ORIENTATION_0;
}

int32_t KeyboardInputMapper::getDisplayId() {
    if (mViewport) {
        return mViewport->displayId;
    }
    return ADISPLAY_ID_NONE;
}

void KeyboardInputMapper::populateDeviceInfo(InputDeviceInfo* info) {
    InputMapper::populateDeviceInfo(info);

    info->setKeyboardType(mKeyboardType);
    info->setKeyCharacterMap(getDeviceContext().getKeyCharacterMap());
}

void KeyboardInputMapper::dump(std::string& dump) {
    dump += INDENT2 "Keyboard Input Mapper:\n";
    dumpParameters(dump);
    dump += StringPrintf(INDENT3 "KeyboardType: %d\n", mKeyboardType);
    dump += StringPrintf(INDENT3 "Orientation: %d\n", getOrientation());
    dump += StringPrintf(INDENT3 "KeyDowns: %zu keys currently down\n", mKeyDowns.size());
    dump += StringPrintf(INDENT3 "MetaState: 0x%0x\n", mMetaState);
}

std::optional<DisplayViewport> KeyboardInputMapper::findViewport(
        nsecs_t when, const InputReaderConfiguration* config) {
    if (getDeviceContext().getAssociatedViewport()) {
        return getDeviceContext().getAssociatedViewport();
    }

    // No associated display defined, try to find default display if orientationAware.
    if (mParameters.orientationAware) {
        return config->getDisplayViewportByType(ViewportType::INTERNAL);
    }

    return std::nullopt;
}

std::list<NotifyArgs> KeyboardInputMapper::configure(nsecs_t when,
                                                     const InputReaderConfiguration* config,
                                                     uint32_t changes) {
    std::list<NotifyArgs> out = InputMapper::configure(when, config, changes);

    if (!changes) { // first time only
        // Configure basic parameters.
        configureParameters();
    }

    if (!changes || (changes & InputReaderConfiguration::CHANGE_DISPLAY_INFO)) {
        mViewport = findViewport(when, config);
    }
    return out;
}

void KeyboardInputMapper::configureParameters() {
    mParameters.orientationAware = false;
    const PropertyMap& config = getDeviceContext().getConfiguration();
    config.tryGetProperty("keyboard.orientationAware", mParameters.orientationAware);

    mParameters.handlesKeyRepeat = false;
    config.tryGetProperty("keyboard.handlesKeyRepeat", mParameters.handlesKeyRepeat);

    mParameters.doNotWakeByDefault = false;
    config.tryGetProperty("keyboard.doNotWakeByDefault", mParameters.doNotWakeByDefault);
}

void KeyboardInputMapper::dumpParameters(std::string& dump) {
    dump += INDENT3 "Parameters:\n";
    dump += StringPrintf(INDENT4 "OrientationAware: %s\n", toString(mParameters.orientationAware));
    dump += StringPrintf(INDENT4 "HandlesKeyRepeat: %s\n", toString(mParameters.handlesKeyRepeat));
}

std::list<NotifyArgs> KeyboardInputMapper::reset(nsecs_t when) {
    std::list<NotifyArgs> out = cancelAllDownKeys(when);
    mCurrentHidUsage = 0;

    resetLedState();

    out += InputMapper::reset(when);
    return out;
}

std::list<NotifyArgs> KeyboardInputMapper::process(const RawEvent* rawEvent) {
    std::list<NotifyArgs> out;
    switch (rawEvent->type) {
        case EV_KEY: {
            int32_t scanCode = rawEvent->code;
            int32_t usageCode = mCurrentHidUsage;
            mCurrentHidUsage = 0;

            if (isKeyboardOrGamepadKey(scanCode)) {
                out += processKey(rawEvent->when, rawEvent->readTime, rawEvent->value != 0,
                                  scanCode, usageCode);
            }
            break;
        }
        case EV_MSC: {
            if (rawEvent->code == MSC_SCAN) {
                mCurrentHidUsage = rawEvent->value;
            }
            break;
        }
        case EV_SYN: {
            if (rawEvent->code == SYN_REPORT) {
                mCurrentHidUsage = 0;
            }
        }
    }
    return out;
}

bool KeyboardInputMapper::isKeyboardOrGamepadKey(int32_t scanCode) {
    return scanCode < BTN_MOUSE || scanCode >= BTN_WHEEL ||
            (scanCode >= BTN_MISC && scanCode < BTN_MOUSE) ||
            (scanCode >= BTN_JOYSTICK && scanCode < BTN_DIGI);
}

bool KeyboardInputMapper::isMediaKey(int32_t keyCode) {
    switch (keyCode) {
        case AKEYCODE_MEDIA_PLAY:
        case AKEYCODE_MEDIA_PAUSE:
        case AKEYCODE_MEDIA_PLAY_PAUSE:
        case AKEYCODE_MUTE:
        case AKEYCODE_HEADSETHOOK:
        case AKEYCODE_MEDIA_STOP:
        case AKEYCODE_MEDIA_NEXT:
        case AKEYCODE_MEDIA_PREVIOUS:
        case AKEYCODE_MEDIA_REWIND:
        case AKEYCODE_MEDIA_RECORD:
        case AKEYCODE_MEDIA_FAST_FORWARD:
        case AKEYCODE_MEDIA_SKIP_FORWARD:
        case AKEYCODE_MEDIA_SKIP_BACKWARD:
        case AKEYCODE_MEDIA_STEP_FORWARD:
        case AKEYCODE_MEDIA_STEP_BACKWARD:
        case AKEYCODE_MEDIA_AUDIO_TRACK:
        case AKEYCODE_VOLUME_UP:
        case AKEYCODE_VOLUME_DOWN:
        case AKEYCODE_VOLUME_MUTE:
        case AKEYCODE_TV_AUDIO_DESCRIPTION:
        case AKEYCODE_TV_AUDIO_DESCRIPTION_MIX_UP:
        case AKEYCODE_TV_AUDIO_DESCRIPTION_MIX_DOWN:
            return true;
    }
    return false;
}

std::list<NotifyArgs> KeyboardInputMapper::processKey(nsecs_t when, nsecs_t readTime, bool down,
                                                      int32_t scanCode, int32_t usageCode) {
    std::list<NotifyArgs> out;
    int32_t keyCode;
    int32_t keyMetaState;
    uint32_t policyFlags;

    if (getDeviceContext().mapKey(scanCode, usageCode, mMetaState, &keyCode, &keyMetaState,
                                  &policyFlags)) {
        keyCode = AKEYCODE_UNKNOWN;
        keyMetaState = mMetaState;
        policyFlags = 0;
    }

    nsecs_t downTime = when;
    if (down) {
        // Rotate key codes according to orientation if needed.
        if (mParameters.orientationAware) {
            keyCode = rotateKeyCode(keyCode, getOrientation());
        }

        // Add key down.
        ssize_t keyDownIndex = findKeyDown(scanCode);
        if (keyDownIndex >= 0) {
            // key repeat, be sure to use same keycode as before in case of rotation
            keyCode = mKeyDowns[keyDownIndex].keyCode;
            downTime = mKeyDowns[keyDownIndex].downTime;
        } else {
            // key down
            if ((policyFlags & POLICY_FLAG_VIRTUAL) &&
                getContext()->shouldDropVirtualKey(when, keyCode, scanCode)) {
                return out;
            }
            if (policyFlags & POLICY_FLAG_GESTURE) {
                out += getDeviceContext().cancelTouch(when, readTime);
            }

            KeyDown keyDown;
            keyDown.keyCode = keyCode;
            keyDown.scanCode = scanCode;
            keyDown.downTime = when;
            mKeyDowns.push_back(keyDown);
        }
    } else {
        // Remove key down.
        ssize_t keyDownIndex = findKeyDown(scanCode);
        if (keyDownIndex >= 0) {
            // key up, be sure to use same keycode as before in case of rotation
            keyCode = mKeyDowns[keyDownIndex].keyCode;
            downTime = mKeyDowns[keyDownIndex].downTime;
            mKeyDowns.erase(mKeyDowns.begin() + (size_t)keyDownIndex);
        } else {
            // key was not actually down
            ALOGI("Dropping key up from device %s because the key was not down.  "
                  "keyCode=%d, scanCode=%d",
                  getDeviceName().c_str(), keyCode, scanCode);
            return out;
        }
    }

    if (updateMetaStateIfNeeded(keyCode, down)) {
        // If global meta state changed send it along with the key.
        // If it has not changed then we'll use what keymap gave us,
        // since key replacement logic might temporarily reset a few
        // meta bits for given key.
        keyMetaState = mMetaState;
    }

    // Key down on external an keyboard should wake the device.
    // We don't do this for internal keyboards to prevent them from waking up in your pocket.
    // For internal keyboards and devices for which the default wake behavior is explicitly
    // prevented (e.g. TV remotes), the key layout file should specify the policy flags for each
    // wake key individually.
    // TODO: Use the input device configuration to control this behavior more finely.
    if (down && getDeviceContext().isExternal() && !mParameters.doNotWakeByDefault &&
        !isMediaKey(keyCode)) {
        policyFlags |= POLICY_FLAG_WAKE;
    }

    if (mParameters.handlesKeyRepeat) {
        policyFlags |= POLICY_FLAG_DISABLE_KEY_REPEAT;
    }

    out.push_back(NotifyKeyArgs(getContext()->getNextId(), when, readTime, getDeviceId(), mSource,
                                getDisplayId(), policyFlags,
                                down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP,
                                AKEY_EVENT_FLAG_FROM_SYSTEM, keyCode, scanCode, keyMetaState,
                                downTime));
    return out;
}

ssize_t KeyboardInputMapper::findKeyDown(int32_t scanCode) {
    size_t n = mKeyDowns.size();
    for (size_t i = 0; i < n; i++) {
        if (mKeyDowns[i].scanCode == scanCode) {
            return i;
        }
    }
    return -1;
}

int32_t KeyboardInputMapper::getKeyCodeState(uint32_t sourceMask, int32_t keyCode) {
    return getDeviceContext().getKeyCodeState(keyCode);
}

int32_t KeyboardInputMapper::getScanCodeState(uint32_t sourceMask, int32_t scanCode) {
    return getDeviceContext().getScanCodeState(scanCode);
}

int32_t KeyboardInputMapper::getKeyCodeForKeyLocation(int32_t locationKeyCode) const {
    return getDeviceContext().getKeyCodeForKeyLocation(locationKeyCode);
}

bool KeyboardInputMapper::markSupportedKeyCodes(uint32_t sourceMask,
                                                const std::vector<int32_t>& keyCodes,
                                                uint8_t* outFlags) {
    return getDeviceContext().markSupportedKeyCodes(keyCodes, outFlags);
}

int32_t KeyboardInputMapper::getMetaState() {
    return mMetaState;
}

bool KeyboardInputMapper::updateMetaState(int32_t keyCode) {
    if (!android::isMetaKey(keyCode) || !getDeviceContext().hasKeyCode(keyCode)) {
        return false;
    }

    updateMetaStateIfNeeded(keyCode, false);
    return true;
}

bool KeyboardInputMapper::updateMetaStateIfNeeded(int32_t keyCode, bool down) {
    int32_t oldMetaState = mMetaState;
    int32_t newMetaState = android::updateMetaState(keyCode, down, oldMetaState);
    int32_t metaStateChanged = oldMetaState ^ newMetaState;
    if (metaStateChanged) {
        mMetaState = newMetaState;
        constexpr int32_t allLedMetaState =
                AMETA_CAPS_LOCK_ON | AMETA_NUM_LOCK_ON | AMETA_SCROLL_LOCK_ON;
        if ((metaStateChanged & allLedMetaState) != 0) {
            getContext()->updateLedMetaState(newMetaState & allLedMetaState);
        }
        getContext()->updateGlobalMetaState();
    }

    return metaStateChanged;
}

void KeyboardInputMapper::resetLedState() {
    initializeLedState(mCapsLockLedState, ALED_CAPS_LOCK);
    initializeLedState(mNumLockLedState, ALED_NUM_LOCK);
    initializeLedState(mScrollLockLedState, ALED_SCROLL_LOCK);

    updateLedState(true);
}

void KeyboardInputMapper::initializeLedState(LedState& ledState, int32_t led) {
    ledState.avail = getDeviceContext().hasLed(led);
    ledState.on = false;
}

void KeyboardInputMapper::updateLedState(bool reset) {
    // Clear the local led state then union the global led state.
    mMetaState &= ~(AMETA_CAPS_LOCK_ON | AMETA_NUM_LOCK_ON | AMETA_SCROLL_LOCK_ON);
    mMetaState |= getContext()->getLedMetaState();

    constexpr int32_t META_NUM = 3;
    const std::vector<int32_t> keyCodes{AKEYCODE_CAPS_LOCK, AKEYCODE_NUM_LOCK,
                                        AKEYCODE_SCROLL_LOCK};
    const std::array<int32_t, META_NUM> metaCodes = {AMETA_CAPS_LOCK_ON, AMETA_NUM_LOCK_ON,
                                                     AMETA_SCROLL_LOCK_ON};
    std::array<uint8_t, META_NUM> flags = {0, 0, 0};
    bool hasKeyLayout = getDeviceContext().markSupportedKeyCodes(keyCodes, flags.data());
    // If the device doesn't have the physical meta key it shouldn't generate the corresponding
    // meta state.
    if (hasKeyLayout) {
        for (int i = 0; i < META_NUM; i++) {
            if (!flags[i]) {
                mMetaState &= ~metaCodes[i];
            }
        }
    }

    updateLedStateForModifier(mCapsLockLedState, ALED_CAPS_LOCK, AMETA_CAPS_LOCK_ON, reset);
    updateLedStateForModifier(mNumLockLedState, ALED_NUM_LOCK, AMETA_NUM_LOCK_ON, reset);
    updateLedStateForModifier(mScrollLockLedState, ALED_SCROLL_LOCK, AMETA_SCROLL_LOCK_ON, reset);
}

void KeyboardInputMapper::updateLedStateForModifier(LedState& ledState, int32_t led,
                                                    int32_t modifier, bool reset) {
    if (ledState.avail) {
        bool desiredState = (mMetaState & modifier) != 0;
        if (reset || ledState.on != desiredState) {
            getDeviceContext().setLedState(led, desiredState);
            ledState.on = desiredState;
        }
    }
}

std::optional<int32_t> KeyboardInputMapper::getAssociatedDisplayId() {
    if (mViewport) {
        return std::make_optional(mViewport->displayId);
    }
    return std::nullopt;
}

std::list<NotifyArgs> KeyboardInputMapper::cancelAllDownKeys(nsecs_t when) {
    std::list<NotifyArgs> out;
    size_t n = mKeyDowns.size();
    for (size_t i = 0; i < n; i++) {
        out.push_back(NotifyKeyArgs(getContext()->getNextId(), when,
                                    systemTime(SYSTEM_TIME_MONOTONIC), getDeviceId(), mSource,
                                    getDisplayId(), 0 /*policyFlags*/, AKEY_EVENT_ACTION_UP,
                                    AKEY_EVENT_FLAG_FROM_SYSTEM | AKEY_EVENT_FLAG_CANCELED,
                                    mKeyDowns[i].keyCode, mKeyDowns[i].scanCode, AMETA_NONE,
                                    mKeyDowns[i].downTime));
    }
    mKeyDowns.clear();
    mMetaState = AMETA_NONE;
    return out;
}

} // namespace android
