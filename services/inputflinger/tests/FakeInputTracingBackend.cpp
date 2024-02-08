/*
 * Copyright 2024 The Android Open Source Project
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

#include "FakeInputTracingBackend.h"

#include <android-base/logging.h>
#include <utils/Errors.h>

namespace android::inputdispatcher {

namespace {

// Use a larger timeout while waiting for events to be traced, compared to the timeout used while
// waiting to receive events through the input channel. Events are traced from a separate thread,
// which does not have the same high thread priority as the InputDispatcher's thread, so the tracer
// is expected to lag behind the Dispatcher at times.
constexpr auto TRACE_TIMEOUT = std::chrono::seconds(5);

base::ResultError<> error(const std::ostringstream& ss) {
    return base::ResultError(ss.str(), BAD_VALUE);
}

} // namespace

// --- VerifyingTrace ---

void VerifyingTrace::expectKeyDispatchTraced(const KeyEvent& event) {
    std::scoped_lock lock(mLock);
    mExpectedEvents.emplace_back(event);
}

void VerifyingTrace::expectMotionDispatchTraced(const MotionEvent& event) {
    std::scoped_lock lock(mLock);
    mExpectedEvents.emplace_back(event);
}

void VerifyingTrace::verifyExpectedEventsTraced() {
    std::unique_lock lock(mLock);
    base::ScopedLockAssertion assumeLocked(mLock);

    base::Result<void> result;
    mEventTracedCondition.wait_for(lock, TRACE_TIMEOUT, [&]() REQUIRES(mLock) {
        for (const auto& expectedEvent : mExpectedEvents) {
            std::visit([&](const auto& event)
                               REQUIRES(mLock) { result = verifyEventTraced(event); },
                       expectedEvent);
            if (!result.ok()) {
                return false;
            }
        }
        return true;
    });

    EXPECT_TRUE(result.ok())
            << "Timed out waiting for all expected events to be traced successfully: "
            << result.error().message();
}

void VerifyingTrace::reset() {
    std::scoped_lock lock(mLock);
    mTracedEvents.clear();
    mExpectedEvents.clear();
}

template <typename Event>
base::Result<void> VerifyingTrace::verifyEventTraced(const Event& expectedEvent) const {
    std::ostringstream msg;

    auto tracedEventsIt = mTracedEvents.find(expectedEvent.getId());
    if (tracedEventsIt == mTracedEvents.end()) {
        msg << "Expected event with ID 0x" << std::hex << expectedEvent.getId()
            << " to be traced, but it was not.\n"
            << "Expected event: " << expectedEvent;
        return error(msg);
    }

    return {};
}

// --- FakeInputTracingBackend ---

void FakeInputTracingBackend::traceKeyEvent(const trace::TracedKeyEvent& event) const {
    {
        std::scoped_lock lock(mTrace->mLock);
        mTrace->mTracedEvents.emplace(event.id);
    }
    mTrace->mEventTracedCondition.notify_all();
}

void FakeInputTracingBackend::traceMotionEvent(const trace::TracedMotionEvent& event) const {
    {
        std::scoped_lock lock(mTrace->mLock);
        mTrace->mTracedEvents.emplace(event.id);
    }
    mTrace->mEventTracedCondition.notify_all();
}

} // namespace android::inputdispatcher
