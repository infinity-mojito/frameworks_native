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

#include "InputTracingPerfettoBackend.h"

#include "AndroidInputEventProtoConverter.h"

#include <android-base/logging.h>
#include <perfetto/trace/android/android_input_event.pbzero.h>

namespace android::inputdispatcher::trace::impl {

namespace {

constexpr auto INPUT_EVENT_TRACE_DATA_SOURCE_NAME = "android.input.inputevent";

} // namespace

// --- PerfettoBackend::InputEventDataSource ---

PerfettoBackend::InputEventDataSource::InputEventDataSource() : mInstanceId(sNextInstanceId++) {}

void PerfettoBackend::InputEventDataSource::OnSetup(const InputEventDataSource::SetupArgs& args) {
    LOG(INFO) << "Setting up perfetto trace for: " << INPUT_EVENT_TRACE_DATA_SOURCE_NAME
              << ", instanceId: " << mInstanceId;
    const auto rawConfig = args.config->android_input_event_config_raw();
    auto protoConfig = perfetto::protos::pbzero::AndroidInputEventConfig::Decoder{rawConfig};

    mConfig = AndroidInputEventProtoConverter::parseConfig(protoConfig);
}

void PerfettoBackend::InputEventDataSource::OnStart(const InputEventDataSource::StartArgs&) {
    LOG(INFO) << "Starting perfetto trace for: " << INPUT_EVENT_TRACE_DATA_SOURCE_NAME
              << ", instanceId: " << mInstanceId;
}

void PerfettoBackend::InputEventDataSource::OnStop(const InputEventDataSource::StopArgs&) {
    LOG(INFO) << "Stopping perfetto trace for: " << INPUT_EVENT_TRACE_DATA_SOURCE_NAME
              << ", instanceId: " << mInstanceId;
    InputEventDataSource::Trace([&](InputEventDataSource::TraceContext ctx) { ctx.Flush(); });
}

bool PerfettoBackend::InputEventDataSource::shouldIgnoreTracedInputEvent(
        const EventType& type) const {
    if (!getFlags().test(TraceFlag::TRACE_DISPATCHER_INPUT_EVENTS)) {
        // Ignore all input events.
        return true;
    }
    if (!getFlags().test(TraceFlag::TRACE_DISPATCHER_WINDOW_DISPATCH) &&
        type != EventType::INBOUND) {
        // When window dispatch tracing is disabled, ignore any events that are not inbound events.
        return true;
    }
    return false;
}

TraceLevel PerfettoBackend::InputEventDataSource::resolveTraceLevel(
        const TracedEventArgs& args) const {
    // Check for matches with the rules in the order that they are defined.
    for (const auto& rule : mConfig.rules) {
        if (ruleMatches(rule, args)) {
            return rule.level;
        }
    }

    // The event is not traced if it matched zero rules.
    return TraceLevel::TRACE_LEVEL_NONE;
}

bool PerfettoBackend::InputEventDataSource::ruleMatches(const TraceRule& rule,
                                                        const TracedEventArgs& args) const {
    // By default, a rule will match all events. Return early if the rule does not match.

    if (rule.matchSecure.has_value() && *rule.matchSecure != args.isSecure) {
        return false;
    }

    return true;
}

// --- PerfettoBackend ---

std::once_flag PerfettoBackend::sDataSourceRegistrationFlag{};

std::atomic<int32_t> PerfettoBackend::sNextInstanceId{1};

PerfettoBackend::PerfettoBackend() {
    // Use a once-flag to ensure that the data source is only registered once per boot, since
    // we never unregister the InputEventDataSource.
    std::call_once(sDataSourceRegistrationFlag, []() {
        perfetto::TracingInitArgs args;
        args.backends = perfetto::kSystemBackend;
        perfetto::Tracing::Initialize(args);

        // Register our custom data source for input event tracing.
        perfetto::DataSourceDescriptor dsd;
        dsd.set_name(INPUT_EVENT_TRACE_DATA_SOURCE_NAME);
        InputEventDataSource::Register(dsd);
        LOG(INFO) << "InputTracer initialized for data source: "
                  << INPUT_EVENT_TRACE_DATA_SOURCE_NAME;
    });
}

void PerfettoBackend::traceMotionEvent(const TracedMotionEvent& event,
                                       const TracedEventArgs& args) {
    InputEventDataSource::Trace([&](InputEventDataSource::TraceContext ctx) {
        auto dataSource = ctx.GetDataSourceLocked();
        if (dataSource->shouldIgnoreTracedInputEvent(event.eventType)) {
            return;
        }
        const TraceLevel traceLevel = dataSource->resolveTraceLevel(args);
        if (traceLevel == TraceLevel::TRACE_LEVEL_NONE) {
            return;
        }
        const bool isRedacted = traceLevel == TraceLevel::TRACE_LEVEL_REDACTED;
        auto tracePacket = ctx.NewTracePacket();
        auto* inputEvent = tracePacket->set_android_input_event();
        auto* dispatchMotion = isRedacted ? inputEvent->set_dispatcher_motion_event_redacted()
                                          : inputEvent->set_dispatcher_motion_event();
        AndroidInputEventProtoConverter::toProtoMotionEvent(event, *dispatchMotion, isRedacted);
    });
}

void PerfettoBackend::traceKeyEvent(const TracedKeyEvent& event, const TracedEventArgs& args) {
    InputEventDataSource::Trace([&](InputEventDataSource::TraceContext ctx) {
        auto dataSource = ctx.GetDataSourceLocked();
        if (dataSource->shouldIgnoreTracedInputEvent(event.eventType)) {
            return;
        }
        const TraceLevel traceLevel = dataSource->resolveTraceLevel(args);
        if (traceLevel == TraceLevel::TRACE_LEVEL_NONE) {
            return;
        }
        const bool isRedacted = traceLevel == TraceLevel::TRACE_LEVEL_REDACTED;
        auto tracePacket = ctx.NewTracePacket();
        auto* inputEvent = tracePacket->set_android_input_event();
        auto* dispatchKey = isRedacted ? inputEvent->set_dispatcher_key_event_redacted()
                                       : inputEvent->set_dispatcher_key_event();
        AndroidInputEventProtoConverter::toProtoKeyEvent(event, *dispatchKey, isRedacted);
    });
}

void PerfettoBackend::traceWindowDispatch(const WindowDispatchArgs& dispatchArgs,
                                          const TracedEventArgs& args) {
    InputEventDataSource::Trace([&](InputEventDataSource::TraceContext ctx) {
        auto dataSource = ctx.GetDataSourceLocked();
        if (!dataSource->getFlags().test(TraceFlag::TRACE_DISPATCHER_WINDOW_DISPATCH)) {
            return;
        }
        const TraceLevel traceLevel = dataSource->resolveTraceLevel(args);
        if (traceLevel == TraceLevel::TRACE_LEVEL_NONE) {
            return;
        }
        const bool isRedacted = traceLevel == TraceLevel::TRACE_LEVEL_REDACTED;
        auto tracePacket = ctx.NewTracePacket();
        auto* inputEvent = tracePacket->set_android_input_event();
        auto* dispatchEvent = isRedacted
                ? inputEvent->set_dispatcher_window_dispatch_event_redacted()
                : inputEvent->set_dispatcher_window_dispatch_event();
        AndroidInputEventProtoConverter::toProtoWindowDispatchEvent(dispatchArgs, *dispatchEvent,
                                                                    isRedacted);
    });
}

} // namespace android::inputdispatcher::trace::impl
