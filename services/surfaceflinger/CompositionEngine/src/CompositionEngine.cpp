/*
 * Copyright 2018 The Android Open Source Project
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

#include <compositionengine/CompositionRefreshArgs.h>
#include <compositionengine/LayerFE.h>
#include <compositionengine/LayerFECompositionState.h>
#include <compositionengine/OutputLayer.h>
#include <compositionengine/impl/CompositionEngine.h>
#include <compositionengine/impl/Display.h>
#include <ui/DisplayMap.h>

#include <renderengine/RenderEngine.h>
#include <utils/Trace.h>

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"

#include "DisplayHardware/HWComposer.h"

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion"

namespace android::compositionengine {

CompositionEngine::~CompositionEngine() = default;

namespace impl {

std::unique_ptr<compositionengine::CompositionEngine> createCompositionEngine() {
    return std::make_unique<CompositionEngine>();
}

CompositionEngine::CompositionEngine() = default;
CompositionEngine::~CompositionEngine() = default;

std::shared_ptr<compositionengine::Display> CompositionEngine::createDisplay(
        const DisplayCreationArgs& args) {
    return compositionengine::impl::createDisplay(*this, args);
}

std::unique_ptr<compositionengine::LayerFECompositionState>
CompositionEngine::createLayerFECompositionState() {
    return std::make_unique<compositionengine::LayerFECompositionState>();
}

HWComposer& CompositionEngine::getHwComposer() const {
    return *mHwComposer.get();
}

void CompositionEngine::setHwComposer(std::unique_ptr<HWComposer> hwComposer) {
    mHwComposer = std::move(hwComposer);
}

renderengine::RenderEngine& CompositionEngine::getRenderEngine() const {
    return *mRenderEngine;
}

void CompositionEngine::setRenderEngine(renderengine::RenderEngine* renderEngine) {
    mRenderEngine = renderEngine;
}

TimeStats* CompositionEngine::getTimeStats() const {
    return mTimeStats.get();
}

void CompositionEngine::setTimeStats(const std::shared_ptr<TimeStats>& timeStats) {
    mTimeStats = timeStats;
}

bool CompositionEngine::needsAnotherUpdate() const {
    return mNeedsAnotherUpdate;
}

nsecs_t CompositionEngine::getLastFrameRefreshTimestamp() const {
    return mRefreshStartTime;
}

namespace {
int numDisplaysWithOffloadPresentSupport(const CompositionRefreshArgs& args) {
    if (!FlagManager::getInstance().multithreaded_present() || args.outputs.size() < 2) {
        return 0;
    }

    int numEligibleDisplays = 0;
    // Only run present in multiple threads if all HWC-enabled displays
    // being refreshed support it.
    if (!std::all_of(args.outputs.begin(), args.outputs.end(),
                     [&numEligibleDisplays](const auto& output) {
                         if (!ftl::Optional(output->getDisplayId())
                                      .and_then(HalDisplayId::tryCast)) {
                             // Not HWC-enabled, so it is always
                             // client-composited.
                             return true;
                         }
                         const bool support = output->supportsOffloadPresent();
                         numEligibleDisplays += static_cast<int>(support);
                         return support;
                     })) {
        return 0;
    }
    return numEligibleDisplays;
}
} // namespace

void CompositionEngine::present(CompositionRefreshArgs& args) {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    preComposition(args);

    {
        // latchedLayers is used to track the set of front-end layer state that
        // has been latched across all outputs for the prepare step, and is not
        // needed for anything else.
        LayerFESet latchedLayers;

        for (const auto& output : args.outputs) {
            output->prepare(args, latchedLayers);
        }
    }

    // Offloading the HWC call for `present` allows us to simultaneously call it
    // on multiple displays. This is desirable because these calls block and can
    // be slow.
    if (const int numEligibleDisplays = numDisplaysWithOffloadPresentSupport(args);
        numEligibleDisplays > 1) {
        // Leave the last eligible display on the main thread, which will
        // allow it to run concurrently without an extra thread hop.
        int numToOffload = numEligibleDisplays - 1;
        for (auto& output : args.outputs) {
            if (output->supportsOffloadPresent()) {
                output->offloadPresentNextFrame();
                if (--numToOffload == 0) {
                    break;
                }
            }
        }
    }

    ui::DisplayVector<ftl::Future<std::monostate>> presentFutures;
    for (const auto& output : args.outputs) {
        presentFutures.push_back(output->present(args));
    }

    {
        ATRACE_NAME("Waiting on HWC");
        for (auto& future : presentFutures) {
            // TODO(b/185536303): Call ftl::Future::wait() once it exists, since
            // we do not need the return value of get().
            future.get();
        }
    }
}

void CompositionEngine::updateCursorAsync(CompositionRefreshArgs& args) {
    std::unordered_map<compositionengine::LayerFE*, compositionengine::LayerFECompositionState*>
            uniqueVisibleLayers;

    for (const auto& output : args.outputs) {
        for (auto* layer : output->getOutputLayersOrderedByZ()) {
            if (layer->isHardwareCursor()) {
                layer->writeCursorPositionToHWC();
            }
        }
    }
}

void CompositionEngine::preComposition(CompositionRefreshArgs& args) {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    bool needsAnotherUpdate = false;

    mRefreshStartTime = systemTime(SYSTEM_TIME_MONOTONIC);

    for (auto& layer : args.layers) {
        if (layer->onPreComposition(mRefreshStartTime, args.updatingOutputGeometryThisFrame)) {
            needsAnotherUpdate = true;
        }
    }

    mNeedsAnotherUpdate = needsAnotherUpdate;
}

FeatureFlags CompositionEngine::getFeatureFlags() const {
    return {};
}

void CompositionEngine::dump(std::string&) const {
    // The base class has no state to dump, but derived classes might.
}

void CompositionEngine::setNeedsAnotherUpdateForTest(bool value) {
    mNeedsAnotherUpdate = value;
}

} // namespace impl
} // namespace android::compositionengine
