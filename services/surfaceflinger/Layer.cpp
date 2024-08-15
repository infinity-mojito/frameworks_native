/*
 * Copyright (C) 2007 The Android Open Source Project
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

// TODO(b/129481165): remove the #pragma below and fix conversion issues

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "Layer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <binder/IPCThreadState.h>
#include <common/trace.h>
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/Display.h>
#include <compositionengine/LayerFECompositionState.h>
#include <compositionengine/OutputLayer.h>
#include <compositionengine/impl/OutputLayerCompositionState.h>
#include <cutils/compiler.h>
#include <cutils/native_handle.h>
#include <cutils/properties.h>
#include <ftl/enum.h>
#include <ftl/fake_guard.h>
#include <gui/BufferItem.h>
#include <gui/Surface.h>
#include <math.h>
#include <private/android_filesystem_config.h>
#include <renderengine/RenderEngine.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <system/graphics-base-v1.0.h>
#include <ui/DebugUtils.h>
#include <ui/FloatRect.h>
#include <ui/GraphicBuffer.h>
#include <ui/HdrRenderTypeUtils.h>
#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <ui/Transform.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/NativeHandle.h>
#include <utils/StopWatch.h>

#include <algorithm>
#include <mutex>
#include <optional>
#include <sstream>

#include "DisplayDevice.h"
#include "DisplayHardware/HWComposer.h"
#include "FrameTimeline.h"
#include "FrameTracer/FrameTracer.h"
#include "FrontEnd/LayerCreationArgs.h"
#include "FrontEnd/LayerHandle.h"
#include "Layer.h"
#include "LayerProtoHelper.h"
#include "MutexUtils.h"
#include "SurfaceFlinger.h"
#include "TimeStats/TimeStats.h"
#include "TransactionCallbackInvoker.h"
#include "TunnelModeEnabledReporter.h"
#include "Utils/FenceUtils.h"

#define DEBUG_RESIZE 0
#define EARLY_RELEASE_ENABLED false

namespace android {
using namespace std::chrono_literals;
namespace {
constexpr int kDumpTableRowLength = 159;

const ui::Transform kIdentityTransform;

ui::LogicalDisplayId toLogicalDisplayId(const ui::LayerStack& layerStack) {
    return ui::LogicalDisplayId{static_cast<int32_t>(layerStack.id)};
}

bool assignTransform(ui::Transform* dst, ui::Transform& from) {
    if (*dst == from) {
        return false;
    }
    *dst = from;
    return true;
}

TimeStats::SetFrameRateVote frameRateToSetFrameRateVotePayload(Layer::FrameRate frameRate) {
    using FrameRateCompatibility = TimeStats::SetFrameRateVote::FrameRateCompatibility;
    using Seamlessness = TimeStats::SetFrameRateVote::Seamlessness;
    const auto frameRateCompatibility = [frameRate] {
        switch (frameRate.vote.type) {
            case Layer::FrameRateCompatibility::Default:
                return FrameRateCompatibility::Default;
            case Layer::FrameRateCompatibility::ExactOrMultiple:
                return FrameRateCompatibility::ExactOrMultiple;
            default:
                return FrameRateCompatibility::Undefined;
        }
    }();

    const auto seamlessness = [frameRate] {
        switch (frameRate.vote.seamlessness) {
            case scheduler::Seamlessness::OnlySeamless:
                return Seamlessness::ShouldBeSeamless;
            case scheduler::Seamlessness::SeamedAndSeamless:
                return Seamlessness::NotRequired;
            default:
                return Seamlessness::Undefined;
        }
    }();

    return TimeStats::SetFrameRateVote{.frameRate = frameRate.vote.rate.getValue(),
                                       .frameRateCompatibility = frameRateCompatibility,
                                       .seamlessness = seamlessness};
}

} // namespace

using namespace ftl::flag_operators;

using base::StringAppendF;
using frontend::LayerSnapshot;
using frontend::RoundedCornerState;
using gui::GameMode;
using gui::LayerMetadata;
using gui::WindowInfo;
using ui::Size;

using PresentState = frametimeline::SurfaceFrame::PresentState;

Layer::Layer(const surfaceflinger::LayerCreationArgs& args)
      : sequence(args.sequence),
        mFlinger(sp<SurfaceFlinger>::fromExisting(args.flinger)),
        mName(base::StringPrintf("%s#%d", args.name.c_str(), sequence)),
        mClientRef(args.client),
        mWindowType(static_cast<WindowInfo::Type>(
                args.metadata.getInt32(gui::METADATA_WINDOW_TYPE, 0))),
        mLayerCreationFlags(args.flags),
        mLegacyLayerFE(args.flinger->getFactory().createLayerFE(mName, this)) {
    ALOGV("Creating Layer %s", getDebugName());

    uint32_t layerFlags = 0;
    if (args.flags & ISurfaceComposerClient::eHidden) layerFlags |= layer_state_t::eLayerHidden;
    if (args.flags & ISurfaceComposerClient::eOpaque) layerFlags |= layer_state_t::eLayerOpaque;
    if (args.flags & ISurfaceComposerClient::eSecure) layerFlags |= layer_state_t::eLayerSecure;
    if (args.flags & ISurfaceComposerClient::eSkipScreenshot)
        layerFlags |= layer_state_t::eLayerSkipScreenshot;
    mDrawingState.flags = layerFlags;
    mDrawingState.crop.makeInvalid();
    mDrawingState.z = 0;
    mDrawingState.color.a = 1.0f;
    mDrawingState.layerStack = ui::DEFAULT_LAYER_STACK;
    mDrawingState.sequence = 0;
    mDrawingState.transform.set(0, 0);
    mDrawingState.frameNumber = 0;
    mDrawingState.previousFrameNumber = 0;
    mDrawingState.barrierFrameNumber = 0;
    mDrawingState.producerId = 0;
    mDrawingState.barrierProducerId = 0;
    mDrawingState.bufferTransform = 0;
    mDrawingState.transformToDisplayInverse = false;
    mDrawingState.acquireFence = sp<Fence>::make(-1);
    mDrawingState.acquireFenceTime = std::make_shared<FenceTime>(mDrawingState.acquireFence);
    mDrawingState.dataspace = ui::Dataspace::V0_SRGB;
    mDrawingState.hdrMetadata.validTypes = 0;
    mDrawingState.surfaceDamageRegion = Region::INVALID_REGION;
    mDrawingState.cornerRadius = 0.0f;
    mDrawingState.backgroundBlurRadius = 0;
    mDrawingState.api = -1;
    mDrawingState.hasColorTransform = false;
    mDrawingState.colorSpaceAgnostic = false;
    mDrawingState.frameRateSelectionPriority = PRIORITY_UNSET;
    mDrawingState.metadata = args.metadata;
    mDrawingState.shadowRadius = 0.f;
    mDrawingState.fixedTransformHint = ui::Transform::ROT_INVALID;
    mDrawingState.frameTimelineInfo = {};
    mDrawingState.postTime = -1;
    mDrawingState.destinationFrame.makeInvalid();
    mDrawingState.isTrustedOverlay = false;
    mDrawingState.dropInputMode = gui::DropInputMode::NONE;
    mDrawingState.dimmingEnabled = true;
    mDrawingState.defaultFrameRateCompatibility = FrameRateCompatibility::Default;
    mDrawingState.frameRateSelectionStrategy = FrameRateSelectionStrategy::Propagate;

    if (args.flags & ISurfaceComposerClient::eNoColorFill) {
        // Set an invalid color so there is no color fill.
        mDrawingState.color.r = -1.0_hf;
        mDrawingState.color.g = -1.0_hf;
        mDrawingState.color.b = -1.0_hf;
    }

    mFrameTracker.setDisplayRefreshPeriod(
            args.flinger->mScheduler->getPacesetterVsyncPeriod().ns());

    mOwnerUid = args.ownerUid;
    mOwnerPid = args.ownerPid;
    mOwnerAppId = mOwnerUid % PER_USER_RANGE;

    mPremultipliedAlpha = !(args.flags & ISurfaceComposerClient::eNonPremultiplied);
    mPotentialCursor = args.flags & ISurfaceComposerClient::eCursorWindow;
    mProtectedByApp = args.flags & ISurfaceComposerClient::eProtectedByApp;

    mSnapshot->sequence = sequence;
    mSnapshot->name = getDebugName();
    mSnapshot->premultipliedAlpha = mPremultipliedAlpha;
    mSnapshot->parentTransform = {};
}

void Layer::onFirstRef() {
    mFlinger->onLayerFirstRef(this);
}

Layer::~Layer() {
    LOG_ALWAYS_FATAL_IF(std::this_thread::get_id() != mFlinger->mMainThreadId,
                        "Layer destructor called off the main thread.");

    if (mBufferInfo.mBuffer != nullptr) {
        callReleaseBufferCallback(mDrawingState.releaseBufferListener,
                                  mBufferInfo.mBuffer->getBuffer(), mBufferInfo.mFrameNumber,
                                  mBufferInfo.mFence);
    }
    const int32_t layerId = getSequence();
    mFlinger->mTimeStats->onDestroy(layerId);
    mFlinger->mFrameTracer->onDestroy(layerId);

    mFrameTracker.logAndResetStats(mName);
    mFlinger->onLayerDestroyed(this);

    if (mDrawingState.sidebandStream != nullptr) {
        mFlinger->mTunnelModeEnabledReporter->decrementTunnelModeCount();
    }
    if (hasTrustedPresentationListener()) {
        mFlinger->mNumTrustedPresentationListeners--;
        updateTrustedPresentationState(nullptr, nullptr, -1 /* time_in_ms */, true /* leaveState*/);
    }
}

// ---------------------------------------------------------------------------
// callbacks
// ---------------------------------------------------------------------------

void Layer::removeRelativeZ(const std::vector<Layer*>& layersInTree) {
    if (mDrawingState.zOrderRelativeOf == nullptr) {
        return;
    }

    sp<Layer> strongRelative = mDrawingState.zOrderRelativeOf.promote();
    if (strongRelative == nullptr) {
        setZOrderRelativeOf(nullptr);
        return;
    }

    if (!std::binary_search(layersInTree.begin(), layersInTree.end(), strongRelative.get())) {
        strongRelative->removeZOrderRelative(wp<Layer>::fromExisting(this));
        mFlinger->setTransactionFlags(eTraversalNeeded);
        setZOrderRelativeOf(nullptr);
    }
}

// ---------------------------------------------------------------------------
// set-up
// ---------------------------------------------------------------------------

bool Layer::getPremultipledAlpha() const {
    return mPremultipliedAlpha;
}

sp<IBinder> Layer::getHandle() {
    Mutex::Autolock _l(mLock);
    if (mGetHandleCalled) {
        ALOGE("Get handle called twice" );
        return nullptr;
    }
    mGetHandleCalled = true;
    mHandleAlive = true;
    return sp<LayerHandle>::make(mFlinger, sp<Layer>::fromExisting(this));
}

// ---------------------------------------------------------------------------
// h/w composer set-up
// ---------------------------------------------------------------------------

static Rect reduce(const Rect& win, const Region& exclude) {
    if (CC_LIKELY(exclude.isEmpty())) {
        return win;
    }
    if (exclude.isRect()) {
        return win.reduce(exclude.getBounds());
    }
    return Region(win).subtract(exclude).getBounds();
}

static FloatRect reduce(const FloatRect& win, const Region& exclude) {
    if (CC_LIKELY(exclude.isEmpty())) {
        return win;
    }
    // Convert through Rect (by rounding) for lack of FloatRegion
    return Region(Rect{win}).subtract(exclude).getBounds().toFloatRect();
}

Rect Layer::getScreenBounds(bool reduceTransparentRegion) const {
    if (!reduceTransparentRegion) {
        return Rect{mScreenBounds};
    }

    FloatRect bounds = getBounds();
    ui::Transform t = getTransform();
    // Transform to screen space.
    bounds = t.transform(bounds);
    return Rect{bounds};
}

FloatRect Layer::getBounds() const {
    const State& s(getDrawingState());
    return getBounds(getActiveTransparentRegion(s));
}

FloatRect Layer::getBounds(const Region& activeTransparentRegion) const {
    // Subtract the transparent region and snap to the bounds.
    return reduce(mBounds, activeTransparentRegion);
}

// No early returns.
void Layer::updateTrustedPresentationState(const DisplayDevice* display,
                                           const frontend::LayerSnapshot* snapshot,
                                           int64_t time_in_ms, bool leaveState) {
    if (!hasTrustedPresentationListener()) {
        return;
    }
    const bool lastState = mLastComputedTrustedPresentationState;
    mLastComputedTrustedPresentationState = false;

    if (!leaveState) {
        const auto outputLayer = findOutputLayerForDisplay(display, snapshot->path);
        if (outputLayer != nullptr) {
            if (outputLayer->getState().coveredRegionExcludingDisplayOverlays) {
                Region coveredRegion =
                        *outputLayer->getState().coveredRegionExcludingDisplayOverlays;
                mLastComputedTrustedPresentationState =
                        computeTrustedPresentationState(snapshot->geomLayerBounds,
                                                        snapshot->sourceBounds(), coveredRegion,
                                                        snapshot->transformedBounds,
                                                        snapshot->alpha,
                                                        snapshot->geomLayerTransform,
                                                        mTrustedPresentationThresholds);
            } else {
                ALOGE("CoveredRegionExcludingDisplayOverlays was not set for %s. Don't compute "
                      "TrustedPresentationState",
                      getDebugName());
            }
        }
    }
    const bool newState = mLastComputedTrustedPresentationState;
    if (lastState && !newState) {
        // We were in the trusted presentation state, but now we left it,
        // emit the callback if needed
        if (mLastReportedTrustedPresentationState) {
            mLastReportedTrustedPresentationState = false;
            mTrustedPresentationListener.invoke(false);
        }
        // Reset the timer
        mEnteredTrustedPresentationStateTime = -1;
    } else if (!lastState && newState) {
        // We were not in the trusted presentation state, but we entered it, begin the timer
        // and make sure this gets called at least once more!
        mEnteredTrustedPresentationStateTime = time_in_ms;
        mFlinger->forceFutureUpdate(mTrustedPresentationThresholds.stabilityRequirementMs * 1.5);
    }

    // Has the timer elapsed, but we are still in the state? Emit a callback if needed
    if (!mLastReportedTrustedPresentationState && newState &&
        (time_in_ms - mEnteredTrustedPresentationStateTime >
         mTrustedPresentationThresholds.stabilityRequirementMs)) {
        mLastReportedTrustedPresentationState = true;
        mTrustedPresentationListener.invoke(true);
    }
}

/**
 * See SurfaceComposerClient.h: setTrustedPresentationCallback for discussion
 * of how the parameters and thresholds are interpreted. The general spirit is
 * to produce an upper bound on the amount of the buffer which was presented.
 */
bool Layer::computeTrustedPresentationState(const FloatRect& bounds, const FloatRect& sourceBounds,
                                            const Region& coveredRegion,
                                            const FloatRect& screenBounds, float alpha,
                                            const ui::Transform& effectiveTransform,
                                            const TrustedPresentationThresholds& thresholds) {
    if (alpha < thresholds.minAlpha) {
        return false;
    }
    if (sourceBounds.getWidth() == 0 || sourceBounds.getHeight() == 0) {
        return false;
    }
    if (screenBounds.getWidth() == 0 || screenBounds.getHeight() == 0) {
        return false;
    }

    const float sx = effectiveTransform.dsdx();
    const float sy = effectiveTransform.dsdy();
    float fractionRendered = std::min(sx * sy, 1.0f);

    float boundsOverSourceW = bounds.getWidth() / (float)sourceBounds.getWidth();
    float boundsOverSourceH = bounds.getHeight() / (float)sourceBounds.getHeight();
    fractionRendered *= boundsOverSourceW * boundsOverSourceH;

    Region tJunctionFreeRegion = Region::createTJunctionFreeRegion(coveredRegion);
    // Compute the size of all the rects since they may be disconnected.
    float coveredSize = 0;
    for (auto rect = tJunctionFreeRegion.begin(); rect < tJunctionFreeRegion.end(); rect++) {
        float size = rect->width() * rect->height();
        coveredSize += size;
    }

    fractionRendered *= (1 - (coveredSize / (screenBounds.getWidth() * screenBounds.getHeight())));

    if (fractionRendered < thresholds.minFractionRendered) {
        return false;
    }

    return true;
}

void Layer::computeBounds(FloatRect parentBounds, ui::Transform parentTransform,
                          float parentShadowRadius) {
    const State& s(getDrawingState());

    // Calculate effective layer transform
    mEffectiveTransform = parentTransform * getActiveTransform(s);

    if (CC_UNLIKELY(!isTransformValid())) {
        ALOGW("Stop computing bounds for %s because it has invalid transformation.",
              getDebugName());
        return;
    }

    // Transform parent bounds to layer space
    parentBounds = getActiveTransform(s).inverse().transform(parentBounds);

    // Calculate source bounds
    mSourceBounds = computeSourceBounds(parentBounds);

    // Calculate bounds by croping diplay frame with layer crop and parent bounds
    FloatRect bounds = mSourceBounds;
    const Rect layerCrop = getCrop(s);
    if (!layerCrop.isEmpty()) {
        bounds = mSourceBounds.intersect(layerCrop.toFloatRect());
    }
    bounds = bounds.intersect(parentBounds);

    mBounds = bounds;
    mScreenBounds = mEffectiveTransform.transform(mBounds);

    // Use the layer's own shadow radius if set. Otherwise get the radius from
    // parent.
    if (s.shadowRadius > 0.f) {
        mEffectiveShadowRadius = s.shadowRadius;
    } else {
        mEffectiveShadowRadius = parentShadowRadius;
    }

    // Shadow radius is passed down to only one layer so if the layer can draw shadows,
    // don't pass it to its children.
    const float childShadowRadius = canDrawShadows() ? 0.f : mEffectiveShadowRadius;

    for (const sp<Layer>& child : mDrawingChildren) {
        child->computeBounds(mBounds, mEffectiveTransform, childShadowRadius);
    }

    if (mPotentialCursor) {
        prepareCursorCompositionState();
    }
}

Rect Layer::getCroppedBufferSize(const State& s) const {
    Rect size = getBufferSize(s);
    Rect crop = getCrop(s);
    if (!crop.isEmpty() && size.isValid()) {
        size.intersect(crop, &size);
    } else if (!crop.isEmpty()) {
        size = crop;
    }
    return size;
}

void Layer::setupRoundedCornersCropCoordinates(Rect win,
                                               const FloatRect& roundedCornersCrop) const {
    // Translate win by the rounded corners rect coordinates, to have all values in
    // layer coordinate space.
    win.left -= roundedCornersCrop.left;
    win.right -= roundedCornersCrop.left;
    win.top -= roundedCornersCrop.top;
    win.bottom -= roundedCornersCrop.top;
}

void Layer::prepareBasicGeometryCompositionState() {
    const auto& drawingState{getDrawingState()};
    const auto alpha = static_cast<float>(getAlpha());
    const bool opaque = isOpaque(drawingState);
    const bool usesRoundedCorners = hasRoundedCorners();

    auto blendMode = Hwc2::IComposerClient::BlendMode::NONE;
    if (!opaque || alpha != 1.0f) {
        blendMode = mPremultipliedAlpha ? Hwc2::IComposerClient::BlendMode::PREMULTIPLIED
                                        : Hwc2::IComposerClient::BlendMode::COVERAGE;
    }

    // Please keep in sync with LayerSnapshotBuilder
    auto* snapshot = editLayerSnapshot();
    snapshot->outputFilter = getOutputFilter();
    snapshot->isVisible = isVisible();
    snapshot->isOpaque = opaque && !usesRoundedCorners && alpha == 1.f;
    snapshot->shadowSettings.length = mEffectiveShadowRadius;

    snapshot->contentDirty = contentDirty;
    contentDirty = false;

    snapshot->geomLayerBounds = mBounds;
    snapshot->geomLayerTransform = getTransform();
    snapshot->geomInverseLayerTransform = snapshot->geomLayerTransform.inverse();
    snapshot->transparentRegionHint = getActiveTransparentRegion(drawingState);
    snapshot->localTransform = getActiveTransform(drawingState);
    snapshot->localTransformInverse = snapshot->localTransform.inverse();
    snapshot->blendMode = static_cast<Hwc2::IComposerClient::BlendMode>(blendMode);
    snapshot->alpha = alpha;
    snapshot->backgroundBlurRadius = getBackgroundBlurRadius();
    snapshot->blurRegions = getBlurRegions();
}

void Layer::prepareGeometryCompositionState() {
    const auto& drawingState{getDrawingState()};
    auto* snapshot = editLayerSnapshot();

    // Please keep in sync with LayerSnapshotBuilder
    snapshot->geomBufferSize = getBufferSize(drawingState);
    snapshot->geomContentCrop = getBufferCrop();
    snapshot->geomCrop = getCrop(drawingState);
    snapshot->geomBufferTransform = getBufferTransform();
    snapshot->geomBufferUsesDisplayInverseTransform = getTransformToDisplayInverse();
    snapshot->geomUsesSourceCrop = usesSourceCrop();
    snapshot->isSecure = isSecure();

    snapshot->metadata.clear();
    const auto& supportedMetadata = mFlinger->getHwComposer().getSupportedLayerGenericMetadata();
    for (const auto& [key, mandatory] : supportedMetadata) {
        const auto& genericLayerMetadataCompatibilityMap =
                mFlinger->getGenericLayerMetadataKeyMap();
        auto compatIter = genericLayerMetadataCompatibilityMap.find(key);
        if (compatIter == std::end(genericLayerMetadataCompatibilityMap)) {
            continue;
        }
        const uint32_t id = compatIter->second;

        auto it = drawingState.metadata.mMap.find(id);
        if (it == std::end(drawingState.metadata.mMap)) {
            continue;
        }

        snapshot->metadata.emplace(key,
                                   compositionengine::GenericLayerMetadataEntry{mandatory,
                                                                                it->second});
    }
}

void Layer::preparePerFrameCompositionState() {
    const auto& drawingState{getDrawingState()};
    // Please keep in sync with LayerSnapshotBuilder
    auto* snapshot = editLayerSnapshot();

    snapshot->forceClientComposition = false;

    snapshot->isColorspaceAgnostic = isColorSpaceAgnostic();
    snapshot->dataspace = getDataSpace();
    snapshot->colorTransform = getColorTransform();
    snapshot->colorTransformIsIdentity = !hasColorTransform();
    snapshot->surfaceDamage = surfaceDamageRegion;
    snapshot->hasProtectedContent = isProtected();
    snapshot->dimmingEnabled = isDimmingEnabled();
    snapshot->currentHdrSdrRatio = getCurrentHdrSdrRatio();
    snapshot->desiredHdrSdrRatio = getDesiredHdrSdrRatio();
    snapshot->cachingHint = getCachingHint();

    const bool usesRoundedCorners = hasRoundedCorners();

    snapshot->isOpaque = isOpaque(drawingState) && !usesRoundedCorners && getAlpha() == 1.0_hf;

    // Force client composition for special cases known only to the front-end.
    // Rounded corners no longer force client composition, since we may use a
    // hole punch so that the layer will appear to have rounded corners.
    if (drawShadows() || snapshot->stretchEffect.hasEffect()) {
        snapshot->forceClientComposition = true;
    }
    // If there are no visible region changes, we still need to update blur parameters.
    snapshot->blurRegions = getBlurRegions();
    snapshot->backgroundBlurRadius = getBackgroundBlurRadius();

    // Layer framerate is used in caching decisions.
    // Retrieve it from the scheduler which maintains an instance of LayerHistory, and store it in
    // LayerFECompositionState where it would be visible to Flattener.
    snapshot->fps = mFlinger->getLayerFramerate(systemTime(), getSequence());

    if (hasBufferOrSidebandStream()) {
        preparePerFrameBufferCompositionState();
    } else {
        preparePerFrameEffectsCompositionState();
    }
}

void Layer::preparePerFrameBufferCompositionState() {
    // Please keep in sync with LayerSnapshotBuilder
    auto* snapshot = editLayerSnapshot();
    // Sideband layers
    if (snapshot->sidebandStream.get() && !snapshot->sidebandStreamHasFrame) {
        snapshot->compositionType =
                aidl::android::hardware::graphics::composer3::Composition::SIDEBAND;
        return;
    } else if ((mDrawingState.flags & layer_state_t::eLayerIsDisplayDecoration) != 0) {
        snapshot->compositionType =
                aidl::android::hardware::graphics::composer3::Composition::DISPLAY_DECORATION;
    } else if ((mDrawingState.flags & layer_state_t::eLayerIsRefreshRateIndicator) != 0) {
        snapshot->compositionType =
                aidl::android::hardware::graphics::composer3::Composition::REFRESH_RATE_INDICATOR;
    } else {
        // Normal buffer layers
        snapshot->hdrMetadata = mBufferInfo.mHdrMetadata;
        snapshot->compositionType = mPotentialCursor
                ? aidl::android::hardware::graphics::composer3::Composition::CURSOR
                : aidl::android::hardware::graphics::composer3::Composition::DEVICE;
    }

    snapshot->buffer = getBuffer();
    snapshot->acquireFence = mBufferInfo.mFence;
    snapshot->frameNumber = mBufferInfo.mFrameNumber;
    snapshot->sidebandStreamHasFrame = false;
}

void Layer::preparePerFrameEffectsCompositionState() {
    // Please keep in sync with LayerSnapshotBuilder
    auto* snapshot = editLayerSnapshot();
    snapshot->color = getColor();
    snapshot->compositionType =
            aidl::android::hardware::graphics::composer3::Composition::SOLID_COLOR;
}

void Layer::prepareCursorCompositionState() {
    const State& drawingState{getDrawingState()};
    // Please keep in sync with LayerSnapshotBuilder
    auto* snapshot = editLayerSnapshot();

    // Apply the layer's transform, followed by the display's global transform
    // Here we're guaranteed that the layer's transform preserves rects
    Rect win = getCroppedBufferSize(drawingState);
    // Subtract the transparent region and snap to the bounds
    Rect bounds = reduce(win, getActiveTransparentRegion(drawingState));
    Rect frame(getTransform().transform(bounds));

    snapshot->cursorFrame = frame;
}

const char* Layer::getDebugName() const {
    return mName.c_str();
}

// ---------------------------------------------------------------------------
// drawing...
// ---------------------------------------------------------------------------

aidl::android::hardware::graphics::composer3::Composition Layer::getCompositionType(
        const DisplayDevice& display) const {
    const auto outputLayer = findOutputLayerForDisplay(&display);
    return getCompositionType(outputLayer);
}

aidl::android::hardware::graphics::composer3::Composition Layer::getCompositionType(
        const compositionengine::OutputLayer* outputLayer) const {
    if (outputLayer == nullptr) {
        return aidl::android::hardware::graphics::composer3::Composition::INVALID;
    }
    if (outputLayer->getState().hwc) {
        return (*outputLayer->getState().hwc).hwcCompositionType;
    } else {
        return aidl::android::hardware::graphics::composer3::Composition::CLIENT;
    }
}

// ----------------------------------------------------------------------------
// local state
// ----------------------------------------------------------------------------

bool Layer::isSecure() const {
    const State& s(mDrawingState);
    if (s.flags & layer_state_t::eLayerSecure) {
        return true;
    }

    const auto p = mDrawingParent.promote();
    return (p != nullptr) ? p->isSecure() : false;
}

// ----------------------------------------------------------------------------
// transaction
// ----------------------------------------------------------------------------

uint32_t Layer::doTransaction(uint32_t flags) {
    SFTRACE_CALL();

    // TODO: This is unfortunate.
    mDrawingStateModified = mDrawingState.modified;
    mDrawingState.modified = false;

    const State& s(getDrawingState());

    if (updateGeometry()) {
        // invalidate and recompute the visible regions if needed
        flags |= Layer::eVisibleRegion;
    }

    if (s.sequence != mLastCommittedTxSequence) {
        // invalidate and recompute the visible regions if needed
        mLastCommittedTxSequence = s.sequence;
        flags |= eVisibleRegion;
        this->contentDirty = true;

        // we may use linear filtering, if the matrix scales us
        mNeedsFiltering = getActiveTransform(s).needsBilinearFiltering();
    }

    if (!mPotentialCursor && (flags & Layer::eVisibleRegion)) {
        mFlinger->mUpdateInputInfo = true;
    }

    commitTransaction();

    return flags;
}

void Layer::commitTransaction() {
    // Set the present state for all bufferlessSurfaceFramesTX to Presented. The
    // bufferSurfaceFrameTX will be presented in latchBuffer.
    for (auto& [token, surfaceFrame] : mDrawingState.bufferlessSurfaceFramesTX) {
        if (surfaceFrame->getPresentState() != PresentState::Presented) {
            // With applyPendingStates, we could end up having presented surfaceframes from previous
            // states
            surfaceFrame->setPresentState(PresentState::Presented, mLastLatchTime);
            mFlinger->mFrameTimeline->addSurfaceFrame(surfaceFrame);
        }
    }
    mDrawingState.bufferlessSurfaceFramesTX.clear();
}

uint32_t Layer::clearTransactionFlags(uint32_t mask) {
    const auto flags = mTransactionFlags & mask;
    mTransactionFlags &= ~mask;
    return flags;
}

void Layer::setTransactionFlags(uint32_t mask) {
    mTransactionFlags |= mask;
}

bool Layer::setLayer(int32_t z) {
    if (mDrawingState.z == z && !usingRelativeZ(LayerVector::StateSet::Current)) return false;
    mDrawingState.sequence++;
    mDrawingState.z = z;
    mDrawingState.modified = true;

    mFlinger->mSomeChildrenChanged = true;

    // Discard all relative layering.
    if (mDrawingState.zOrderRelativeOf != nullptr) {
        sp<Layer> strongRelative = mDrawingState.zOrderRelativeOf.promote();
        if (strongRelative != nullptr) {
            strongRelative->removeZOrderRelative(wp<Layer>::fromExisting(this));
        }
        setZOrderRelativeOf(nullptr);
    }
    setTransactionFlags(eTransactionNeeded);
    return true;
}

void Layer::removeZOrderRelative(const wp<Layer>& relative) {
    mDrawingState.zOrderRelatives.remove(relative);
    mDrawingState.sequence++;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
}

void Layer::addZOrderRelative(const wp<Layer>& relative) {
    mDrawingState.zOrderRelatives.add(relative);
    mDrawingState.modified = true;
    mDrawingState.sequence++;
    setTransactionFlags(eTransactionNeeded);
}

void Layer::setZOrderRelativeOf(const wp<Layer>& relativeOf) {
    mDrawingState.zOrderRelativeOf = relativeOf;
    mDrawingState.sequence++;
    mDrawingState.modified = true;
    mDrawingState.isRelativeOf = relativeOf != nullptr;

    setTransactionFlags(eTransactionNeeded);
}

bool Layer::setRelativeLayer(const sp<IBinder>& relativeToHandle, int32_t relativeZ) {
    sp<Layer> relative = LayerHandle::getLayer(relativeToHandle);
    if (relative == nullptr) {
        return false;
    }

    if (mDrawingState.z == relativeZ && usingRelativeZ(LayerVector::StateSet::Current) &&
        mDrawingState.zOrderRelativeOf == relative) {
        return false;
    }

    if (CC_UNLIKELY(relative->usingRelativeZ(LayerVector::StateSet::Drawing)) &&
        (relative->mDrawingState.zOrderRelativeOf == this)) {
        ALOGE("Detected relative layer loop between %s and %s",
              mName.c_str(), relative->mName.c_str());
        ALOGE("Ignoring new call to set relative layer");
        return false;
    }

    mFlinger->mSomeChildrenChanged = true;

    mDrawingState.sequence++;
    mDrawingState.modified = true;
    mDrawingState.z = relativeZ;

    auto oldZOrderRelativeOf = mDrawingState.zOrderRelativeOf.promote();
    if (oldZOrderRelativeOf != nullptr) {
        oldZOrderRelativeOf->removeZOrderRelative(wp<Layer>::fromExisting(this));
    }
    setZOrderRelativeOf(relative);
    relative->addZOrderRelative(wp<Layer>::fromExisting(this));

    setTransactionFlags(eTransactionNeeded);

    return true;
}

bool Layer::setTrustedOverlay(bool isTrustedOverlay) {
    if (mDrawingState.isTrustedOverlay == isTrustedOverlay) return false;
    mDrawingState.isTrustedOverlay = isTrustedOverlay;
    mDrawingState.modified = true;
    mFlinger->mUpdateInputInfo = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::isTrustedOverlay() const {
    if (getDrawingState().isTrustedOverlay) {
        return true;
    }
    const auto& p = mDrawingParent.promote();
    return (p != nullptr) && p->isTrustedOverlay();
}

bool Layer::setAlpha(float alpha) {
    if (mDrawingState.color.a == alpha) return false;
    mDrawingState.sequence++;
    mDrawingState.color.a = alpha;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setCornerRadius(float cornerRadius) {
    if (mDrawingState.cornerRadius == cornerRadius) return false;

    mDrawingState.sequence++;
    mDrawingState.cornerRadius = cornerRadius;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setBackgroundBlurRadius(int backgroundBlurRadius) {
    if (mDrawingState.backgroundBlurRadius == backgroundBlurRadius) return false;
    // If we start or stop drawing blur then the layer's visibility state may change so increment
    // the magic sequence number.
    if (mDrawingState.backgroundBlurRadius == 0 || backgroundBlurRadius == 0) {
        mDrawingState.sequence++;
    }
    mDrawingState.backgroundBlurRadius = backgroundBlurRadius;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setTransparentRegionHint(const Region& transparent) {
    mDrawingState.sequence++;
    mDrawingState.transparentRegionHint = transparent;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setBlurRegions(const std::vector<BlurRegion>& blurRegions) {
    // If we start or stop drawing blur then the layer's visibility state may change so increment
    // the magic sequence number.
    if (mDrawingState.blurRegions.size() == 0 || blurRegions.size() == 0) {
        mDrawingState.sequence++;
    }
    mDrawingState.blurRegions = blurRegions;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setFlags(uint32_t flags, uint32_t mask) {
    const uint32_t newFlags = (mDrawingState.flags & ~mask) | (flags & mask);
    if (mDrawingState.flags == newFlags) return false;
    mDrawingState.sequence++;
    mDrawingState.flags = newFlags;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setCrop(const Rect& crop) {
    if (mDrawingState.crop == crop) return false;
    mDrawingState.sequence++;
    mDrawingState.crop = crop;

    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setMetadata(const LayerMetadata& data) {
    if (!mDrawingState.metadata.merge(data, true /* eraseEmpty */)) return false;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setLayerStack(ui::LayerStack layerStack) {
    if (mDrawingState.layerStack == layerStack) return false;
    mDrawingState.sequence++;
    mDrawingState.layerStack = layerStack;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setColorSpaceAgnostic(const bool agnostic) {
    if (mDrawingState.colorSpaceAgnostic == agnostic) {
        return false;
    }
    mDrawingState.sequence++;
    mDrawingState.colorSpaceAgnostic = agnostic;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setDimmingEnabled(const bool dimmingEnabled) {
    if (mDrawingState.dimmingEnabled == dimmingEnabled) return false;

    mDrawingState.sequence++;
    mDrawingState.dimmingEnabled = dimmingEnabled;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::isLayerFocusedBasedOnPriority(int32_t priority) {
    return priority == PRIORITY_FOCUSED_WITH_MODE || priority == PRIORITY_FOCUSED_WITHOUT_MODE;
};

ui::LayerStack Layer::getLayerStack(LayerVector::StateSet state) const {
    bool useDrawing = state == LayerVector::StateSet::Drawing;
    const auto parent = useDrawing ? mDrawingParent.promote() : mCurrentParent.promote();
    if (parent) {
        return parent->getLayerStack();
    }
    return getDrawingState().layerStack;
}

bool Layer::setShadowRadius(float shadowRadius) {
    if (mDrawingState.shadowRadius == shadowRadius) {
        return false;
    }

    mDrawingState.sequence++;
    mDrawingState.shadowRadius = shadowRadius;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setFixedTransformHint(ui::Transform::RotationFlags fixedTransformHint) {
    if (mDrawingState.fixedTransformHint == fixedTransformHint) {
        return false;
    }

    mDrawingState.sequence++;
    mDrawingState.fixedTransformHint = fixedTransformHint;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setStretchEffect(const StretchEffect& effect) {
    StretchEffect temp = effect;
    temp.sanitize();
    if (mDrawingState.stretchEffect == temp) {
        return false;
    }
    mDrawingState.sequence++;
    mDrawingState.stretchEffect = temp;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

void Layer::setFrameTimelineVsyncForBufferTransaction(const FrameTimelineInfo& info,
                                                      nsecs_t postTime, gui::GameMode gameMode) {
    mDrawingState.postTime = postTime;

    // Check if one of the bufferlessSurfaceFramesTX contains the same vsyncId. This can happen if
    // there are two transactions with the same token, the first one without a buffer and the
    // second one with a buffer. We promote the bufferlessSurfaceFrame to a bufferSurfaceFrameTX
    // in that case.
    auto it = mDrawingState.bufferlessSurfaceFramesTX.find(info.vsyncId);
    if (it != mDrawingState.bufferlessSurfaceFramesTX.end()) {
        // Promote the bufferlessSurfaceFrame to a bufferSurfaceFrameTX
        mDrawingState.bufferSurfaceFrameTX = it->second;
        mDrawingState.bufferlessSurfaceFramesTX.erase(it);
        mDrawingState.bufferSurfaceFrameTX->promoteToBuffer();
        mDrawingState.bufferSurfaceFrameTX->setActualQueueTime(postTime);
    } else {
        mDrawingState.bufferSurfaceFrameTX =
                createSurfaceFrameForBuffer(info, postTime, mTransactionName, gameMode);
    }

    setFrameTimelineVsyncForSkippedFrames(info, postTime, mTransactionName, gameMode);
}

void Layer::setFrameTimelineVsyncForBufferlessTransaction(const FrameTimelineInfo& info,
                                                          nsecs_t postTime,
                                                          gui::GameMode gameMode) {
    mDrawingState.frameTimelineInfo = info;
    mDrawingState.postTime = postTime;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);

    if (const auto& bufferSurfaceFrameTX = mDrawingState.bufferSurfaceFrameTX;
        bufferSurfaceFrameTX != nullptr) {
        if (bufferSurfaceFrameTX->getToken() == info.vsyncId) {
            // BufferSurfaceFrame takes precedence over BufferlessSurfaceFrame. If the same token is
            // being used for BufferSurfaceFrame, don't create a new one.
            return;
        }
    }
    // For Transactions without a buffer, we create only one SurfaceFrame per vsyncId. If multiple
    // transactions use the same vsyncId, we just treat them as one SurfaceFrame (unless they are
    // targeting different vsyncs).
    auto it = mDrawingState.bufferlessSurfaceFramesTX.find(info.vsyncId);
    if (it == mDrawingState.bufferlessSurfaceFramesTX.end()) {
        auto surfaceFrame = createSurfaceFrameForTransaction(info, postTime, gameMode);
        mDrawingState.bufferlessSurfaceFramesTX[info.vsyncId] = surfaceFrame;
    } else {
        if (it->second->getPresentState() == PresentState::Presented) {
            // If the SurfaceFrame was already presented, its safe to overwrite it since it must
            // have been from previous vsync.
            it->second = createSurfaceFrameForTransaction(info, postTime, gameMode);
        }
    }

    setFrameTimelineVsyncForSkippedFrames(info, postTime, mTransactionName, gameMode);
}

void Layer::addSurfaceFrameDroppedForBuffer(
        std::shared_ptr<frametimeline::SurfaceFrame>& surfaceFrame, nsecs_t dropTime) {
    surfaceFrame->setDropTime(dropTime);
    surfaceFrame->setPresentState(PresentState::Dropped);
    mFlinger->mFrameTimeline->addSurfaceFrame(surfaceFrame);
}

void Layer::addSurfaceFramePresentedForBuffer(
        std::shared_ptr<frametimeline::SurfaceFrame>& surfaceFrame, nsecs_t acquireFenceTime,
        nsecs_t currentLatchTime) {
    surfaceFrame->setAcquireFenceTime(acquireFenceTime);
    surfaceFrame->setPresentState(PresentState::Presented, mLastLatchTime);
    mFlinger->mFrameTimeline->addSurfaceFrame(surfaceFrame);
    updateLastLatchTime(currentLatchTime);
}

std::shared_ptr<frametimeline::SurfaceFrame> Layer::createSurfaceFrameForTransaction(
        const FrameTimelineInfo& info, nsecs_t postTime, gui::GameMode gameMode) {
    auto surfaceFrame =
            mFlinger->mFrameTimeline->createSurfaceFrameForToken(info, mOwnerPid, mOwnerUid,
                                                                 getSequence(), mName,
                                                                 mTransactionName,
                                                                 /*isBuffer*/ false, gameMode);
    surfaceFrame->setActualStartTime(info.startTimeNanos);
    // For Transactions, the post time is considered to be both queue and acquire fence time.
    surfaceFrame->setActualQueueTime(postTime);
    surfaceFrame->setAcquireFenceTime(postTime);
    const auto fps = mFlinger->mScheduler->getFrameRateOverride(getOwnerUid());
    if (fps) {
        surfaceFrame->setRenderRate(*fps);
    }
    return surfaceFrame;
}

std::shared_ptr<frametimeline::SurfaceFrame> Layer::createSurfaceFrameForBuffer(
        const FrameTimelineInfo& info, nsecs_t queueTime, std::string debugName,
        gui::GameMode gameMode) {
    auto surfaceFrame =
            mFlinger->mFrameTimeline->createSurfaceFrameForToken(info, mOwnerPid, mOwnerUid,
                                                                 getSequence(), mName, debugName,
                                                                 /*isBuffer*/ true, gameMode);
    surfaceFrame->setActualStartTime(info.startTimeNanos);
    // For buffers, acquire fence time will set during latch.
    surfaceFrame->setActualQueueTime(queueTime);
    const auto fps = mFlinger->mScheduler->getFrameRateOverride(getOwnerUid());
    if (fps) {
        surfaceFrame->setRenderRate(*fps);
    }
    return surfaceFrame;
}

void Layer::setFrameTimelineVsyncForSkippedFrames(const FrameTimelineInfo& info, nsecs_t postTime,
                                                  std::string debugName, gui::GameMode gameMode) {
    if (info.skippedFrameVsyncId == FrameTimelineInfo::INVALID_VSYNC_ID) {
        return;
    }

    FrameTimelineInfo skippedFrameTimelineInfo = info;
    skippedFrameTimelineInfo.vsyncId = info.skippedFrameVsyncId;

    auto surfaceFrame =
            mFlinger->mFrameTimeline->createSurfaceFrameForToken(skippedFrameTimelineInfo,
                                                                 mOwnerPid, mOwnerUid,
                                                                 getSequence(), mName, debugName,
                                                                 /*isBuffer*/ false, gameMode);
    surfaceFrame->setActualStartTime(skippedFrameTimelineInfo.skippedFrameStartTimeNanos);
    // For Transactions, the post time is considered to be both queue and acquire fence time.
    surfaceFrame->setActualQueueTime(postTime);
    surfaceFrame->setAcquireFenceTime(postTime);
    const auto fps = mFlinger->mScheduler->getFrameRateOverride(getOwnerUid());
    if (fps) {
        surfaceFrame->setRenderRate(*fps);
    }
    addSurfaceFrameDroppedForBuffer(surfaceFrame, postTime);
}

bool Layer::setFrameRateForLayerTree(FrameRate frameRate, const scheduler::LayerProps& layerProps,
                                     nsecs_t now) {
    if (mDrawingState.frameRateForLayerTree == frameRate) {
        return false;
    }

    mDrawingState.frameRateForLayerTree = frameRate;
    mFlinger->mScheduler
            ->recordLayerHistory(sequence, layerProps, now, now,
                                 scheduler::LayerHistory::LayerUpdateType::SetFrameRate);
    return true;
}

Layer::FrameRate Layer::getFrameRateForLayerTree() const {
    return getDrawingState().frameRateForLayerTree;
}

bool Layer::isHiddenByPolicy() const {
    const State& s(mDrawingState);
    const auto& parent = mDrawingParent.promote();
    if (parent != nullptr && parent->isHiddenByPolicy()) {
        return true;
    }
    if (usingRelativeZ(LayerVector::StateSet::Drawing)) {
        auto zOrderRelativeOf = mDrawingState.zOrderRelativeOf.promote();
        if (zOrderRelativeOf != nullptr) {
            if (zOrderRelativeOf->isHiddenByPolicy()) {
                return true;
            }
        }
    }
    if (CC_UNLIKELY(!isTransformValid())) {
        ALOGW("Hide layer %s because it has invalid transformation.", getDebugName());
        return true;
    }
    return s.flags & layer_state_t::eLayerHidden;
}

uint32_t Layer::getEffectiveUsage(uint32_t usage) const {
    // TODO: should we do something special if mSecure is set?
    if (mProtectedByApp) {
        // need a hardware-protected path to external video sink
        usage |= GraphicBuffer::USAGE_PROTECTED;
    }
    if (mPotentialCursor) {
        usage |= GraphicBuffer::USAGE_CURSOR;
    }
    usage |= GraphicBuffer::USAGE_HW_COMPOSER;
    return usage;
}

// ----------------------------------------------------------------------------
// debugging
// ----------------------------------------------------------------------------

void Layer::miniDumpHeader(std::string& result) {
    result.append(kDumpTableRowLength, '-');
    result.append("\n");
    result.append(" Layer name\n");
    result.append("           Z | ");
    result.append(" Window Type | ");
    result.append(" Comp Type | ");
    result.append(" Transform | ");
    result.append("  Disp Frame (LTRB) | ");
    result.append("         Source Crop (LTRB) | ");
    result.append("    Frame Rate (Explicit) (Seamlessness) [Focused]\n");
    result.append(kDumpTableRowLength, '-');
    result.append("\n");
}

void Layer::miniDump(std::string& result, const frontend::LayerSnapshot& snapshot,
                     const DisplayDevice& display) const {
    const auto outputLayer = findOutputLayerForDisplay(&display, snapshot.path);
    if (!outputLayer) {
        return;
    }

    StringAppendF(&result, " %s\n", snapshot.debugName.c_str());
    StringAppendF(&result, "  %10zu | ", snapshot.globalZ);
    StringAppendF(&result, "  %10d | ",
                  snapshot.layerMetadata.getInt32(gui::METADATA_WINDOW_TYPE, 0));
    StringAppendF(&result, "%10s | ", toString(getCompositionType(outputLayer)).c_str());
    const auto& outputLayerState = outputLayer->getState();
    StringAppendF(&result, "%10s | ", toString(outputLayerState.bufferTransform).c_str());
    const Rect& frame = outputLayerState.displayFrame;
    StringAppendF(&result, "%4d %4d %4d %4d | ", frame.left, frame.top, frame.right, frame.bottom);
    const FloatRect& crop = outputLayerState.sourceCrop;
    StringAppendF(&result, "%6.1f %6.1f %6.1f %6.1f | ", crop.left, crop.top, crop.right,
                  crop.bottom);
    const auto frameRate = snapshot.frameRate;
    std::string frameRateStr;
    if (frameRate.vote.rate.isValid()) {
        StringAppendF(&frameRateStr, "%.2f", frameRate.vote.rate.getValue());
    }
    if (frameRate.vote.rate.isValid() || frameRate.vote.type != FrameRateCompatibility::Default) {
        StringAppendF(&result, "%6s %15s %17s", frameRateStr.c_str(),
                      ftl::enum_string(frameRate.vote.type).c_str(),
                      ftl::enum_string(frameRate.vote.seamlessness).c_str());
    } else if (frameRate.category != FrameRateCategory::Default) {
        StringAppendF(&result, "%6s %15s %17s", frameRateStr.c_str(),
                      (std::string("Cat::") + ftl::enum_string(frameRate.category)).c_str(),
                      ftl::enum_string(frameRate.vote.seamlessness).c_str());
    } else {
        result.append(41, ' ');
    }

    const auto focused = isLayerFocusedBasedOnPriority(snapshot.frameRateSelectionPriority);
    StringAppendF(&result, "    [%s]\n", focused ? "*" : " ");

    result.append(kDumpTableRowLength, '-');
    result.append("\n");
}

void Layer::dumpFrameStats(std::string& result) const {
    mFrameTracker.dumpStats(result);
}

void Layer::clearFrameStats() {
    mFrameTracker.clearStats();
}

void Layer::logFrameStats() {
    mFrameTracker.logAndResetStats(mName);
}

void Layer::getFrameStats(FrameStats* outStats) const {
    mFrameTracker.getStats(outStats);
}

void Layer::dumpOffscreenDebugInfo(std::string& result) const {
    std::string hasBuffer = hasBufferOrSidebandStream() ? " (contains buffer)" : "";
    StringAppendF(&result, "Layer %s%s pid:%d uid:%d%s\n", getName().c_str(), hasBuffer.c_str(),
                  mOwnerPid, mOwnerUid, isHandleAlive() ? " handleAlive" : "");
}

void Layer::onDisconnect() {
    const int32_t layerId = getSequence();
    mFlinger->mTimeStats->onDestroy(layerId);
    mFlinger->mFrameTracer->onDestroy(layerId);
}

void Layer::setChildrenDrawingParent(const sp<Layer>& newParent) {
    for (const sp<Layer>& child : mDrawingChildren) {
        child->mDrawingParent = newParent;
        const float parentShadowRadius =
                newParent->canDrawShadows() ? 0.f : newParent->mEffectiveShadowRadius;
        child->computeBounds(newParent->mBounds, newParent->mEffectiveTransform,
                             parentShadowRadius);
    }
}

bool Layer::setColorTransform(const mat4& matrix) {
    static const mat4 identityMatrix = mat4();

    if (mDrawingState.colorTransform == matrix) {
        return false;
    }
    ++mDrawingState.sequence;
    mDrawingState.colorTransform = matrix;
    mDrawingState.hasColorTransform = matrix != identityMatrix;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

mat4 Layer::getColorTransform() const {
    mat4 colorTransform = mat4(getDrawingState().colorTransform);
    if (sp<Layer> parent = mDrawingParent.promote(); parent != nullptr) {
        colorTransform = parent->getColorTransform() * colorTransform;
    }
    return colorTransform;
}

bool Layer::hasColorTransform() const {
    bool hasColorTransform = getDrawingState().hasColorTransform;
    if (sp<Layer> parent = mDrawingParent.promote(); parent != nullptr) {
        hasColorTransform = hasColorTransform || parent->hasColorTransform();
    }
    return hasColorTransform;
}

bool Layer::isLegacyDataSpace() const {
    // return true when no higher bits are set
    return !(getDataSpace() &
             (ui::Dataspace::STANDARD_MASK | ui::Dataspace::TRANSFER_MASK |
              ui::Dataspace::RANGE_MASK));
}

void Layer::setParent(const sp<Layer>& layer) {
    mCurrentParent = layer;
}

int32_t Layer::getZ(LayerVector::StateSet) const {
    return mDrawingState.z;
}

bool Layer::usingRelativeZ(LayerVector::StateSet stateSet) const {
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const State& state = useDrawing ? mDrawingState : mDrawingState;
    return state.isRelativeOf;
}

__attribute__((no_sanitize("unsigned-integer-overflow"))) LayerVector Layer::makeTraversalList(
        LayerVector::StateSet stateSet, bool* outSkipRelativeZUsers) {
    LOG_ALWAYS_FATAL_IF(stateSet == LayerVector::StateSet::Invalid,
                        "makeTraversalList received invalid stateSet");
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const LayerVector& children = useDrawing ? mDrawingChildren : mCurrentChildren;
    const State& state = useDrawing ? mDrawingState : mDrawingState;

    if (state.zOrderRelatives.size() == 0) {
        *outSkipRelativeZUsers = true;
        return children;
    }

    LayerVector traverse(stateSet);
    for (const wp<Layer>& weakRelative : state.zOrderRelatives) {
        sp<Layer> strongRelative = weakRelative.promote();
        if (strongRelative != nullptr) {
            traverse.add(strongRelative);
        }
    }

    for (const sp<Layer>& child : children) {
        if (child->usingRelativeZ(stateSet)) {
            continue;
        }
        traverse.add(child);
    }

    return traverse;
}

ui::Transform Layer::getTransform() const {
    return mEffectiveTransform;
}

bool Layer::isTransformValid() const {
    float transformDet = getTransform().det();
    return transformDet != 0 && !isinf(transformDet) && !isnan(transformDet);
}

half Layer::getAlpha() const {
    const auto& p = mDrawingParent.promote();

    half parentAlpha = (p != nullptr) ? p->getAlpha() : 1.0_hf;
    return parentAlpha * getDrawingState().color.a;
}

ui::Transform::RotationFlags Layer::getFixedTransformHint() const {
    ui::Transform::RotationFlags fixedTransformHint = mDrawingState.fixedTransformHint;
    if (fixedTransformHint != ui::Transform::ROT_INVALID) {
        return fixedTransformHint;
    }
    const auto& p = mCurrentParent.promote();
    if (!p) return fixedTransformHint;
    return p->getFixedTransformHint();
}

half4 Layer::getColor() const {
    const half4 color(getDrawingState().color);
    return half4(color.r, color.g, color.b, getAlpha());
}

int32_t Layer::getBackgroundBlurRadius() const {
    if (getDrawingState().backgroundBlurRadius == 0) {
        return 0;
    }

    const auto& p = mDrawingParent.promote();
    half parentAlpha = (p != nullptr) ? p->getAlpha() : 1.0_hf;
    return parentAlpha * getDrawingState().backgroundBlurRadius;
}

const std::vector<BlurRegion> Layer::getBlurRegions() const {
    auto regionsCopy(getDrawingState().blurRegions);
    float layerAlpha = getAlpha();
    for (auto& region : regionsCopy) {
        region.alpha = region.alpha * layerAlpha;
    }
    return regionsCopy;
}

RoundedCornerState Layer::getRoundedCornerState() const {
    // Today's DPUs cannot do rounded corners. If RenderEngine cannot render
    // protected content, remove rounded corners from protected content so it
    // can be rendered by the DPU.
    if (isProtected() && !mFlinger->getRenderEngine().supportsProtectedContent()) {
        return {};
    }

    // Get parent settings
    RoundedCornerState parentSettings;
    const auto& parent = mDrawingParent.promote();
    if (parent != nullptr) {
        parentSettings = parent->getRoundedCornerState();
        if (parentSettings.hasRoundedCorners()) {
            ui::Transform t = getActiveTransform(getDrawingState());
            t = t.inverse();
            parentSettings.cropRect = t.transform(parentSettings.cropRect);
            parentSettings.radius.x *= t.getScaleX();
            parentSettings.radius.y *= t.getScaleY();
        }
    }

    // Get layer settings
    Rect layerCropRect = getCroppedBufferSize(getDrawingState());
    const vec2 radius(getDrawingState().cornerRadius, getDrawingState().cornerRadius);
    RoundedCornerState layerSettings(layerCropRect.toFloatRect(), radius);
    const bool layerSettingsValid = layerSettings.hasRoundedCorners() && layerCropRect.isValid();

    if (layerSettingsValid && parentSettings.hasRoundedCorners()) {
        // If the parent and the layer have rounded corner settings, use the parent settings if the
        // parent crop is entirely inside the layer crop.
        // This has limitations and cause rendering artifacts. See b/200300845 for correct fix.
        if (parentSettings.cropRect.left > layerCropRect.left &&
            parentSettings.cropRect.top > layerCropRect.top &&
            parentSettings.cropRect.right < layerCropRect.right &&
            parentSettings.cropRect.bottom < layerCropRect.bottom) {
            return parentSettings;
        } else {
            return layerSettings;
        }
    } else if (layerSettingsValid) {
        return layerSettings;
    } else if (parentSettings.hasRoundedCorners()) {
        return parentSettings;
    }
    return {};
}

bool Layer::findInHierarchy(const sp<Layer>& l) {
    if (l == this) {
        return true;
    }
    for (auto& child : mDrawingChildren) {
      if (child->findInHierarchy(l)) {
          return true;
      }
    }
    return false;
}

void Layer::setInputInfo(const WindowInfo& info) {
    mDrawingState.inputInfo = info;
    mDrawingState.touchableRegionCrop =
            LayerHandle::getLayer(info.touchableRegionCropHandle.promote());
    mDrawingState.modified = true;
    mFlinger->mUpdateInputInfo = true;
    setTransactionFlags(eTransactionNeeded);
}

perfetto::protos::LayerProto* Layer::writeToProto(perfetto::protos::LayersProto& layersProto,
                                                  uint32_t traceFlags) {
    perfetto::protos::LayerProto* layerProto = layersProto.add_layers();
    writeToProtoDrawingState(layerProto);
    writeToProtoCommonState(layerProto, LayerVector::StateSet::Drawing, traceFlags);

    if (traceFlags & LayerTracing::TRACE_COMPOSITION) {
        ui::LayerStack layerStack =
                (mSnapshot) ? mSnapshot->outputFilter.layerStack : ui::INVALID_LAYER_STACK;
        writeCompositionStateToProto(layerProto, layerStack);
    }

    for (const sp<Layer>& layer : mDrawingChildren) {
        layer->writeToProto(layersProto, traceFlags);
    }

    return layerProto;
}

void Layer::writeCompositionStateToProto(perfetto::protos::LayerProto* layerProto,
                                         ui::LayerStack layerStack) {
    ftl::FakeGuard guard(mFlinger->mStateLock); // Called from the main thread.
    ftl::FakeGuard mainThreadGuard(kMainThreadContext);

    // Only populate for the primary display.
    if (const auto display = mFlinger->getDisplayFromLayerStack(layerStack)) {
        const auto compositionType = getCompositionType(*display);
        layerProto->set_hwc_composition_type(
                static_cast<perfetto::protos::HwcCompositionType>(compositionType));
        LayerProtoHelper::writeToProto(getVisibleRegion(display),
                                       [&]() { return layerProto->mutable_visible_region(); });
    }
}

void Layer::writeToProtoDrawingState(perfetto::protos::LayerProto* layerInfo) {
    const ui::Transform transform = getTransform();
    auto buffer = getExternalTexture();
    if (buffer != nullptr) {
        LayerProtoHelper::writeToProto(*buffer,
                                       [&]() { return layerInfo->mutable_active_buffer(); });
        LayerProtoHelper::writeToProtoDeprecated(ui::Transform(getBufferTransform()),
                                                 layerInfo->mutable_buffer_transform());
    }
    layerInfo->set_invalidate(contentDirty);
    layerInfo->set_is_protected(isProtected());
    layerInfo->set_dataspace(dataspaceDetails(static_cast<android_dataspace>(getDataSpace())));
    layerInfo->set_queued_frames(getQueuedFrameCount());
    layerInfo->set_curr_frame(mCurrentFrameNumber);
    layerInfo->set_requested_corner_radius(getDrawingState().cornerRadius);
    layerInfo->set_corner_radius(
            (getRoundedCornerState().radius.x + getRoundedCornerState().radius.y) / 2.0);
    layerInfo->set_background_blur_radius(getBackgroundBlurRadius());
    layerInfo->set_is_trusted_overlay(isTrustedOverlay());
    LayerProtoHelper::writeToProtoDeprecated(transform, layerInfo->mutable_transform());
    LayerProtoHelper::writePositionToProto(transform.tx(), transform.ty(),
                                           [&]() { return layerInfo->mutable_position(); });
    LayerProtoHelper::writeToProto(mBounds, [&]() { return layerInfo->mutable_bounds(); });
    LayerProtoHelper::writeToProto(surfaceDamageRegion,
                                   [&]() { return layerInfo->mutable_damage_region(); });

    if (hasColorTransform()) {
        LayerProtoHelper::writeToProto(getColorTransform(), layerInfo->mutable_color_transform());
    }

    LayerProtoHelper::writeToProto(mSourceBounds,
                                   [&]() { return layerInfo->mutable_source_bounds(); });
    LayerProtoHelper::writeToProto(mScreenBounds,
                                   [&]() { return layerInfo->mutable_screen_bounds(); });
    LayerProtoHelper::writeToProto(getRoundedCornerState().cropRect,
                                   [&]() { return layerInfo->mutable_corner_radius_crop(); });
    layerInfo->set_shadow_radius(mEffectiveShadowRadius);
}

void Layer::writeToProtoCommonState(perfetto::protos::LayerProto* layerInfo,
                                    LayerVector::StateSet stateSet, uint32_t traceFlags) {
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const LayerVector& children = useDrawing ? mDrawingChildren : mCurrentChildren;
    const State& state = useDrawing ? mDrawingState : mDrawingState;

    ui::Transform requestedTransform = state.transform;

    layerInfo->set_id(sequence);
    layerInfo->set_name(getName().c_str());
    layerInfo->set_type(getType());

    for (const auto& child : children) {
        layerInfo->add_children(child->sequence);
    }

    for (const wp<Layer>& weakRelative : state.zOrderRelatives) {
        sp<Layer> strongRelative = weakRelative.promote();
        if (strongRelative != nullptr) {
            layerInfo->add_relatives(strongRelative->sequence);
        }
    }

    LayerProtoHelper::writeToProto(state.transparentRegionHint,
                                   [&]() { return layerInfo->mutable_transparent_region(); });

    layerInfo->set_layer_stack(getLayerStack().id);
    layerInfo->set_z(state.z);

    LayerProtoHelper::writePositionToProto(requestedTransform.tx(), requestedTransform.ty(), [&]() {
        return layerInfo->mutable_requested_position();
    });

    LayerProtoHelper::writeToProto(state.crop, [&]() { return layerInfo->mutable_crop(); });

    layerInfo->set_is_opaque(isOpaque(state));

    layerInfo->set_pixel_format(decodePixelFormat(getPixelFormat()));
    LayerProtoHelper::writeToProto(getColor(), [&]() { return layerInfo->mutable_color(); });
    LayerProtoHelper::writeToProto(state.color,
                                   [&]() { return layerInfo->mutable_requested_color(); });
    layerInfo->set_flags(state.flags);

    LayerProtoHelper::writeToProtoDeprecated(requestedTransform,
                                             layerInfo->mutable_requested_transform());

    auto parent = useDrawing ? mDrawingParent.promote() : mCurrentParent.promote();
    if (parent != nullptr) {
        layerInfo->set_parent(parent->sequence);
    }

    auto zOrderRelativeOf = state.zOrderRelativeOf.promote();
    if (zOrderRelativeOf != nullptr) {
        layerInfo->set_z_order_relative_of(zOrderRelativeOf->sequence);
    }

    layerInfo->set_is_relative_of(state.isRelativeOf);

    layerInfo->set_owner_uid(mOwnerUid);

    if ((traceFlags & LayerTracing::TRACE_INPUT) && needsInputInfo()) {
        WindowInfo info;
        if (useDrawing) {
            info = fillInputInfo(
                    InputDisplayArgs{.transform = &kIdentityTransform, .isSecure = true});
        } else {
            info = state.inputInfo;
        }

        LayerProtoHelper::writeToProto(info, state.touchableRegionCrop,
                                       [&]() { return layerInfo->mutable_input_window_info(); });
    }

    if (traceFlags & LayerTracing::TRACE_EXTRA) {
        auto protoMap = layerInfo->mutable_metadata();
        for (const auto& entry : state.metadata.mMap) {
            (*protoMap)[entry.first] = std::string(entry.second.cbegin(), entry.second.cend());
        }
    }

    LayerProtoHelper::writeToProto(state.destinationFrame,
                                   [&]() { return layerInfo->mutable_destination_frame(); });
}

// Applies the given transform to the region, while protecting against overflows caused by any
// offsets. If applying the offset in the transform to any of the Rects in the region would result
// in an overflow, they are not added to the output Region.
static Region transformTouchableRegionSafely(const ui::Transform& t, const Region& r,
                                             const std::string& debugWindowName) {
    // Round the translation using the same rounding strategy used by ui::Transform.
    const auto tx = static_cast<int32_t>(t.tx() + 0.5);
    const auto ty = static_cast<int32_t>(t.ty() + 0.5);

    ui::Transform transformWithoutOffset = t;
    transformWithoutOffset.set(0.f, 0.f);

    const Region transformed = transformWithoutOffset.transform(r);

    // Apply the translation to each of the Rects in the region while discarding any that overflow.
    Region ret;
    for (const auto& rect : transformed) {
        Rect newRect;
        if (__builtin_add_overflow(rect.left, tx, &newRect.left) ||
            __builtin_add_overflow(rect.top, ty, &newRect.top) ||
            __builtin_add_overflow(rect.right, tx, &newRect.right) ||
            __builtin_add_overflow(rect.bottom, ty, &newRect.bottom)) {
            ALOGE("Applying transform to touchable region of window '%s' resulted in an overflow.",
                  debugWindowName.c_str());
            continue;
        }
        ret.orSelf(newRect);
    }
    return ret;
}

void Layer::fillInputFrameInfo(WindowInfo& info, const ui::Transform& screenToDisplay) {
    auto [inputBounds, inputBoundsValid] = getInputBounds(/*fillParentBounds=*/false);
    if (!inputBoundsValid) {
        info.touchableRegion.clear();
    }

    info.frame = getInputBoundsInDisplaySpace(inputBounds, screenToDisplay);

    ui::Transform inputToLayer;
    inputToLayer.set(inputBounds.left, inputBounds.top);
    const ui::Transform layerToScreen = getInputTransform();
    const ui::Transform inputToDisplay = screenToDisplay * layerToScreen * inputToLayer;

    // InputDispatcher expects a display-to-input transform.
    info.transform = inputToDisplay.inverse();

    // The touchable region is specified in the input coordinate space. Change it to display space.
    info.touchableRegion =
            transformTouchableRegionSafely(inputToDisplay, info.touchableRegion, mName);
}

void Layer::fillTouchOcclusionMode(WindowInfo& info) {
    sp<Layer> p = sp<Layer>::fromExisting(this);
    while (p != nullptr && !p->hasInputInfo()) {
        p = p->mDrawingParent.promote();
    }
    if (p != nullptr) {
        info.touchOcclusionMode = p->mDrawingState.inputInfo.touchOcclusionMode;
    }
}

gui::DropInputMode Layer::getDropInputMode() const {
    gui::DropInputMode mode = mDrawingState.dropInputMode;
    if (mode == gui::DropInputMode::ALL) {
        return mode;
    }
    sp<Layer> parent = mDrawingParent.promote();
    if (parent) {
        gui::DropInputMode parentMode = parent->getDropInputMode();
        if (parentMode != gui::DropInputMode::NONE) {
            return parentMode;
        }
    }
    return mode;
}

void Layer::handleDropInputMode(gui::WindowInfo& info) const {
    if (mDrawingState.inputInfo.inputConfig.test(WindowInfo::InputConfig::NO_INPUT_CHANNEL)) {
        return;
    }

    // Check if we need to drop input unconditionally
    gui::DropInputMode dropInputMode = getDropInputMode();
    if (dropInputMode == gui::DropInputMode::ALL) {
        info.inputConfig |= WindowInfo::InputConfig::DROP_INPUT;
        ALOGV("Dropping input for %s as requested by policy.", getDebugName());
        return;
    }

    // Check if we need to check if the window is obscured by parent
    if (dropInputMode != gui::DropInputMode::OBSCURED) {
        return;
    }

    // Check if the parent has set an alpha on the layer
    sp<Layer> parent = mDrawingParent.promote();
    if (parent && parent->getAlpha() != 1.0_hf) {
        info.inputConfig |= WindowInfo::InputConfig::DROP_INPUT;
        ALOGV("Dropping input for %s as requested by policy because alpha=%f", getDebugName(),
              static_cast<float>(getAlpha()));
    }

    // Check if the parent has cropped the buffer
    Rect bufferSize = getCroppedBufferSize(getDrawingState());
    if (!bufferSize.isValid()) {
        info.inputConfig |= WindowInfo::InputConfig::DROP_INPUT_IF_OBSCURED;
        return;
    }

    // Screenbounds are the layer bounds cropped by parents, transformed to screenspace.
    // To check if the layer has been cropped, we take the buffer bounds, apply the local
    // layer crop and apply the same set of transforms to move to screenspace. If the bounds
    // match then the layer has not been cropped by its parents.
    Rect bufferInScreenSpace(getTransform().transform(bufferSize));
    bool croppedByParent = bufferInScreenSpace != Rect{mScreenBounds};

    if (croppedByParent) {
        info.inputConfig |= WindowInfo::InputConfig::DROP_INPUT;
        ALOGV("Dropping input for %s as requested by policy because buffer is cropped by parent",
              getDebugName());
    } else {
        // If the layer is not obscured by its parents (by setting an alpha or crop), then only drop
        // input if the window is obscured. This check should be done in surfaceflinger but the
        // logic currently resides in inputflinger. So pass the if_obscured check to input to only
        // drop input events if the window is obscured.
        info.inputConfig |= WindowInfo::InputConfig::DROP_INPUT_IF_OBSCURED;
    }
}

WindowInfo Layer::fillInputInfo(const InputDisplayArgs& displayArgs) {
    if (!hasInputInfo()) {
        mDrawingState.inputInfo.name = getName();
        mDrawingState.inputInfo.ownerUid = gui::Uid{mOwnerUid};
        mDrawingState.inputInfo.ownerPid = gui::Pid{mOwnerPid};
        mDrawingState.inputInfo.inputConfig |= WindowInfo::InputConfig::NO_INPUT_CHANNEL;
        mDrawingState.inputInfo.displayId = toLogicalDisplayId(getLayerStack());
    }

    const ui::Transform& displayTransform =
            displayArgs.transform != nullptr ? *displayArgs.transform : kIdentityTransform;

    WindowInfo info = mDrawingState.inputInfo;
    info.id = sequence;
    info.displayId = toLogicalDisplayId(getLayerStack());

    fillInputFrameInfo(info, displayTransform);

    if (displayArgs.transform == nullptr) {
        // Do not let the window receive touches if it is not associated with a valid display
        // transform. We still allow the window to receive keys and prevent ANRs.
        info.inputConfig |= WindowInfo::InputConfig::NOT_TOUCHABLE;
    }

    info.setInputConfig(WindowInfo::InputConfig::NOT_VISIBLE, !isVisibleForInput());

    info.alpha = getAlpha();
    fillTouchOcclusionMode(info);
    handleDropInputMode(info);

    // If the window will be blacked out on a display because the display does not have the secure
    // flag and the layer has the secure flag set, then drop input.
    if (!displayArgs.isSecure && isSecure()) {
        info.inputConfig |= WindowInfo::InputConfig::DROP_INPUT;
    }

    sp<Layer> cropLayer = mDrawingState.touchableRegionCrop.promote();
    if (info.replaceTouchableRegionWithCrop) {
        Rect inputBoundsInDisplaySpace;
        if (!cropLayer) {
            FloatRect inputBounds = getInputBounds(/*fillParentBounds=*/true).first;
            inputBoundsInDisplaySpace = getInputBoundsInDisplaySpace(inputBounds, displayTransform);
        } else {
            FloatRect inputBounds = cropLayer->getInputBounds(/*fillParentBounds=*/true).first;
            inputBoundsInDisplaySpace =
                    cropLayer->getInputBoundsInDisplaySpace(inputBounds, displayTransform);
        }
        info.touchableRegion = Region(inputBoundsInDisplaySpace);
    } else if (cropLayer != nullptr) {
        FloatRect inputBounds = cropLayer->getInputBounds(/*fillParentBounds=*/true).first;
        Rect inputBoundsInDisplaySpace =
                cropLayer->getInputBoundsInDisplaySpace(inputBounds, displayTransform);
        info.touchableRegion = info.touchableRegion.intersect(inputBoundsInDisplaySpace);
    }

    // Inherit the trusted state from the parent hierarchy, but don't clobber the trusted state
    // if it was set by WM for a known system overlay
    if (isTrustedOverlay()) {
        info.inputConfig |= WindowInfo::InputConfig::TRUSTED_OVERLAY;
    }

    Rect bufferSize = getBufferSize(getDrawingState());
    info.contentSize = Size(bufferSize.width(), bufferSize.height());

    return info;
}

Rect Layer::getInputBoundsInDisplaySpace(const FloatRect& inputBounds,
                                         const ui::Transform& screenToDisplay) {
    // InputDispatcher works in the display device's coordinate space. Here, we calculate the
    // frame and transform used for the layer, which determines the bounds and the coordinate space
    // within which the layer will receive input.

    // Coordinate space definitions:
    //   - display: The display device's coordinate space. Correlates to pixels on the display.
    //   - screen: The post-rotation coordinate space for the display, a.k.a. logical display space.
    //   - layer: The coordinate space of this layer.
    //   - input: The coordinate space in which this layer will receive input events. This could be
    //            different than layer space if a surfaceInset is used, which changes the origin
    //            of the input space.

    // Crop the input bounds to ensure it is within the parent's bounds.
    const FloatRect croppedInputBounds = mBounds.intersect(inputBounds);
    const ui::Transform layerToScreen = getInputTransform();
    const ui::Transform layerToDisplay = screenToDisplay * layerToScreen;
    return Rect{layerToDisplay.transform(croppedInputBounds)};
}

bool Layer::hasInputInfo() const {
    return mDrawingState.inputInfo.token != nullptr ||
            mDrawingState.inputInfo.inputConfig.test(WindowInfo::InputConfig::NO_INPUT_CHANNEL);
}

compositionengine::OutputLayer* Layer::findOutputLayerForDisplay(
        const DisplayDevice* display) const {
    if (!display) return nullptr;
    sp<LayerFE> layerFE;
    frontend::LayerHierarchy::TraversalPath path{.id = static_cast<uint32_t>(sequence)};
    for (auto& [p, layer] : mLayerFEs) {
        if (p == path) {
            layerFE = layer;
        }
    }

    if (!layerFE) return nullptr;
    return display->getCompositionDisplay()->getOutputLayerForLayer(layerFE);
}

compositionengine::OutputLayer* Layer::findOutputLayerForDisplay(
        const DisplayDevice* display, const frontend::LayerHierarchy::TraversalPath& path) const {
    if (!display) return nullptr;
    sp<LayerFE> layerFE;
    for (auto& [p, layer] : mLayerFEs) {
        if (p == path) {
            layerFE = layer;
        }
    }

    if (!layerFE) return nullptr;
    return display->getCompositionDisplay()->getOutputLayerForLayer(layerFE);
}

Region Layer::getVisibleRegion(const DisplayDevice* display) const {
    const auto outputLayer = findOutputLayerForDisplay(display);
    return outputLayer ? outputLayer->getState().visibleRegion : Region();
}

bool Layer::isInternalDisplayOverlay() const {
    const State& s(mDrawingState);
    if (s.flags & layer_state_t::eLayerSkipScreenshot) {
        return true;
    }

    sp<Layer> parent = mDrawingParent.promote();
    return parent && parent->isInternalDisplayOverlay();
}

bool Layer::setDropInputMode(gui::DropInputMode mode) {
    if (mDrawingState.dropInputMode == mode) {
        return false;
    }
    mDrawingState.dropInputMode = mode;
    return true;
}

void Layer::callReleaseBufferCallback(const sp<ITransactionCompletedListener>& listener,
                                      const sp<GraphicBuffer>& buffer, uint64_t framenumber,
                                      const sp<Fence>& releaseFence) {
    if (!listener) {
        return;
    }
    SFTRACE_FORMAT_INSTANT("callReleaseBufferCallback %s - %" PRIu64, getDebugName(), framenumber);
    uint32_t currentMaxAcquiredBufferCount =
            mFlinger->getMaxAcquiredBufferCountForCurrentRefreshRate(mOwnerUid);
    listener->onReleaseBuffer({buffer->getId(), framenumber},
                              releaseFence ? releaseFence : Fence::NO_FENCE,
                              currentMaxAcquiredBufferCount);
}

sp<CallbackHandle> Layer::findCallbackHandle() {
    // If we are displayed on multiple displays in a single composition cycle then we would
    // need to do careful tracking to enable the use of the mLastClientCompositionFence.
    //  For example we can only use it if all the displays are client comp, and we need
    //  to merge all the client comp fences. We could do this, but for now we just
    // disable the optimization when a layer is composed on multiple displays.
    if (mClearClientCompositionFenceOnLayerDisplayed) {
        mLastClientCompositionFence = nullptr;
    } else {
        mClearClientCompositionFenceOnLayerDisplayed = true;
    }

    // The previous release fence notifies the client that SurfaceFlinger is done with the previous
    // buffer that was presented on this layer. The first transaction that came in this frame that
    // replaced the previous buffer on this layer needs this release fence, because the fence will
    // let the client know when that previous buffer is removed from the screen.
    //
    // Every other transaction on this layer does not need a release fence because no other
    // Transactions that were set on this layer this frame are going to have their preceding buffer
    // removed from the display this frame.
    //
    // For example, if we have 3 transactions this frame. The first transaction doesn't contain a
    // buffer so it doesn't need a previous release fence because the layer still needs the previous
    // buffer. The second transaction contains a buffer so it needs a previous release fence because
    // the previous buffer will be released this frame. The third transaction also contains a
    // buffer. It replaces the buffer in the second transaction. The buffer in the second
    // transaction will now no longer be presented so it is released immediately and the third
    // transaction doesn't need a previous release fence.
    sp<CallbackHandle> ch;
    for (auto& handle : mDrawingState.callbackHandles) {
        if (handle->releasePreviousBuffer && mPreviousReleaseBufferEndpoint == handle->listener) {
            ch = handle;
            break;
        }
    }
    return ch;
}

void Layer::prepareReleaseCallbacks(ftl::Future<FenceResult> futureFenceResult,
                                    ui::LayerStack layerStack) {
    sp<CallbackHandle> ch = findCallbackHandle();

    if (ch != nullptr) {
        ch->previousReleaseCallbackId = mPreviousReleaseCallbackId;
        ch->previousReleaseFences.emplace_back(std::move(futureFenceResult));
        ch->name = mName;
    } else {
        // If we didn't get a release callback yet (e.g. some scenarios when capturing
        // screenshots asynchronously) then make sure we don't drop the fence.
        // Older fences for the same layer stack can be dropped when a new fence arrives.
        // An assumption here is that RenderEngine performs work sequentially, so an
        // incoming fence will not fire before an existing fence.
        mAdditionalPreviousReleaseFences.emplace_or_replace(layerStack,
                                                            std::move(futureFenceResult));
    }

    if (mBufferInfo.mBuffer) {
        mPreviouslyPresentedLayerStacks.push_back(layerStack);
    }

    if (mDrawingState.frameNumber > 0) {
        mDrawingState.previousFrameNumber = mDrawingState.frameNumber;
    }
}

void Layer::onLayerDisplayed(ftl::SharedFuture<FenceResult> futureFenceResult,
                             ui::LayerStack layerStack,
                             std::function<FenceResult(FenceResult)>&& continuation) {
    sp<CallbackHandle> ch = findCallbackHandle();

    if (!FlagManager::getInstance().screenshot_fence_preservation() && continuation) {
        futureFenceResult = ftl::Future(futureFenceResult).then(std::move(continuation)).share();
    }

    if (ch != nullptr) {
        ch->previousReleaseCallbackId = mPreviousReleaseCallbackId;
        ch->previousSharedReleaseFences.emplace_back(std::move(futureFenceResult));
        ch->name = mName;
    } else if (FlagManager::getInstance().screenshot_fence_preservation()) {
        // If we didn't get a release callback yet, e.g. some scenarios when capturing screenshots
        // asynchronously, then make sure we don't drop the fence.
        mPreviousReleaseFenceAndContinuations.emplace_back(std::move(futureFenceResult),
                                                           std::move(continuation));
        std::vector<FenceAndContinuation> mergedFences;
        sp<Fence> prevFence = nullptr;
        // For a layer that's frequently screenshotted, try to merge fences to make sure we don't
        // grow unbounded.
        for (const auto& futureAndContinuation : mPreviousReleaseFenceAndContinuations) {
            auto result = futureAndContinuation.future.wait_for(0s);
            if (result != std::future_status::ready) {
                mergedFences.emplace_back(futureAndContinuation);
                continue;
            }

            mergeFence(getDebugName(),
                       futureAndContinuation.chain().get().value_or(Fence::NO_FENCE), prevFence);
        }
        if (prevFence != nullptr) {
            mergedFences.emplace_back(ftl::yield(FenceResult(std::move(prevFence))).share());
        }

        mPreviousReleaseFenceAndContinuations.swap(mergedFences);
    }

    if (mBufferInfo.mBuffer) {
        mPreviouslyPresentedLayerStacks.push_back(layerStack);
    }

    if (mDrawingState.frameNumber > 0) {
        mDrawingState.previousFrameNumber = mDrawingState.frameNumber;
    }
}

void Layer::releasePendingBuffer(nsecs_t dequeueReadyTime) {
    for (const auto& handle : mDrawingState.callbackHandles) {
        handle->transformHint = mTransformHint;
        handle->dequeueReadyTime = dequeueReadyTime;
        handle->currentMaxAcquiredBufferCount =
                mFlinger->getMaxAcquiredBufferCountForCurrentRefreshRate(mOwnerUid);
        SFTRACE_FORMAT_INSTANT("releasePendingBuffer %s - %" PRIu64, getDebugName(),
                               handle->previousReleaseCallbackId.framenumber);
    }

    for (auto& handle : mDrawingState.callbackHandles) {
        if (handle->releasePreviousBuffer && mPreviousReleaseBufferEndpoint == handle->listener) {
            handle->previousReleaseCallbackId = mPreviousReleaseCallbackId;
            break;
        }
    }

    mFlinger->getTransactionCallbackInvoker().addCallbackHandles(mDrawingState.callbackHandles);
    mDrawingState.callbackHandles = {};
}

bool Layer::willPresentCurrentTransaction() const {
    // Returns true if the most recent Transaction applied to CurrentState will be presented.
    return (getSidebandStreamChanged() || getAutoRefresh() ||
            (mDrawingState.modified &&
             (mDrawingState.buffer != nullptr || mDrawingState.bgColorLayer != nullptr)));
}

bool Layer::setTransform(uint32_t transform) {
    if (mDrawingState.bufferTransform == transform) return false;
    mDrawingState.bufferTransform = transform;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setTransformToDisplayInverse(bool transformToDisplayInverse) {
    if (mDrawingState.transformToDisplayInverse == transformToDisplayInverse) return false;
    mDrawingState.sequence++;
    mDrawingState.transformToDisplayInverse = transformToDisplayInverse;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setBufferCrop(const Rect& bufferCrop) {
    if (mDrawingState.bufferCrop == bufferCrop) return false;

    mDrawingState.sequence++;
    mDrawingState.bufferCrop = bufferCrop;

    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setDestinationFrame(const Rect& destinationFrame) {
    if (mDrawingState.destinationFrame == destinationFrame) return false;

    mDrawingState.sequence++;
    mDrawingState.destinationFrame = destinationFrame;

    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

// Translate destination frame into scale and position. If a destination frame is not set, use the
// provided scale and position
bool Layer::updateGeometry() {
    if ((mDrawingState.flags & layer_state_t::eIgnoreDestinationFrame) ||
        mDrawingState.destinationFrame.isEmpty()) {
        // If destination frame is not set, use the requested transform set via
        // Layer::setPosition and Layer::setMatrix.
        return assignTransform(&mDrawingState.transform, mRequestedTransform);
    }

    Rect destRect = mDrawingState.destinationFrame;
    int32_t destW = destRect.width();
    int32_t destH = destRect.height();
    if (destRect.left < 0) {
        destRect.left = 0;
        destRect.right = destW;
    }
    if (destRect.top < 0) {
        destRect.top = 0;
        destRect.bottom = destH;
    }

    if (!mDrawingState.buffer) {
        ui::Transform t;
        t.set(destRect.left, destRect.top);
        return assignTransform(&mDrawingState.transform, t);
    }

    uint32_t bufferWidth = mDrawingState.buffer->getWidth();
    uint32_t bufferHeight = mDrawingState.buffer->getHeight();
    // Undo any transformations on the buffer.
    if (mDrawingState.bufferTransform & ui::Transform::ROT_90) {
        std::swap(bufferWidth, bufferHeight);
    }
    uint32_t invTransform = SurfaceFlinger::getActiveDisplayRotationFlags();
    if (mDrawingState.transformToDisplayInverse) {
        if (invTransform & ui::Transform::ROT_90) {
            std::swap(bufferWidth, bufferHeight);
        }
    }

    float sx = destW / static_cast<float>(bufferWidth);
    float sy = destH / static_cast<float>(bufferHeight);
    ui::Transform t;
    t.set(sx, 0, 0, sy);
    t.set(destRect.left, destRect.top);
    return assignTransform(&mDrawingState.transform, t);
}

bool Layer::setMatrix(const layer_state_t::matrix22_t& matrix) {
    if (mRequestedTransform.dsdx() == matrix.dsdx && mRequestedTransform.dtdy() == matrix.dtdy &&
        mRequestedTransform.dtdx() == matrix.dtdx && mRequestedTransform.dsdy() == matrix.dsdy) {
        return false;
    }

    mRequestedTransform.set(matrix.dsdx, matrix.dtdy, matrix.dtdx, matrix.dsdy);

    mDrawingState.sequence++;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);

    return true;
}

bool Layer::setPosition(float x, float y) {
    if (mRequestedTransform.tx() == x && mRequestedTransform.ty() == y) {
        return false;
    }

    mRequestedTransform.set(x, y);

    mDrawingState.sequence++;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);

    return true;
}

void Layer::releasePreviousBuffer() {
    mReleasePreviousBuffer = true;
    if (!mBufferInfo.mBuffer ||
        (!mDrawingState.buffer->hasSameBuffer(*mBufferInfo.mBuffer) ||
         mDrawingState.frameNumber != mBufferInfo.mFrameNumber)) {
        // If mDrawingState has a buffer, and we are about to update again
        // before swapping to drawing state, then the first buffer will be
        // dropped and we should decrement the pending buffer count and
        // call any release buffer callbacks if set.
        callReleaseBufferCallback(mDrawingState.releaseBufferListener,
                                  mDrawingState.buffer->getBuffer(), mDrawingState.frameNumber,
                                  mDrawingState.acquireFence);
        const int32_t layerId = getSequence();
        mFlinger->mTimeStats->removeTimeRecord(layerId, mDrawingState.frameNumber);
        decrementPendingBufferCount();
        if (mDrawingState.bufferSurfaceFrameTX != nullptr &&
            mDrawingState.bufferSurfaceFrameTX->getPresentState() != PresentState::Presented) {
            addSurfaceFrameDroppedForBuffer(mDrawingState.bufferSurfaceFrameTX, systemTime());
            mDrawingState.bufferSurfaceFrameTX.reset();
        }
    } else if (EARLY_RELEASE_ENABLED && mLastClientCompositionFence != nullptr) {
        callReleaseBufferCallback(mDrawingState.releaseBufferListener,
                                  mDrawingState.buffer->getBuffer(), mDrawingState.frameNumber,
                                  mLastClientCompositionFence);
        mLastClientCompositionFence = nullptr;
    }
}

void Layer::resetDrawingStateBufferInfo() {
    mDrawingState.producerId = 0;
    mDrawingState.frameNumber = 0;
    mDrawingState.previousFrameNumber = 0;
    mDrawingState.releaseBufferListener = nullptr;
    mDrawingState.buffer = nullptr;
    mDrawingState.acquireFence = sp<Fence>::make(-1);
    mDrawingState.acquireFenceTime = std::make_unique<FenceTime>(mDrawingState.acquireFence);
    mCallbackHandleAcquireTimeOrFence = mDrawingState.acquireFenceTime->getSignalTime();
    mDrawingState.releaseBufferEndpoint = nullptr;
}

bool Layer::setBuffer(std::shared_ptr<renderengine::ExternalTexture>& buffer,
                      const BufferData& bufferData, nsecs_t postTime, nsecs_t desiredPresentTime,
                      bool isAutoTimestamp, const FrameTimelineInfo& info, gui::GameMode gameMode) {
    SFTRACE_FORMAT("setBuffer %s - hasBuffer=%s", getDebugName(), (buffer ? "true" : "false"));

    const bool frameNumberChanged =
            bufferData.flags.test(BufferData::BufferDataChange::frameNumberChanged);
    const uint64_t frameNumber =
            frameNumberChanged ? bufferData.frameNumber : mDrawingState.frameNumber + 1;
    SFTRACE_FORMAT_INSTANT("setBuffer %s - %" PRIu64, getDebugName(), frameNumber);

    if (mDrawingState.buffer) {
        releasePreviousBuffer();
    } else if (buffer) {
        // if we are latching a buffer for the first time then clear the mLastLatchTime since
        // we don't want to incorrectly classify a frame if we miss the desired present time.
        updateLastLatchTime(0);
    }

    mDrawingState.desiredPresentTime = desiredPresentTime;
    mDrawingState.isAutoTimestamp = isAutoTimestamp;
    mDrawingState.latchedVsyncId = info.vsyncId;
    mDrawingState.useVsyncIdForRefreshRateSelection = info.useForRefreshRateSelection;
    mDrawingState.modified = true;
    if (!buffer) {
        resetDrawingStateBufferInfo();
        setTransactionFlags(eTransactionNeeded);
        mDrawingState.bufferSurfaceFrameTX = nullptr;
        setFrameTimelineVsyncForBufferlessTransaction(info, postTime, gameMode);
        return true;
    } else {
        // release sideband stream if it exists and a non null buffer is being set
        if (mDrawingState.sidebandStream != nullptr) {
            setSidebandStream(nullptr, info, postTime, gameMode);
        }
    }

    if ((mDrawingState.producerId > bufferData.producerId) ||
        ((mDrawingState.producerId == bufferData.producerId) &&
         (mDrawingState.frameNumber > frameNumber))) {
        ALOGE("Out of order buffers detected for %s producedId=%d frameNumber=%" PRIu64
              " -> producedId=%d frameNumber=%" PRIu64,
              getDebugName(), mDrawingState.producerId, mDrawingState.frameNumber,
              bufferData.producerId, frameNumber);
        TransactionTraceWriter::getInstance().invoke("out_of_order_buffers_", /*overwrite=*/false);
    }

    mDrawingState.producerId = bufferData.producerId;
    mDrawingState.barrierProducerId =
            std::max(mDrawingState.producerId, mDrawingState.barrierProducerId);
    mDrawingState.frameNumber = frameNumber;
    mDrawingState.barrierFrameNumber =
            std::max(mDrawingState.frameNumber, mDrawingState.barrierFrameNumber);

    mDrawingState.releaseBufferListener = bufferData.releaseBufferListener;
    mDrawingState.buffer = std::move(buffer);
    mDrawingState.acquireFence = bufferData.flags.test(BufferData::BufferDataChange::fenceChanged)
            ? bufferData.acquireFence
            : Fence::NO_FENCE;
    mDrawingState.acquireFenceTime = std::make_unique<FenceTime>(mDrawingState.acquireFence);
    if (mDrawingState.acquireFenceTime->getSignalTime() == Fence::SIGNAL_TIME_PENDING) {
        // We latched this buffer unsiganled, so we need to pass the acquire fence
        // on the callback instead of just the acquire time, since it's unknown at
        // this point.
        mCallbackHandleAcquireTimeOrFence = mDrawingState.acquireFence;
    } else {
        mCallbackHandleAcquireTimeOrFence = mDrawingState.acquireFenceTime->getSignalTime();
    }
    setTransactionFlags(eTransactionNeeded);

    const int32_t layerId = getSequence();
    mFlinger->mTimeStats->setPostTime(layerId, mDrawingState.frameNumber, getName().c_str(),
                                      mOwnerUid, postTime, gameMode);

    setFrameTimelineVsyncForBufferTransaction(info, postTime, gameMode);

    if (bufferData.dequeueTime > 0) {
        const uint64_t bufferId = mDrawingState.buffer->getId();
        mFlinger->mFrameTracer->traceNewLayer(layerId, getName().c_str());
        mFlinger->mFrameTracer->traceTimestamp(layerId, bufferId, frameNumber,
                                               bufferData.dequeueTime,
                                               FrameTracer::FrameEvent::DEQUEUE);
        mFlinger->mFrameTracer->traceTimestamp(layerId, bufferId, frameNumber, postTime,
                                               FrameTracer::FrameEvent::QUEUE);
    }

    mDrawingState.releaseBufferEndpoint = bufferData.releaseBufferEndpoint;

    // If the layer had been updated a TextureView, this would make sure the present time could be
    // same to TextureView update when it's a small dirty, and get the correct heuristic rate.
    if (mFlinger->mScheduler->supportSmallDirtyDetection(mOwnerAppId)) {
        if (mDrawingState.useVsyncIdForRefreshRateSelection) {
            mUsedVsyncIdForRefreshRateSelection = true;
        }
    }
    return true;
}

void Layer::setDesiredPresentTime(nsecs_t desiredPresentTime, bool isAutoTimestamp) {
    mDrawingState.desiredPresentTime = desiredPresentTime;
    mDrawingState.isAutoTimestamp = isAutoTimestamp;
}

void Layer::recordLayerHistoryBufferUpdate(const scheduler::LayerProps& layerProps, nsecs_t now) {
    SFTRACE_CALL();
    const nsecs_t presentTime = [&] {
        if (!mDrawingState.isAutoTimestamp) {
            SFTRACE_FORMAT_INSTANT("desiredPresentTime");
            return mDrawingState.desiredPresentTime;
        }

        if (mDrawingState.useVsyncIdForRefreshRateSelection) {
            const auto prediction =
                    mFlinger->mFrameTimeline->getTokenManager()->getPredictionsForToken(
                            mDrawingState.latchedVsyncId);
            if (prediction.has_value()) {
                SFTRACE_FORMAT_INSTANT("predictedPresentTime");
                mMaxTimeForUseVsyncId = prediction->presentTime +
                        scheduler::LayerHistory::kMaxPeriodForHistory.count();
                return prediction->presentTime;
            }
        }

        if (!mFlinger->mScheduler->supportSmallDirtyDetection(mOwnerAppId)) {
            return static_cast<nsecs_t>(0);
        }

        // If the layer is not an application and didn't set an explicit rate or desiredPresentTime,
        // return "0" to tell the layer history that it will use the max refresh rate without
        // calculating the adaptive rate.
        if (mWindowType != WindowInfo::Type::APPLICATION &&
            mWindowType != WindowInfo::Type::BASE_APPLICATION) {
            return static_cast<nsecs_t>(0);
        }

        // Return the valid present time only when the layer potentially updated a TextureView so
        // LayerHistory could heuristically calculate the rate if the UI is continually updating.
        if (mUsedVsyncIdForRefreshRateSelection) {
            const auto prediction =
                    mFlinger->mFrameTimeline->getTokenManager()->getPredictionsForToken(
                            mDrawingState.latchedVsyncId);
            if (prediction.has_value()) {
                if (mMaxTimeForUseVsyncId >= prediction->presentTime) {
                    return prediction->presentTime;
                }
                mUsedVsyncIdForRefreshRateSelection = false;
            }
        }

        return static_cast<nsecs_t>(0);
    }();

    if (SFTRACE_ENABLED() && presentTime > 0) {
        const auto presentIn = TimePoint::fromNs(presentTime) - TimePoint::now();
        SFTRACE_FORMAT_INSTANT("presentIn %s", to_string(presentIn).c_str());
    }

    mFlinger->mScheduler->recordLayerHistory(sequence, layerProps, presentTime, now,
                                             scheduler::LayerHistory::LayerUpdateType::Buffer);
}

void Layer::recordLayerHistoryAnimationTx(const scheduler::LayerProps& layerProps, nsecs_t now) {
    const nsecs_t presentTime =
            mDrawingState.isAutoTimestamp ? 0 : mDrawingState.desiredPresentTime;
    mFlinger->mScheduler->recordLayerHistory(sequence, layerProps, presentTime, now,
                                             scheduler::LayerHistory::LayerUpdateType::AnimationTX);
}

bool Layer::setDataspace(ui::Dataspace dataspace) {
    if (mDrawingState.dataspace == dataspace) return false;
    mDrawingState.dataspace = dataspace;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setExtendedRangeBrightness(float currentBufferRatio, float desiredRatio) {
    if (mDrawingState.currentHdrSdrRatio == currentBufferRatio &&
        mDrawingState.desiredHdrSdrRatio == desiredRatio)
        return false;
    mDrawingState.currentHdrSdrRatio = currentBufferRatio;
    mDrawingState.desiredHdrSdrRatio = desiredRatio;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setDesiredHdrHeadroom(float desiredRatio) {
    if (mDrawingState.desiredHdrSdrRatio == desiredRatio) return false;
    mDrawingState.desiredHdrSdrRatio = desiredRatio;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setCachingHint(gui::CachingHint cachingHint) {
    if (mDrawingState.cachingHint == cachingHint) return false;
    mDrawingState.cachingHint = cachingHint;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setHdrMetadata(const HdrMetadata& hdrMetadata) {
    if (mDrawingState.hdrMetadata == hdrMetadata) return false;
    mDrawingState.hdrMetadata = hdrMetadata;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setSurfaceDamageRegion(const Region& surfaceDamage) {
    if (mDrawingState.surfaceDamageRegion.hasSameRects(surfaceDamage)) return false;
    mDrawingState.surfaceDamageRegion = surfaceDamage;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    setIsSmallDirty(surfaceDamage, getTransform());
    return true;
}

bool Layer::setApi(int32_t api) {
    if (mDrawingState.api == api) return false;
    mDrawingState.api = api;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setSidebandStream(const sp<NativeHandle>& sidebandStream, const FrameTimelineInfo& info,
                              nsecs_t postTime, gui::GameMode gameMode) {
    if (mDrawingState.sidebandStream == sidebandStream) return false;

    if (mDrawingState.sidebandStream != nullptr && sidebandStream == nullptr) {
        mFlinger->mTunnelModeEnabledReporter->decrementTunnelModeCount();
    } else if (sidebandStream != nullptr) {
        mFlinger->mTunnelModeEnabledReporter->incrementTunnelModeCount();
    }

    mDrawingState.sidebandStream = sidebandStream;
    mDrawingState.modified = true;
    if (sidebandStream != nullptr && mDrawingState.buffer != nullptr) {
        releasePreviousBuffer();
        resetDrawingStateBufferInfo();
        mDrawingState.bufferSurfaceFrameTX = nullptr;
        setFrameTimelineVsyncForBufferlessTransaction(info, postTime, gameMode);
    }
    setTransactionFlags(eTransactionNeeded);
    if (!mSidebandStreamChanged.exchange(true)) {
        // mSidebandStreamChanged was false
        mFlinger->onLayerUpdate();
    }
    return true;
}

bool Layer::setTransactionCompletedListeners(const std::vector<sp<CallbackHandle>>& handles,
                                             bool willPresent) {
    // If there is no handle, we will not send a callback so reset mReleasePreviousBuffer and return
    if (handles.empty()) {
        mReleasePreviousBuffer = false;
        return false;
    }

    std::deque<sp<CallbackHandle>> remainingHandles;
    for (const auto& handle : handles) {
        // If this transaction set a buffer on this layer, release its previous buffer
        handle->releasePreviousBuffer = mReleasePreviousBuffer;

        // If this layer will be presented in this frame
        if (willPresent) {
            // If this transaction set an acquire fence on this layer, set its acquire time
            handle->acquireTimeOrFence = mCallbackHandleAcquireTimeOrFence;
            handle->frameNumber = mDrawingState.frameNumber;
            handle->previousFrameNumber = mDrawingState.previousFrameNumber;
            if (FlagManager::getInstance().ce_fence_promise() &&
                mPreviousReleaseBufferEndpoint == handle->listener) {
                // Add fence from previous screenshot now so that it can be dispatched to the
                // client.
                for (auto& [_, future] : mAdditionalPreviousReleaseFences) {
                    handle->previousReleaseFences.emplace_back(std::move(future));
                }
                mAdditionalPreviousReleaseFences.clear();
            } else if (FlagManager::getInstance().screenshot_fence_preservation() &&
                       mPreviousReleaseBufferEndpoint == handle->listener) {
                // Add fences from previous screenshots now so that they can be dispatched to the
                // client.
                for (const auto& futureAndContinution : mPreviousReleaseFenceAndContinuations) {
                    handle->previousSharedReleaseFences.emplace_back(futureAndContinution.chain());
                }
                mPreviousReleaseFenceAndContinuations.clear();
            }
            // Store so latched time and release fence can be set
            mDrawingState.callbackHandles.push_back(handle);

        } else { // If this layer will NOT need to be relatched and presented this frame
            // Queue this handle to be notified below.
            remainingHandles.push_back(handle);
        }
    }

    if (!remainingHandles.empty()) {
        // Notify the transaction completed threads these handles are done. These are only the
        // handles that were not added to the mDrawingState, which will be notified later.
        mFlinger->getTransactionCallbackInvoker().addCallbackHandles(remainingHandles);
    }

    mReleasePreviousBuffer = false;
    mCallbackHandleAcquireTimeOrFence = -1;

    return willPresent;
}

Rect Layer::getBufferSize(const State& /*s*/) const {
    // for buffer state layers we use the display frame size as the buffer size.

    if (mBufferInfo.mBuffer == nullptr) {
        return Rect::INVALID_RECT;
    }

    uint32_t bufWidth = mBufferInfo.mBuffer->getWidth();
    uint32_t bufHeight = mBufferInfo.mBuffer->getHeight();

    // Undo any transformations on the buffer and return the result.
    if (mBufferInfo.mTransform & ui::Transform::ROT_90) {
        std::swap(bufWidth, bufHeight);
    }

    if (getTransformToDisplayInverse()) {
        uint32_t invTransform = SurfaceFlinger::getActiveDisplayRotationFlags();
        if (invTransform & ui::Transform::ROT_90) {
            std::swap(bufWidth, bufHeight);
        }
    }

    return Rect(0, 0, static_cast<int32_t>(bufWidth), static_cast<int32_t>(bufHeight));
}

FloatRect Layer::computeSourceBounds(const FloatRect& parentBounds) const {
    if (mBufferInfo.mBuffer == nullptr) {
        return parentBounds;
    }

    return getBufferSize(getDrawingState()).toFloatRect();
}

bool Layer::fenceHasSignaled() const {
    if (SurfaceFlinger::enableLatchUnsignaledConfig != LatchUnsignaledConfig::Disabled) {
        return true;
    }

    const bool fenceSignaled =
            getDrawingState().acquireFence->getStatus() == Fence::Status::Signaled;
    if (!fenceSignaled) {
        mFlinger->mTimeStats->incrementLatchSkipped(getSequence(),
                                                    TimeStats::LatchSkipReason::LateAcquire);
    }

    return fenceSignaled;
}

void Layer::onPreComposition(nsecs_t refreshStartTime) {
    for (const auto& handle : mDrawingState.callbackHandles) {
        handle->refreshStartTime = refreshStartTime;
    }
}

void Layer::setAutoRefresh(bool autoRefresh) {
    mDrawingState.autoRefresh = autoRefresh;
}

bool Layer::latchSidebandStream(bool& recomputeVisibleRegions) {
    // We need to update the sideband stream if the layer has both a buffer and a sideband stream.
    auto* snapshot = editLayerSnapshot();
    snapshot->sidebandStreamHasFrame = hasFrameUpdate() && mSidebandStream.get();

    if (mSidebandStreamChanged.exchange(false)) {
        const State& s(getDrawingState());
        // mSidebandStreamChanged was true
        mSidebandStream = s.sidebandStream;
        snapshot->sidebandStream = mSidebandStream;
        if (mSidebandStream != nullptr) {
            setTransactionFlags(eTransactionNeeded);
            mFlinger->setTransactionFlags(eTraversalNeeded);
        }
        recomputeVisibleRegions = true;

        return true;
    }
    return false;
}

bool Layer::hasFrameUpdate() const {
    const State& c(getDrawingState());
    return (mDrawingStateModified || mDrawingState.modified) &&
            (c.buffer != nullptr || c.bgColorLayer != nullptr);
}

void Layer::updateTexImage(nsecs_t latchTime, bool bgColorOnly) {
    const State& s(getDrawingState());

    if (!s.buffer) {
        if (bgColorOnly || mBufferInfo.mBuffer) {
            for (auto& handle : mDrawingState.callbackHandles) {
                handle->latchTime = latchTime;
            }
        }
        return;
    }

    for (auto& handle : mDrawingState.callbackHandles) {
        if (handle->frameNumber == mDrawingState.frameNumber) {
            handle->latchTime = latchTime;
        }
    }

    const int32_t layerId = getSequence();
    const uint64_t bufferId = mDrawingState.buffer->getId();
    const uint64_t frameNumber = mDrawingState.frameNumber;
    const auto acquireFence = std::make_shared<FenceTime>(mDrawingState.acquireFence);
    mFlinger->mTimeStats->setAcquireFence(layerId, frameNumber, acquireFence);
    mFlinger->mTimeStats->setLatchTime(layerId, frameNumber, latchTime);

    mFlinger->mFrameTracer->traceFence(layerId, bufferId, frameNumber, acquireFence,
                                       FrameTracer::FrameEvent::ACQUIRE_FENCE);
    mFlinger->mFrameTracer->traceTimestamp(layerId, bufferId, frameNumber, latchTime,
                                           FrameTracer::FrameEvent::LATCH);

    auto& bufferSurfaceFrame = mDrawingState.bufferSurfaceFrameTX;
    if (bufferSurfaceFrame != nullptr &&
        bufferSurfaceFrame->getPresentState() != PresentState::Presented) {
        // Update only if the bufferSurfaceFrame wasn't already presented. A Presented
        // bufferSurfaceFrame could be seen here if a pending state was applied successfully and we
        // are processing the next state.
        addSurfaceFramePresentedForBuffer(bufferSurfaceFrame,
                                          mDrawingState.acquireFenceTime->getSignalTime(),
                                          latchTime);
        mDrawingState.bufferSurfaceFrameTX.reset();
    }

    std::deque<sp<CallbackHandle>> remainingHandles;
    mFlinger->getTransactionCallbackInvoker()
            .addOnCommitCallbackHandles(mDrawingState.callbackHandles, remainingHandles);
    mDrawingState.callbackHandles = remainingHandles;

    mDrawingStateModified = false;
}

void Layer::gatherBufferInfo() {
    mPreviousReleaseCallbackId = {getCurrentBufferId(), mBufferInfo.mFrameNumber};
    mPreviousReleaseBufferEndpoint = mBufferInfo.mReleaseBufferEndpoint;
    if (!mDrawingState.buffer) {
        mBufferInfo = {};
        return;
    }

    if ((!mBufferInfo.mBuffer || !mDrawingState.buffer->hasSameBuffer(*mBufferInfo.mBuffer))) {
        decrementPendingBufferCount();
    }

    mBufferInfo.mBuffer = mDrawingState.buffer;
    mBufferInfo.mReleaseBufferEndpoint = mDrawingState.releaseBufferEndpoint;
    mBufferInfo.mFence = mDrawingState.acquireFence;
    mBufferInfo.mFrameNumber = mDrawingState.frameNumber;
    mBufferInfo.mPixelFormat =
            !mBufferInfo.mBuffer ? PIXEL_FORMAT_NONE : mBufferInfo.mBuffer->getPixelFormat();
    mBufferInfo.mFrameLatencyNeeded = true;
    mBufferInfo.mDesiredPresentTime = mDrawingState.desiredPresentTime;
    mBufferInfo.mFenceTime = std::make_shared<FenceTime>(mDrawingState.acquireFence);
    mBufferInfo.mFence = mDrawingState.acquireFence;
    mBufferInfo.mTransform = mDrawingState.bufferTransform;
    auto lastDataspace = mBufferInfo.mDataspace;
    mBufferInfo.mDataspace = translateDataspace(mDrawingState.dataspace);
    if (mBufferInfo.mBuffer != nullptr) {
        auto& mapper = GraphicBufferMapper::get();
        // TODO: We should measure if it's faster to do a blind write if we're on newer api levels
        // and don't need to possibly remaps buffers.
        ui::Dataspace dataspace = ui::Dataspace::UNKNOWN;
        status_t err = OK;
        {
            SFTRACE_NAME("getDataspace");
            err = mapper.getDataspace(mBufferInfo.mBuffer->getBuffer()->handle, &dataspace);
        }
        if (err != OK || dataspace != mBufferInfo.mDataspace) {
            {
                SFTRACE_NAME("setDataspace");
                err = mapper.setDataspace(mBufferInfo.mBuffer->getBuffer()->handle,
                                          static_cast<ui::Dataspace>(mBufferInfo.mDataspace));
            }

            // Some GPU drivers may cache gralloc metadata which means before we composite we need
            // to upsert RenderEngine's caches. Put in a special workaround to be backwards
            // compatible with old vendors, with a ticking clock.
            static const int32_t kVendorVersion =
                    base::GetIntProperty("ro.board.api_level", __ANDROID_API_FUTURE__);
            if (const auto format =
                        static_cast<aidl::android::hardware::graphics::common::PixelFormat>(
                                mBufferInfo.mBuffer->getPixelFormat());
                err == OK && kVendorVersion < __ANDROID_API_U__ &&
                (format ==
                         aidl::android::hardware::graphics::common::PixelFormat::
                                 IMPLEMENTATION_DEFINED ||
                 format == aidl::android::hardware::graphics::common::PixelFormat::YCBCR_420_888 ||
                 format == aidl::android::hardware::graphics::common::PixelFormat::YV12 ||
                 format == aidl::android::hardware::graphics::common::PixelFormat::YCBCR_P010)) {
                mBufferInfo.mBuffer->remapBuffer();
            }
        }
    }
    if (lastDataspace != mBufferInfo.mDataspace) {
        mFlinger->mHdrLayerInfoChanged = true;
    }
    if (mBufferInfo.mDesiredHdrSdrRatio != mDrawingState.desiredHdrSdrRatio) {
        mBufferInfo.mDesiredHdrSdrRatio = mDrawingState.desiredHdrSdrRatio;
        mFlinger->mHdrLayerInfoChanged = true;
    }
    mBufferInfo.mCrop = computeBufferCrop(mDrawingState);
    mBufferInfo.mScaleMode = NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW;
    mBufferInfo.mSurfaceDamage = mDrawingState.surfaceDamageRegion;
    mBufferInfo.mHdrMetadata = mDrawingState.hdrMetadata;
    mBufferInfo.mApi = mDrawingState.api;
    mBufferInfo.mTransformToDisplayInverse = mDrawingState.transformToDisplayInverse;
}

Rect Layer::computeBufferCrop(const State& s) {
    if (s.buffer && !s.bufferCrop.isEmpty()) {
        Rect bufferCrop;
        s.buffer->getBounds().intersect(s.bufferCrop, &bufferCrop);
        return bufferCrop;
    } else if (s.buffer) {
        return s.buffer->getBounds();
    } else {
        return s.bufferCrop;
    }
}

void Layer::decrementPendingBufferCount() {
    int32_t pendingBuffers = --mPendingBufferTransactions;
    tracePendingBufferCount(pendingBuffers);
}

void Layer::tracePendingBufferCount(int32_t pendingBuffers) {
    SFTRACE_INT(mBlastTransactionName.c_str(), pendingBuffers);
}

/*
 * We don't want to send the layer's transform to input, but rather the
 * parent's transform. This is because Layer's transform is
 * information about how the buffer is placed on screen. The parent's
 * transform makes more sense to send since it's information about how the
 * layer is placed on screen. This transform is used by input to determine
 * how to go from screen space back to window space.
 */
ui::Transform Layer::getInputTransform() const {
    if (!hasBufferOrSidebandStream()) {
        return getTransform();
    }
    sp<Layer> parent = mDrawingParent.promote();
    if (parent == nullptr) {
        return ui::Transform();
    }

    return parent->getTransform();
}

/**
 * Returns the bounds used to fill the input frame and the touchable region.
 *
 * Similar to getInputTransform, we need to update the bounds to include the transform.
 * This is because bounds don't include the buffer transform, where the input assumes
 * that's already included.
 */
std::pair<FloatRect, bool> Layer::getInputBounds(bool fillParentBounds) const {
    Rect croppedBufferSize = getCroppedBufferSize(getDrawingState());
    FloatRect inputBounds = croppedBufferSize.toFloatRect();
    if (hasBufferOrSidebandStream() && croppedBufferSize.isValid() &&
        mDrawingState.transform.getType() != ui::Transform::IDENTITY) {
        inputBounds = mDrawingState.transform.transform(inputBounds);
    }

    bool inputBoundsValid = croppedBufferSize.isValid();
    if (!inputBoundsValid) {
        /**
         * Input bounds are based on the layer crop or buffer size. But if we are using
         * the layer bounds as the input bounds (replaceTouchableRegionWithCrop flag) then
         * we can use the parent bounds as the input bounds if the layer does not have buffer
         * or a crop. We want to unify this logic but because of compat reasons we cannot always
         * use the parent bounds. A layer without a buffer can get input. So when a window is
         * initially added, its touchable region can fill its parent layer bounds and that can
         * have negative consequences.
         */
        inputBounds = fillParentBounds ? mBounds : FloatRect{};
    }

    // Clamp surface inset to the input bounds.
    const float inset = static_cast<float>(mDrawingState.inputInfo.surfaceInset);
    const float xSurfaceInset = std::clamp(inset, 0.f, inputBounds.getWidth() / 2.f);
    const float ySurfaceInset = std::clamp(inset, 0.f, inputBounds.getHeight() / 2.f);

    // Apply the insets to the input bounds.
    inputBounds.left += xSurfaceInset;
    inputBounds.top += ySurfaceInset;
    inputBounds.right -= xSurfaceInset;
    inputBounds.bottom -= ySurfaceInset;

    return {inputBounds, inputBoundsValid};
}

bool Layer::isSimpleBufferUpdate(const layer_state_t& s) const {
    const uint64_t requiredFlags = layer_state_t::eBufferChanged;

    const uint64_t deniedFlags = layer_state_t::eProducerDisconnect | layer_state_t::eLayerChanged |
            layer_state_t::eRelativeLayerChanged | layer_state_t::eTransparentRegionChanged |
            layer_state_t::eFlagsChanged | layer_state_t::eBlurRegionsChanged |
            layer_state_t::eLayerStackChanged | layer_state_t::eReparent |
            (FlagManager::getInstance().latch_unsignaled_with_auto_refresh_changed()
                     ? 0
                     : layer_state_t::eAutoRefreshChanged);

    if ((s.what & requiredFlags) != requiredFlags) {
        SFTRACE_FORMAT_INSTANT("%s: false [missing required flags 0x%" PRIx64 "]", __func__,
                               (s.what | requiredFlags) & ~s.what);
        return false;
    }

    if (s.what & deniedFlags) {
        SFTRACE_FORMAT_INSTANT("%s: false [has denied flags 0x%" PRIx64 "]", __func__,
                               s.what & deniedFlags);
        return false;
    }

    if (s.what & layer_state_t::ePositionChanged) {
        if (mRequestedTransform.tx() != s.x || mRequestedTransform.ty() != s.y) {
            SFTRACE_FORMAT_INSTANT("%s: false [ePositionChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eAlphaChanged) {
        if (mDrawingState.color.a != s.color.a) {
            SFTRACE_FORMAT_INSTANT("%s: false [eAlphaChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eColorTransformChanged) {
        if (mDrawingState.colorTransform != s.colorTransform) {
            SFTRACE_FORMAT_INSTANT("%s: false [eColorTransformChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eBackgroundColorChanged) {
        if (mDrawingState.bgColorLayer || s.bgColor.a != 0) {
            SFTRACE_FORMAT_INSTANT("%s: false [eBackgroundColorChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eMatrixChanged) {
        if (mRequestedTransform.dsdx() != s.matrix.dsdx ||
            mRequestedTransform.dtdy() != s.matrix.dtdy ||
            mRequestedTransform.dtdx() != s.matrix.dtdx ||
            mRequestedTransform.dsdy() != s.matrix.dsdy) {
            SFTRACE_FORMAT_INSTANT("%s: false [eMatrixChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eCornerRadiusChanged) {
        if (mDrawingState.cornerRadius != s.cornerRadius) {
            SFTRACE_FORMAT_INSTANT("%s: false [eCornerRadiusChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eBackgroundBlurRadiusChanged) {
        if (mDrawingState.backgroundBlurRadius != static_cast<int>(s.backgroundBlurRadius)) {
            SFTRACE_FORMAT_INSTANT("%s: false [eBackgroundBlurRadiusChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eBufferTransformChanged) {
        if (mDrawingState.bufferTransform != s.bufferTransform) {
            SFTRACE_FORMAT_INSTANT("%s: false [eBufferTransformChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eTransformToDisplayInverseChanged) {
        if (mDrawingState.transformToDisplayInverse != s.transformToDisplayInverse) {
            SFTRACE_FORMAT_INSTANT("%s: false [eTransformToDisplayInverseChanged changed]",
                                   __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eCropChanged) {
        if (mDrawingState.crop != s.crop) {
            SFTRACE_FORMAT_INSTANT("%s: false [eCropChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eDataspaceChanged) {
        if (mDrawingState.dataspace != s.dataspace) {
            SFTRACE_FORMAT_INSTANT("%s: false [eDataspaceChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eHdrMetadataChanged) {
        if (mDrawingState.hdrMetadata != s.hdrMetadata) {
            SFTRACE_FORMAT_INSTANT("%s: false [eHdrMetadataChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eSidebandStreamChanged) {
        if (mDrawingState.sidebandStream != s.sidebandStream) {
            SFTRACE_FORMAT_INSTANT("%s: false [eSidebandStreamChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eColorSpaceAgnosticChanged) {
        if (mDrawingState.colorSpaceAgnostic != s.colorSpaceAgnostic) {
            SFTRACE_FORMAT_INSTANT("%s: false [eColorSpaceAgnosticChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eShadowRadiusChanged) {
        if (mDrawingState.shadowRadius != s.shadowRadius) {
            SFTRACE_FORMAT_INSTANT("%s: false [eShadowRadiusChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eFixedTransformHintChanged) {
        if (mDrawingState.fixedTransformHint != s.fixedTransformHint) {
            SFTRACE_FORMAT_INSTANT("%s: false [eFixedTransformHintChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eTrustedOverlayChanged) {
        if (mDrawingState.isTrustedOverlay != (s.trustedOverlay == gui::TrustedOverlay::ENABLED)) {
            SFTRACE_FORMAT_INSTANT("%s: false [eTrustedOverlayChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eStretchChanged) {
        StretchEffect temp = s.stretchEffect;
        temp.sanitize();
        if (mDrawingState.stretchEffect != temp) {
            SFTRACE_FORMAT_INSTANT("%s: false [eStretchChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eBufferCropChanged) {
        if (mDrawingState.bufferCrop != s.bufferCrop) {
            SFTRACE_FORMAT_INSTANT("%s: false [eBufferCropChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eDestinationFrameChanged) {
        if (mDrawingState.destinationFrame != s.destinationFrame) {
            SFTRACE_FORMAT_INSTANT("%s: false [eDestinationFrameChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eDimmingEnabledChanged) {
        if (mDrawingState.dimmingEnabled != s.dimmingEnabled) {
            SFTRACE_FORMAT_INSTANT("%s: false [eDimmingEnabledChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eExtendedRangeBrightnessChanged) {
        if (mDrawingState.currentHdrSdrRatio != s.currentHdrSdrRatio ||
            mDrawingState.desiredHdrSdrRatio != s.desiredHdrSdrRatio) {
            SFTRACE_FORMAT_INSTANT("%s: false [eExtendedRangeBrightnessChanged changed]", __func__);
            return false;
        }
    }

    if (s.what & layer_state_t::eDesiredHdrHeadroomChanged) {
        if (mDrawingState.desiredHdrSdrRatio != s.desiredHdrSdrRatio) {
            SFTRACE_FORMAT_INSTANT("%s: false [eDesiredHdrHeadroomChanged changed]", __func__);
            return false;
        }
    }

    return true;
}

sp<LayerFE> Layer::getCompositionEngineLayerFE() const {
    // There's no need to get a CE Layer if the layer isn't going to draw anything.
    return hasSomethingToDraw() ? mLegacyLayerFE : nullptr;
}

const LayerSnapshot* Layer::getLayerSnapshot() const {
    return mSnapshot.get();
}

LayerSnapshot* Layer::editLayerSnapshot() {
    return mSnapshot.get();
}

std::unique_ptr<frontend::LayerSnapshot> Layer::stealLayerSnapshot() {
    return std::move(mSnapshot);
}

void Layer::updateLayerSnapshot(std::unique_ptr<frontend::LayerSnapshot> snapshot) {
    mSnapshot = std::move(snapshot);
}

const compositionengine::LayerFECompositionState* Layer::getCompositionState() const {
    return mSnapshot.get();
}

sp<LayerFE> Layer::copyCompositionEngineLayerFE() const {
    auto result = mFlinger->getFactory().createLayerFE(mName, this);
    result->mSnapshot = std::make_unique<LayerSnapshot>(*mSnapshot);
    return result;
}

sp<LayerFE> Layer::getCompositionEngineLayerFE(
        const frontend::LayerHierarchy::TraversalPath& path) {
    for (auto& [p, layerFE] : mLayerFEs) {
        if (p == path) {
            return layerFE;
        }
    }
    auto layerFE = mFlinger->getFactory().createLayerFE(mName, this);
    mLayerFEs.emplace_back(path, layerFE);
    return layerFE;
}

void Layer::useSurfaceDamage() {
    if (mFlinger->mForceFullDamage) {
        surfaceDamageRegion = Region::INVALID_REGION;
    } else {
        surfaceDamageRegion = mBufferInfo.mSurfaceDamage;
    }
}

void Layer::useEmptyDamage() {
    surfaceDamageRegion.clear();
}

bool Layer::isOpaque(const Layer::State& s) const {
    // if we don't have a buffer or sidebandStream yet, we're translucent regardless of the
    // layer's opaque flag.
    if (!hasSomethingToDraw()) {
        return false;
    }

    // if the layer has the opaque flag, then we're always opaque
    if ((s.flags & layer_state_t::eLayerOpaque) == layer_state_t::eLayerOpaque) {
        return true;
    }

    // If the buffer has no alpha channel, then we are opaque
    if (hasBufferOrSidebandStream() && LayerSnapshot::isOpaqueFormat(getPixelFormat())) {
        return true;
    }

    // Lastly consider the layer opaque if drawing a color with alpha == 1.0
    return fillsColor() && getAlpha() == 1.0_hf;
}

bool Layer::canReceiveInput() const {
    return !isHiddenByPolicy() && (mBufferInfo.mBuffer == nullptr || getAlpha() > 0.0f);
}

bool Layer::isVisible() const {
    if (!hasSomethingToDraw()) {
        return false;
    }

    if (isHiddenByPolicy()) {
        return false;
    }

    return getAlpha() > 0.0f || hasBlur();
}

void Layer::onCompositionPresented(const DisplayDevice* display,
                                   const std::shared_ptr<FenceTime>& glDoneFence,
                                   const std::shared_ptr<FenceTime>& presentFence,
                                   const CompositorTiming& compositorTiming,
                                   gui::GameMode gameMode) {
    // mFrameLatencyNeeded is true when a new frame was latched for the
    // composition.
    if (!mBufferInfo.mFrameLatencyNeeded) return;

    for (const auto& handle : mDrawingState.callbackHandles) {
        handle->gpuCompositionDoneFence = glDoneFence;
        handle->compositorTiming = compositorTiming;
    }

    // Update mFrameTracker.
    nsecs_t desiredPresentTime = mBufferInfo.mDesiredPresentTime;
    mFrameTracker.setDesiredPresentTime(desiredPresentTime);

    const int32_t layerId = getSequence();
    mFlinger->mTimeStats->setDesiredTime(layerId, mCurrentFrameNumber, desiredPresentTime);

    const auto outputLayer = findOutputLayerForDisplay(display);
    if (outputLayer && outputLayer->requiresClientComposition()) {
        nsecs_t clientCompositionTimestamp = outputLayer->getState().clientCompositionTimestamp;
        mFlinger->mFrameTracer->traceTimestamp(layerId, getCurrentBufferId(), mCurrentFrameNumber,
                                               clientCompositionTimestamp,
                                               FrameTracer::FrameEvent::FALLBACK_COMPOSITION);
        // Update the SurfaceFrames in the drawing state
        if (mDrawingState.bufferSurfaceFrameTX) {
            mDrawingState.bufferSurfaceFrameTX->setGpuComposition();
        }
        for (auto& [token, surfaceFrame] : mDrawingState.bufferlessSurfaceFramesTX) {
            surfaceFrame->setGpuComposition();
        }
    }

    std::shared_ptr<FenceTime> frameReadyFence = mBufferInfo.mFenceTime;
    if (frameReadyFence->isValid()) {
        mFrameTracker.setFrameReadyFence(std::move(frameReadyFence));
    } else {
        // There was no fence for this frame, so assume that it was ready
        // to be presented at the desired present time.
        mFrameTracker.setFrameReadyTime(desiredPresentTime);
    }

    if (display) {
        const auto activeMode = display->refreshRateSelector().getActiveMode();
        const Fps refreshRate = activeMode.fps;
        const std::optional<Fps> renderRate =
                mFlinger->mScheduler->getFrameRateOverride(getOwnerUid());

        const auto vote = frameRateToSetFrameRateVotePayload(getFrameRateForLayerTree());

        if (presentFence->isValid()) {
            mFlinger->mTimeStats->setPresentFence(layerId, mCurrentFrameNumber, presentFence,
                                                  refreshRate, renderRate, vote, gameMode);
            mFlinger->mFrameTracer->traceFence(layerId, getCurrentBufferId(), mCurrentFrameNumber,
                                               presentFence,
                                               FrameTracer::FrameEvent::PRESENT_FENCE);
            mFrameTracker.setActualPresentFence(std::shared_ptr<FenceTime>(presentFence));
        } else if (const auto displayId = PhysicalDisplayId::tryCast(display->getId());
                   displayId && mFlinger->getHwComposer().isConnected(*displayId)) {
            // The HWC doesn't support present fences, so use the present timestamp instead.
            const nsecs_t presentTimestamp =
                    mFlinger->getHwComposer().getPresentTimestamp(*displayId);

            const nsecs_t now = systemTime(CLOCK_MONOTONIC);
            const nsecs_t vsyncPeriod =
                    mFlinger->getHwComposer()
                            .getDisplayVsyncPeriod(*displayId)
                            .value_opt()
                            .value_or(activeMode.modePtr->getVsyncRate().getPeriodNsecs());

            const nsecs_t actualPresentTime = now - ((now - presentTimestamp) % vsyncPeriod);

            mFlinger->mTimeStats->setPresentTime(layerId, mCurrentFrameNumber, actualPresentTime,
                                                 refreshRate, renderRate, vote, gameMode);
            mFlinger->mFrameTracer->traceTimestamp(layerId, getCurrentBufferId(),
                                                   mCurrentFrameNumber, actualPresentTime,
                                                   FrameTracer::FrameEvent::PRESENT_FENCE);
            mFrameTracker.setActualPresentTime(actualPresentTime);
        }
    }

    mFrameTracker.advanceFrame();
    mBufferInfo.mFrameLatencyNeeded = false;
}

bool Layer::willReleaseBufferOnLatch() const {
    return !mDrawingState.buffer && mBufferInfo.mBuffer;
}

bool Layer::latchBuffer(bool& recomputeVisibleRegions, nsecs_t latchTime) {
    const bool bgColorOnly = mDrawingState.bgColorLayer != nullptr;
    return latchBufferImpl(recomputeVisibleRegions, latchTime, bgColorOnly);
}

bool Layer::latchBufferImpl(bool& recomputeVisibleRegions, nsecs_t latchTime, bool bgColorOnly) {
    SFTRACE_FORMAT_INSTANT("latchBuffer %s - %" PRIu64, getDebugName(),
                           getDrawingState().frameNumber);

    bool refreshRequired = latchSidebandStream(recomputeVisibleRegions);

    if (refreshRequired) {
        return refreshRequired;
    }

    // If the head buffer's acquire fence hasn't signaled yet, return and
    // try again later
    if (!fenceHasSignaled()) {
        SFTRACE_NAME("!fenceHasSignaled()");
        mFlinger->onLayerUpdate();
        return false;
    }
    updateTexImage(latchTime, bgColorOnly);

    // Capture the old state of the layer for comparisons later
    BufferInfo oldBufferInfo = mBufferInfo;
    const bool oldOpacity = isOpaque(mDrawingState);
    mPreviousFrameNumber = mCurrentFrameNumber;
    mCurrentFrameNumber = mDrawingState.frameNumber;
    gatherBufferInfo();

    if (mBufferInfo.mBuffer) {
        // We latched a buffer that will be presented soon. Clear the previously presented layer
        // stack list.
        mPreviouslyPresentedLayerStacks.clear();
    }

    if (mDrawingState.buffer == nullptr) {
        const bool bufferReleased = oldBufferInfo.mBuffer != nullptr;
        recomputeVisibleRegions = bufferReleased;
        return bufferReleased;
    }

    if (oldBufferInfo.mBuffer == nullptr) {
        // the first time we receive a buffer, we need to trigger a
        // geometry invalidation.
        recomputeVisibleRegions = true;
    }

    if ((mBufferInfo.mCrop != oldBufferInfo.mCrop) ||
        (mBufferInfo.mTransform != oldBufferInfo.mTransform) ||
        (mBufferInfo.mScaleMode != oldBufferInfo.mScaleMode) ||
        (mBufferInfo.mTransformToDisplayInverse != oldBufferInfo.mTransformToDisplayInverse)) {
        recomputeVisibleRegions = true;
    }

    if (oldBufferInfo.mBuffer != nullptr) {
        uint32_t bufWidth = mBufferInfo.mBuffer->getWidth();
        uint32_t bufHeight = mBufferInfo.mBuffer->getHeight();
        if (bufWidth != oldBufferInfo.mBuffer->getWidth() ||
            bufHeight != oldBufferInfo.mBuffer->getHeight()) {
            recomputeVisibleRegions = true;
        }
    }

    if (oldOpacity != isOpaque(mDrawingState)) {
        recomputeVisibleRegions = true;
    }

    return true;
}

bool Layer::hasReadyFrame() const {
    return hasFrameUpdate() || getSidebandStreamChanged() || getAutoRefresh();
}

bool Layer::isProtected() const {
    return (mBufferInfo.mBuffer != nullptr) &&
            (mBufferInfo.mBuffer->getUsage() & GRALLOC_USAGE_PROTECTED);
}

void Layer::latchAndReleaseBuffer() {
    if (hasReadyFrame()) {
        bool ignored = false;
        latchBuffer(ignored, systemTime());
    }
    releasePendingBuffer(systemTime());
}

PixelFormat Layer::getPixelFormat() const {
    return mBufferInfo.mPixelFormat;
}

bool Layer::getTransformToDisplayInverse() const {
    return mBufferInfo.mTransformToDisplayInverse;
}

Rect Layer::getBufferCrop() const {
    // this is the crop rectangle that applies to the buffer
    // itself (as opposed to the window)
    if (!mBufferInfo.mCrop.isEmpty()) {
        // if the buffer crop is defined, we use that
        return mBufferInfo.mCrop;
    } else if (mBufferInfo.mBuffer != nullptr) {
        // otherwise we use the whole buffer
        return mBufferInfo.mBuffer->getBounds();
    } else {
        // if we don't have a buffer yet, we use an empty/invalid crop
        return Rect();
    }
}

uint32_t Layer::getBufferTransform() const {
    return mBufferInfo.mTransform;
}

ui::Dataspace Layer::getDataSpace() const {
    return hasBufferOrSidebandStream() ? mBufferInfo.mDataspace : mDrawingState.dataspace;
}

bool Layer::isFrontBuffered() const {
    if (mBufferInfo.mBuffer == nullptr) {
        return false;
    }

    return mBufferInfo.mBuffer->getUsage() & AHARDWAREBUFFER_USAGE_FRONT_BUFFER;
}

ui::Dataspace Layer::translateDataspace(ui::Dataspace dataspace) {
    ui::Dataspace updatedDataspace = dataspace;
    // translate legacy dataspaces to modern dataspaces
    switch (dataspace) {
        // Treat unknown dataspaces as V0_sRGB
        case ui::Dataspace::UNKNOWN:
        case ui::Dataspace::SRGB:
            updatedDataspace = ui::Dataspace::V0_SRGB;
            break;
        case ui::Dataspace::SRGB_LINEAR:
            updatedDataspace = ui::Dataspace::V0_SRGB_LINEAR;
            break;
        case ui::Dataspace::JFIF:
            updatedDataspace = ui::Dataspace::V0_JFIF;
            break;
        case ui::Dataspace::BT601_625:
            updatedDataspace = ui::Dataspace::V0_BT601_625;
            break;
        case ui::Dataspace::BT601_525:
            updatedDataspace = ui::Dataspace::V0_BT601_525;
            break;
        case ui::Dataspace::BT709:
            updatedDataspace = ui::Dataspace::V0_BT709;
            break;
        default:
            break;
    }

    return updatedDataspace;
}

sp<GraphicBuffer> Layer::getBuffer() const {
    return mBufferInfo.mBuffer ? mBufferInfo.mBuffer->getBuffer() : nullptr;
}

const std::shared_ptr<renderengine::ExternalTexture>& Layer::getExternalTexture() const {
    return mBufferInfo.mBuffer;
}

bool Layer::setColor(const half3& color) {
    if (mDrawingState.color.rgb == color) {
        return false;
    }

    mDrawingState.sequence++;
    mDrawingState.color.rgb = color;
    mDrawingState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::fillsColor() const {
    return !hasBufferOrSidebandStream() && mDrawingState.color.r >= 0.0_hf &&
            mDrawingState.color.g >= 0.0_hf && mDrawingState.color.b >= 0.0_hf;
}

bool Layer::hasBlur() const {
    return getBackgroundBlurRadius() > 0 || getDrawingState().blurRegions.size() > 0;
}

void Layer::updateSnapshot(bool updateGeometry) {
    if (!getCompositionEngineLayerFE()) {
        return;
    }

    auto* snapshot = editLayerSnapshot();
    if (updateGeometry) {
        prepareBasicGeometryCompositionState();
        prepareGeometryCompositionState();
        snapshot->roundedCorner = getRoundedCornerState();
        snapshot->transformedBounds = mScreenBounds;
        if (mEffectiveShadowRadius > 0.f) {
            snapshot->shadowSettings = mFlinger->mDrawingState.globalShadowSettings;

            // Note: this preserves existing behavior of shadowing the entire layer and not cropping
            // it if transparent regions are present. This may not be necessary since shadows are
            // typically cast by layers without transparent regions.
            snapshot->shadowSettings.boundaries = mBounds;

            const float casterAlpha = snapshot->alpha;
            const bool casterIsOpaque =
                    ((mBufferInfo.mBuffer != nullptr) && isOpaque(mDrawingState));

            // If the casting layer is translucent, we need to fill in the shadow underneath the
            // layer. Otherwise the generated shadow will only be shown around the casting layer.
            snapshot->shadowSettings.casterIsTranslucent = !casterIsOpaque || (casterAlpha < 1.0f);
            snapshot->shadowSettings.ambientColor *= casterAlpha;
            snapshot->shadowSettings.spotColor *= casterAlpha;
        }
        snapshot->shadowSettings.length = mEffectiveShadowRadius;
    }
    snapshot->contentOpaque = isOpaque(mDrawingState);
    snapshot->layerOpaqueFlagSet =
            (mDrawingState.flags & layer_state_t::eLayerOpaque) == layer_state_t::eLayerOpaque;
    sp<Layer> p = mDrawingParent.promote();
    if (p != nullptr) {
        snapshot->parentTransform = p->getTransform();
    } else {
        snapshot->parentTransform.reset();
    }
    snapshot->bufferSize = getBufferSize(mDrawingState);
    snapshot->externalTexture = mBufferInfo.mBuffer;
    snapshot->hasReadyFrame = hasReadyFrame();
    preparePerFrameCompositionState();
}

void Layer::updateChildrenSnapshots(bool updateGeometry) {
    for (const sp<Layer>& child : mDrawingChildren) {
        child->updateSnapshot(updateGeometry);
        child->updateChildrenSnapshots(updateGeometry);
    }
}

bool Layer::setTrustedPresentationInfo(TrustedPresentationThresholds const& thresholds,
                                       TrustedPresentationListener const& listener) {
    bool hadTrustedPresentationListener = hasTrustedPresentationListener();
    mTrustedPresentationListener = listener;
    mTrustedPresentationThresholds = thresholds;
    bool haveTrustedPresentationListener = hasTrustedPresentationListener();
    if (!hadTrustedPresentationListener && haveTrustedPresentationListener) {
        mFlinger->mNumTrustedPresentationListeners++;
    } else if (hadTrustedPresentationListener && !haveTrustedPresentationListener) {
        mFlinger->mNumTrustedPresentationListeners--;
    }

    // Reset trusted presentation states to ensure we start the time again.
    mEnteredTrustedPresentationStateTime = -1;
    mLastReportedTrustedPresentationState = false;
    mLastComputedTrustedPresentationState = false;

    // If there's a new trusted presentation listener, the code needs to go through the composite
    // path to ensure it recomutes the current state and invokes the TrustedPresentationListener if
    // we're already in the requested state.
    return haveTrustedPresentationListener;
}

void Layer::updateLastLatchTime(nsecs_t latchTime) {
    mLastLatchTime = latchTime;
}

void Layer::setIsSmallDirty(const Region& damageRegion,
                            const ui::Transform& layerToDisplayTransform) {
    mSmallDirty = false;
    if (!mFlinger->mScheduler->supportSmallDirtyDetection(mOwnerAppId)) {
        return;
    }

    if (mWindowType != WindowInfo::Type::APPLICATION &&
        mWindowType != WindowInfo::Type::BASE_APPLICATION) {
        return;
    }

    Rect bounds = damageRegion.getBounds();
    if (!bounds.isValid()) {
        return;
    }

    // Transform to screen space.
    bounds = layerToDisplayTransform.transform(bounds);

    // If the damage region is a small dirty, this could give the hint for the layer history that
    // it could suppress the heuristic rate when calculating.
    mSmallDirty = mFlinger->mScheduler->isSmallDirtyArea(mOwnerAppId,
                                                         bounds.getWidth() * bounds.getHeight());
}

void Layer::setIsSmallDirty(frontend::LayerSnapshot* snapshot) {
    setIsSmallDirty(snapshot->surfaceDamage, snapshot->localTransform);
    snapshot->isSmallDirty = mSmallDirty;
}

} // namespace android

#if defined(__gl_h_)
#error "don't include gl/gl.h in this file"
#endif

#if defined(__gl2_h_)
#error "don't include gl2/gl2.h in this file"
#endif

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion"
