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

#include "InputMapperTest.h"

#include <InputReaderBase.h>
#include <gtest/gtest.h>
#include <ui/Rotation.h>

namespace android {

const char* InputMapperTest::DEVICE_NAME = "device";
const char* InputMapperTest::DEVICE_LOCATION = "USB1";
const ftl::Flags<InputDeviceClass> InputMapperTest::DEVICE_CLASSES =
        ftl::Flags<InputDeviceClass>(0); // not needed for current tests

void InputMapperTest::SetUp(ftl::Flags<InputDeviceClass> classes, int bus) {
    mFakeEventHub = std::make_unique<FakeEventHub>();
    mFakePolicy = sp<FakeInputReaderPolicy>::make();
    mFakeListener = std::make_unique<TestInputListener>();
    mReader = std::make_unique<InstrumentedInputReader>(mFakeEventHub, mFakePolicy, *mFakeListener);
    mDevice = newDevice(DEVICE_ID, DEVICE_NAME, DEVICE_LOCATION, EVENTHUB_ID, classes, bus);
    // Consume the device reset notification generated when adding a new device.
    mFakeListener->assertNotifyDeviceResetWasCalled();
}

void InputMapperTest::SetUp() {
    SetUp(DEVICE_CLASSES);
}

void InputMapperTest::TearDown() {
    mFakeListener.reset();
    mFakePolicy.clear();
}

void InputMapperTest::addConfigurationProperty(const char* key, const char* value) {
    mFakeEventHub->addConfigurationProperty(EVENTHUB_ID, key, value);
}

std::list<NotifyArgs> InputMapperTest::configureDevice(uint32_t changes) {
    if (!changes ||
        (changes &
         (InputReaderConfiguration::CHANGE_DISPLAY_INFO |
          InputReaderConfiguration::CHANGE_POINTER_CAPTURE))) {
        mReader->requestRefreshConfiguration(changes);
        mReader->loopOnce();
    }
    std::list<NotifyArgs> out =
            mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(), changes);
    // Loop the reader to flush the input listener queue.
    for (const NotifyArgs& args : out) {
        mFakeListener->notify(args);
    }
    mReader->loopOnce();
    return out;
}

std::shared_ptr<InputDevice> InputMapperTest::newDevice(int32_t deviceId, const std::string& name,
                                                        const std::string& location,
                                                        int32_t eventHubId,
                                                        ftl::Flags<InputDeviceClass> classes,
                                                        int bus) {
    InputDeviceIdentifier identifier;
    identifier.name = name;
    identifier.location = location;
    identifier.bus = bus;
    std::shared_ptr<InputDevice> device =
            std::make_shared<InputDevice>(mReader->getContext(), deviceId, DEVICE_GENERATION,
                                          identifier);
    mReader->pushNextDevice(device);
    mFakeEventHub->addDevice(eventHubId, name, classes, bus);
    mReader->loopOnce();
    return device;
}

void InputMapperTest::setDisplayInfoAndReconfigure(int32_t displayId, int32_t width, int32_t height,
                                                   ui::Rotation orientation,
                                                   const std::string& uniqueId,
                                                   std::optional<uint8_t> physicalPort,
                                                   ViewportType viewportType) {
    mFakePolicy->addDisplayViewport(displayId, width, height, orientation, /* isActive= */ true,
                                    uniqueId, physicalPort, viewportType);
    configureDevice(InputReaderConfiguration::CHANGE_DISPLAY_INFO);
}

void InputMapperTest::clearViewports() {
    mFakePolicy->clearViewports();
}

std::list<NotifyArgs> InputMapperTest::process(InputMapper& mapper, nsecs_t when, nsecs_t readTime,
                                               int32_t type, int32_t code, int32_t value) {
    RawEvent event;
    event.when = when;
    event.readTime = readTime;
    event.deviceId = mapper.getDeviceContext().getEventHubId();
    event.type = type;
    event.code = code;
    event.value = value;
    std::list<NotifyArgs> processArgList = mapper.process(&event);
    for (const NotifyArgs& args : processArgList) {
        mFakeListener->notify(args);
    }
    // Loop the reader to flush the input listener queue.
    mReader->loopOnce();
    return processArgList;
}

void InputMapperTest::resetMapper(InputMapper& mapper, nsecs_t when) {
    const auto resetArgs = mapper.reset(when);
    for (const auto args : resetArgs) {
        mFakeListener->notify(args);
    }
    // Loop the reader to flush the input listener queue.
    mReader->loopOnce();
}

std::list<NotifyArgs> InputMapperTest::handleTimeout(InputMapper& mapper, nsecs_t when) {
    std::list<NotifyArgs> generatedArgs = mapper.timeoutExpired(when);
    for (const NotifyArgs& args : generatedArgs) {
        mFakeListener->notify(args);
    }
    // Loop the reader to flush the input listener queue.
    mReader->loopOnce();
    return generatedArgs;
}

void InputMapperTest::assertMotionRange(const InputDeviceInfo& info, int32_t axis, uint32_t source,
                                        float min, float max, float flat, float fuzz) {
    const InputDeviceInfo::MotionRange* range = info.getMotionRange(axis, source);
    ASSERT_TRUE(range != nullptr) << "Axis: " << axis << " Source: " << source;
    ASSERT_EQ(axis, range->axis) << "Axis: " << axis << " Source: " << source;
    ASSERT_EQ(source, range->source) << "Axis: " << axis << " Source: " << source;
    ASSERT_NEAR(min, range->min, EPSILON) << "Axis: " << axis << " Source: " << source;
    ASSERT_NEAR(max, range->max, EPSILON) << "Axis: " << axis << " Source: " << source;
    ASSERT_NEAR(flat, range->flat, EPSILON) << "Axis: " << axis << " Source: " << source;
    ASSERT_NEAR(fuzz, range->fuzz, EPSILON) << "Axis: " << axis << " Source: " << source;
}

void InputMapperTest::assertPointerCoords(const PointerCoords& coords, float x, float y,
                                          float pressure, float size, float touchMajor,
                                          float touchMinor, float toolMajor, float toolMinor,
                                          float orientation, float distance,
                                          float scaledAxisEpsilon) {
    ASSERT_NEAR(x, coords.getAxisValue(AMOTION_EVENT_AXIS_X), scaledAxisEpsilon);
    ASSERT_NEAR(y, coords.getAxisValue(AMOTION_EVENT_AXIS_Y), scaledAxisEpsilon);
    ASSERT_NEAR(pressure, coords.getAxisValue(AMOTION_EVENT_AXIS_PRESSURE), EPSILON);
    ASSERT_NEAR(size, coords.getAxisValue(AMOTION_EVENT_AXIS_SIZE), EPSILON);
    ASSERT_NEAR(touchMajor, coords.getAxisValue(AMOTION_EVENT_AXIS_TOUCH_MAJOR), scaledAxisEpsilon);
    ASSERT_NEAR(touchMinor, coords.getAxisValue(AMOTION_EVENT_AXIS_TOUCH_MINOR), scaledAxisEpsilon);
    ASSERT_NEAR(toolMajor, coords.getAxisValue(AMOTION_EVENT_AXIS_TOOL_MAJOR), scaledAxisEpsilon);
    ASSERT_NEAR(toolMinor, coords.getAxisValue(AMOTION_EVENT_AXIS_TOOL_MINOR), scaledAxisEpsilon);
    ASSERT_NEAR(orientation, coords.getAxisValue(AMOTION_EVENT_AXIS_ORIENTATION), EPSILON);
    ASSERT_NEAR(distance, coords.getAxisValue(AMOTION_EVENT_AXIS_DISTANCE), EPSILON);
}

void InputMapperTest::assertPosition(const FakePointerController& controller, float x, float y) {
    float actualX, actualY;
    controller.getPosition(&actualX, &actualY);
    ASSERT_NEAR(x, actualX, 1);
    ASSERT_NEAR(y, actualY, 1);
}

} // namespace android
