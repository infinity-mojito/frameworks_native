/*
 * Copyright 2021 The Android Open Source Project
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

#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include <gui/LayerState.h>
#include <system/window.h>

namespace android {

struct TransactionState {
    TransactionState() = default;

    TransactionState(const FrameTimelineInfo& frameTimelineInfo,
                     const Vector<ComposerState>& composerStates,
                     const Vector<DisplayState>& displayStates, uint32_t transactionFlags,
                     const sp<IBinder>& applyToken, const InputWindowCommands& inputWindowCommands,
                     int64_t desiredPresentTime, bool isAutoTimestamp,
                     const client_cache_t& uncacheBuffer, int64_t postTime, uint32_t permissions,
                     bool hasListenerCallbacks, std::vector<ListenerCallbacks> listenerCallbacks,
                     int originPid, int originUid, uint64_t transactionId)
          : frameTimelineInfo(frameTimelineInfo),
            states(composerStates),
            displays(displayStates),
            flags(transactionFlags),
            applyToken(applyToken),
            inputWindowCommands(inputWindowCommands),
            desiredPresentTime(desiredPresentTime),
            isAutoTimestamp(isAutoTimestamp),
            buffer(uncacheBuffer),
            postTime(postTime),
            permissions(permissions),
            hasListenerCallbacks(hasListenerCallbacks),
            listenerCallbacks(listenerCallbacks),
            originPid(originPid),
            originUid(originUid),
            id(transactionId) {}

    // Invokes `void(const layer_state_t&)` visitor for matching layers.
    template <typename Visitor>
    void traverseStatesWithBuffers(Visitor&& visitor) const {
        for (const auto& [state] : states) {
            if (state.hasBufferChanges() && state.hasValidBuffer() && state.surface) {
                visitor(state);
            }
        }
    }

    template <typename Visitor>
    void traverseStatesWithBuffersWhileTrue(Visitor&& visitor) const {
        for (const auto& [state] : states) {
            if (state.hasBufferChanges() && state.hasValidBuffer() && state.surface) {
                if (!visitor(state)) return;
            }
        }
    }

    // TODO(b/185535769): Remove FrameHint. Instead, reset the idle timer (of the relevant physical
    // display) on the main thread if commit leads to composite. Then, RefreshRateOverlay should be
    // able to setFrameRate once, rather than for each transaction.
    bool isFrameActive() const {
        if (!displays.empty()) return true;

        for (const auto& [state] : states) {
            if (state.frameRateCompatibility != ANATIVEWINDOW_FRAME_RATE_NO_VOTE) {
                return true;
            }
        }

        return false;
    }

    FrameTimelineInfo frameTimelineInfo;
    Vector<ComposerState> states;
    Vector<DisplayState> displays;
    uint32_t flags;
    sp<IBinder> applyToken;
    InputWindowCommands inputWindowCommands;
    int64_t desiredPresentTime;
    bool isAutoTimestamp;
    client_cache_t buffer;
    int64_t postTime;
    uint32_t permissions;
    bool hasListenerCallbacks;
    std::vector<ListenerCallbacks> listenerCallbacks;
    int originPid;
    int originUid;
    uint64_t id;
    bool sentFenceTimeoutWarning = false;
};

} // namespace android
