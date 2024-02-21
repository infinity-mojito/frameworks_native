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

#define LOG_TAG "InputTracer"

#include "InputTracer.h"

#include <android-base/logging.h>

namespace android::inputdispatcher::trace::impl {

namespace {

// Helper to std::visit with lambdas.
template <typename... V>
struct Visitor : V... {
    using V::operator()...;
};

TracedEvent createTracedEvent(const MotionEntry& e) {
    return TracedMotionEvent{e.id,
                             e.eventTime,
                             e.policyFlags,
                             e.deviceId,
                             e.source,
                             e.displayId,
                             e.action,
                             e.actionButton,
                             e.flags,
                             e.metaState,
                             e.buttonState,
                             e.classification,
                             e.edgeFlags,
                             e.xPrecision,
                             e.yPrecision,
                             e.xCursorPosition,
                             e.yCursorPosition,
                             e.downTime,
                             e.pointerProperties,
                             e.pointerCoords};
}

TracedEvent createTracedEvent(const KeyEntry& e) {
    return TracedKeyEvent{e.id,        e.eventTime, e.policyFlags, e.deviceId, e.source,
                          e.displayId, e.action,    e.keyCode,     e.scanCode, e.metaState,
                          e.downTime,  e.flags,     e.repeatCount};
}

void writeEventToBackend(const TracedEvent& event, InputTracingBackendInterface& backend) {
    std::visit(Visitor{[&](const TracedMotionEvent& e) { backend.traceMotionEvent(e); },
                       [&](const TracedKeyEvent& e) { backend.traceKeyEvent(e); }},
               event);
}

} // namespace

// --- InputTracer ---

InputTracer::InputTracer(std::unique_ptr<InputTracingBackendInterface> backend)
      : mBackend(std::move(backend)) {}

std::unique_ptr<EventTrackerInterface> InputTracer::traceInboundEvent(const EventEntry& entry) {
    // This is a newly traced inbound event. Create a new state to track it and its derived events.
    auto eventState = std::make_shared<EventState>(*this);

    if (entry.type == EventEntry::Type::MOTION) {
        const auto& motion = static_cast<const MotionEntry&>(entry);
        eventState->events.emplace_back(createTracedEvent(motion));
    } else if (entry.type == EventEntry::Type::KEY) {
        const auto& key = static_cast<const KeyEntry&>(entry);
        eventState->events.emplace_back(createTracedEvent(key));
    } else {
        LOG(FATAL) << "Cannot trace EventEntry of type: " << ftl::enum_string(entry.type);
    }

    return std::make_unique<EventTrackerImpl>(std::move(eventState), /*isDerived=*/false);
}

void InputTracer::dispatchToTargetHint(const EventTrackerInterface& cookie,
                                       const InputTarget& target) {
    if (isDerivedCookie(cookie)) {
        LOG(FATAL) << "Event target cannot be updated from a derived cookie.";
    }
    auto& eventState = getState(cookie);
    if (eventState->isEventProcessingComplete) {
        // TODO(b/210460522): Disallow adding new targets after eventProcessingComplete() is called.
        return;
    }
    // TODO(b/210460522): Determine if the event is sensitive based on the target.
}

void InputTracer::eventProcessingComplete(const EventTrackerInterface& cookie) {
    if (isDerivedCookie(cookie)) {
        LOG(FATAL) << "Event processing cannot be set from a derived cookie.";
    }
    auto& eventState = getState(cookie);
    if (eventState->isEventProcessingComplete) {
        LOG(FATAL) << "Traced event was already logged. "
                      "eventProcessingComplete() was likely called more than once.";
    }

    for (const auto& event : eventState->events) {
        writeEventToBackend(event, *mBackend);
    }
    eventState->isEventProcessingComplete = true;
}

std::unique_ptr<EventTrackerInterface> InputTracer::traceDerivedEvent(
        const EventEntry& entry, const EventTrackerInterface& originalEventCookie) {
    // This is an event derived from an already-established event. Use the same state to track
    // this event too.
    auto eventState = getState(originalEventCookie);

    if (entry.type == EventEntry::Type::MOTION) {
        const auto& motion = static_cast<const MotionEntry&>(entry);
        eventState->events.emplace_back(createTracedEvent(motion));
    } else if (entry.type == EventEntry::Type::KEY) {
        const auto& key = static_cast<const KeyEntry&>(entry);
        eventState->events.emplace_back(createTracedEvent(key));
    } else {
        LOG(FATAL) << "Cannot trace EventEntry of type: " << ftl::enum_string(entry.type);
    }

    if (eventState->isEventProcessingComplete) {
        // It is possible for a derived event to be dispatched some time after the original event
        // is dispatched, such as in the case of key fallback events. To account for these cases,
        // derived events can be traced after the processing is complete for the original event.
        writeEventToBackend(eventState->events.back(), *mBackend);
    }
    return std::make_unique<EventTrackerImpl>(std::move(eventState), /*isDerived=*/true);
}

void InputTracer::traceEventDispatch(const DispatchEntry& dispatchEntry,
                                     const EventTrackerInterface* cookie) {
    const EventEntry& entry = *dispatchEntry.eventEntry;
    // TODO(b/328618922): Remove resolved key repeats after making repeatCount non-mutable.
    // The KeyEntry's repeatCount is mutable and can be modified after an event is initially traced,
    // so we need to find the repeatCount at the time of dispatching to trace it accurately.
    int32_t resolvedKeyRepeatCount = 0;

    TracedEvent traced;
    if (entry.type == EventEntry::Type::MOTION) {
        const auto& motion = static_cast<const MotionEntry&>(entry);
        traced = createTracedEvent(motion);
    } else if (entry.type == EventEntry::Type::KEY) {
        const auto& key = static_cast<const KeyEntry&>(entry);
        resolvedKeyRepeatCount = key.repeatCount;
        traced = createTracedEvent(key);
    } else {
        LOG(FATAL) << "Cannot trace EventEntry of type: " << ftl::enum_string(entry.type);
    }

    if (!cookie) {
        // This event was not tracked as an inbound event, so trace it now.
        writeEventToBackend(traced, *mBackend);
    }

    // The vsyncId only has meaning if the event is targeting a window.
    const int32_t windowId = dispatchEntry.windowId.value_or(0);
    const int32_t vsyncId = dispatchEntry.windowId.has_value() ? dispatchEntry.vsyncId : 0;

    mBackend->traceWindowDispatch({std::move(traced), dispatchEntry.deliveryTime,
                                   dispatchEntry.resolvedFlags, dispatchEntry.targetUid, vsyncId,
                                   windowId, dispatchEntry.transform, dispatchEntry.rawTransform,
                                   /*hmac=*/{}, resolvedKeyRepeatCount});
}

std::shared_ptr<InputTracer::EventState>& InputTracer::getState(
        const EventTrackerInterface& cookie) {
    return static_cast<const EventTrackerImpl&>(cookie).mState;
}

bool InputTracer::isDerivedCookie(const EventTrackerInterface& cookie) {
    return static_cast<const EventTrackerImpl&>(cookie).mIsDerived;
}

// --- InputTracer::EventState ---

InputTracer::EventState::~EventState() {
    if (isEventProcessingComplete) {
        // This event has already been written to the trace as expected.
        return;
    }
    // The event processing was never marked as complete, so do it now.
    // TODO(b/210460522): Determine why/where the event is being destroyed before
    //   eventProcessingComplete() is called.
    for (const auto& event : events) {
        writeEventToBackend(event, *tracer.mBackend);
    }
    isEventProcessingComplete = true;
}

} // namespace android::inputdispatcher::trace::impl
