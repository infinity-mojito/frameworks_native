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

// #define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#undef LOG_TAG
#define LOG_TAG "LayerSnapshotBuilder"

#include "LayerSnapshotBuilder.h"
#include <gui/TraceUtils.h>
#include <numeric>
#include "DisplayHardware/HWC2.h"
#include "DisplayHardware/Hal.h"
#include "LayerLog.h"
#include "TimeStats/TimeStats.h"
#include "ftl/small_map.h"

namespace android::surfaceflinger::frontend {

using namespace ftl::flag_operators;

namespace {
FloatRect getMaxDisplayBounds(
        const display::DisplayMap<ui::LayerStack, frontend::DisplayInfo>& displays) {
    const ui::Size maxSize = [&displays] {
        if (displays.empty()) return ui::Size{5000, 5000};

        return std::accumulate(displays.begin(), displays.end(), ui::kEmptySize,
                               [](ui::Size size, const auto& pair) -> ui::Size {
                                   const auto& display = pair.second;
                                   return {std::max(size.getWidth(), display.info.logicalWidth),
                                           std::max(size.getHeight(), display.info.logicalHeight)};
                               });
    }();

    // Ignore display bounds for now since they will be computed later. Use a large Rect bound
    // to ensure it's bigger than an actual display will be.
    const float xMax = static_cast<float>(maxSize.getWidth()) * 10.f;
    const float yMax = static_cast<float>(maxSize.getHeight()) * 10.f;

    return {-xMax, -yMax, xMax, yMax};
}

// Applies the given transform to the region, while protecting against overflows caused by any
// offsets. If applying the offset in the transform to any of the Rects in the region would result
// in an overflow, they are not added to the output Region.
Region transformTouchableRegionSafely(const ui::Transform& t, const Region& r,
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

/*
 * We don't want to send the layer's transform to input, but rather the
 * parent's transform. This is because Layer's transform is
 * information about how the buffer is placed on screen. The parent's
 * transform makes more sense to send since it's information about how the
 * layer is placed on screen. This transform is used by input to determine
 * how to go from screen space back to window space.
 */
ui::Transform getInputTransform(const LayerSnapshot& snapshot) {
    if (!snapshot.hasBufferOrSidebandStream()) {
        return snapshot.geomLayerTransform;
    }
    return snapshot.parentTransform;
}

/**
 * Similar to getInputTransform, we need to update the bounds to include the transform.
 * This is because bounds don't include the buffer transform, where the input assumes
 * that's already included.
 */
Rect getInputBounds(const LayerSnapshot& snapshot) {
    if (!snapshot.hasBufferOrSidebandStream()) {
        return snapshot.croppedBufferSize;
    }

    if (snapshot.localTransform.getType() == ui::Transform::IDENTITY ||
        !snapshot.croppedBufferSize.isValid()) {
        return snapshot.croppedBufferSize;
    }
    return snapshot.localTransform.transform(snapshot.croppedBufferSize);
}

void fillInputFrameInfo(gui::WindowInfo& info, const ui::Transform& screenToDisplay,
                        const LayerSnapshot& snapshot) {
    Rect tmpBounds = getInputBounds(snapshot);
    if (!tmpBounds.isValid()) {
        info.touchableRegion.clear();
        // A layer could have invalid input bounds and still expect to receive touch input if it has
        // replaceTouchableRegionWithCrop. For that case, the input transform needs to be calculated
        // correctly to determine the coordinate space for input events. Use an empty rect so that
        // the layer will receive input in its own layer space.
        tmpBounds = Rect::EMPTY_RECT;
    }

    // InputDispatcher works in the display device's coordinate space. Here, we calculate the
    // frame and transform used for the layer, which determines the bounds and the coordinate space
    // within which the layer will receive input.
    //
    // The coordinate space within which each of the bounds are specified is explicitly documented
    // in the variable name. For example "inputBoundsInLayer" is specified in layer space. A
    // Transform converts one coordinate space to another, which is apparent in its naming. For
    // example, "layerToDisplay" transforms layer space to display space.
    //
    // Coordinate space definitions:
    //   - display: The display device's coordinate space. Correlates to pixels on the display.
    //   - screen: The post-rotation coordinate space for the display, a.k.a. logical display space.
    //   - layer: The coordinate space of this layer.
    //   - input: The coordinate space in which this layer will receive input events. This could be
    //            different than layer space if a surfaceInset is used, which changes the origin
    //            of the input space.
    const FloatRect inputBoundsInLayer = tmpBounds.toFloatRect();

    // Clamp surface inset to the input bounds.
    const auto surfaceInset = static_cast<float>(info.surfaceInset);
    const float xSurfaceInset =
            std::max(0.f, std::min(surfaceInset, inputBoundsInLayer.getWidth() / 2.f));
    const float ySurfaceInset =
            std::max(0.f, std::min(surfaceInset, inputBoundsInLayer.getHeight() / 2.f));

    // Apply the insets to the input bounds.
    const FloatRect insetBoundsInLayer(inputBoundsInLayer.left + xSurfaceInset,
                                       inputBoundsInLayer.top + ySurfaceInset,
                                       inputBoundsInLayer.right - xSurfaceInset,
                                       inputBoundsInLayer.bottom - ySurfaceInset);

    // Crop the input bounds to ensure it is within the parent's bounds.
    const FloatRect croppedInsetBoundsInLayer =
            snapshot.geomLayerBounds.intersect(insetBoundsInLayer);

    const ui::Transform layerToScreen = getInputTransform(snapshot);
    const ui::Transform layerToDisplay = screenToDisplay * layerToScreen;

    const Rect roundedFrameInDisplay{layerToDisplay.transform(croppedInsetBoundsInLayer)};
    info.frameLeft = roundedFrameInDisplay.left;
    info.frameTop = roundedFrameInDisplay.top;
    info.frameRight = roundedFrameInDisplay.right;
    info.frameBottom = roundedFrameInDisplay.bottom;

    ui::Transform inputToLayer;
    inputToLayer.set(insetBoundsInLayer.left, insetBoundsInLayer.top);
    const ui::Transform inputToDisplay = layerToDisplay * inputToLayer;

    // InputDispatcher expects a display-to-input transform.
    info.transform = inputToDisplay.inverse();

    // The touchable region is specified in the input coordinate space. Change it to display space.
    info.touchableRegion =
            transformTouchableRegionSafely(inputToDisplay, info.touchableRegion, snapshot.name);
}

void handleDropInputMode(LayerSnapshot& snapshot, const LayerSnapshot& parentSnapshot) {
    if (snapshot.inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL)) {
        return;
    }

    // Check if we need to drop input unconditionally
    const gui::DropInputMode dropInputMode = snapshot.dropInputMode;
    if (dropInputMode == gui::DropInputMode::ALL) {
        snapshot.inputInfo.inputConfig |= gui::WindowInfo::InputConfig::DROP_INPUT;
        ALOGV("Dropping input for %s as requested by policy.", snapshot.name.c_str());
        return;
    }

    // Check if we need to check if the window is obscured by parent
    if (dropInputMode != gui::DropInputMode::OBSCURED) {
        return;
    }

    // Check if the parent has set an alpha on the layer
    if (parentSnapshot.color.a != 1.0_hf) {
        snapshot.inputInfo.inputConfig |= gui::WindowInfo::InputConfig::DROP_INPUT;
        ALOGV("Dropping input for %s as requested by policy because alpha=%f",
              snapshot.name.c_str(), static_cast<float>(parentSnapshot.color.a));
    }

    // Check if the parent has cropped the buffer
    Rect bufferSize = snapshot.croppedBufferSize;
    if (!bufferSize.isValid()) {
        snapshot.inputInfo.inputConfig |= gui::WindowInfo::InputConfig::DROP_INPUT_IF_OBSCURED;
        return;
    }

    // Screenbounds are the layer bounds cropped by parents, transformed to screenspace.
    // To check if the layer has been cropped, we take the buffer bounds, apply the local
    // layer crop and apply the same set of transforms to move to screenspace. If the bounds
    // match then the layer has not been cropped by its parents.
    Rect bufferInScreenSpace(snapshot.geomLayerTransform.transform(bufferSize));
    bool croppedByParent = bufferInScreenSpace != Rect{snapshot.transformedBounds};

    if (croppedByParent) {
        snapshot.inputInfo.inputConfig |= gui::WindowInfo::InputConfig::DROP_INPUT;
        ALOGV("Dropping input for %s as requested by policy because buffer is cropped by parent",
              snapshot.name.c_str());
    } else {
        // If the layer is not obscured by its parents (by setting an alpha or crop), then only drop
        // input if the window is obscured. This check should be done in surfaceflinger but the
        // logic currently resides in inputflinger. So pass the if_obscured check to input to only
        // drop input events if the window is obscured.
        snapshot.inputInfo.inputConfig |= gui::WindowInfo::InputConfig::DROP_INPUT_IF_OBSCURED;
    }
}

auto getBlendMode(const LayerSnapshot& snapshot, const RequestedLayerState& requested) {
    auto blendMode = Hwc2::IComposerClient::BlendMode::NONE;
    if (snapshot.alpha != 1.0f || !snapshot.isContentOpaque()) {
        blendMode = requested.premultipliedAlpha ? Hwc2::IComposerClient::BlendMode::PREMULTIPLIED
                                                 : Hwc2::IComposerClient::BlendMode::COVERAGE;
    }
    return blendMode;
}

void updateSurfaceDamage(const RequestedLayerState& requested, bool hasReadyFrame,
                         bool forceFullDamage, Region& outSurfaceDamageRegion) {
    if (!hasReadyFrame) {
        outSurfaceDamageRegion.clear();
        return;
    }
    if (forceFullDamage) {
        outSurfaceDamageRegion = Region::INVALID_REGION;
    } else {
        outSurfaceDamageRegion = requested.surfaceDamageRegion;
    }
}

void updateVisibility(LayerSnapshot& snapshot, bool visible) {
    snapshot.isVisible = visible;

    // TODO(b/238781169) we are ignoring this compat for now, since we will have
    // to remove any optimization based on visibility.

    // For compatibility reasons we let layers which can receive input
    // receive input before they have actually submitted a buffer. Because
    // of this we use canReceiveInput instead of isVisible to check the
    // policy-visibility, ignoring the buffer state. However for layers with
    // hasInputInfo()==false we can use the real visibility state.
    // We are just using these layers for occlusion detection in
    // InputDispatcher, and obviously if they aren't visible they can't occlude
    // anything.
    const bool visibleForInput =
            (snapshot.inputInfo.token != nullptr) ? snapshot.canReceiveInput() : snapshot.isVisible;
    snapshot.inputInfo.setInputConfig(gui::WindowInfo::InputConfig::NOT_VISIBLE, !visibleForInput);
}

bool needsInputInfo(const LayerSnapshot& snapshot, const RequestedLayerState& requested) {
    if (requested.potentialCursor) {
        return false;
    }

    if (snapshot.inputInfo.token != nullptr) {
        return true;
    }

    if (snapshot.hasBufferOrSidebandStream()) {
        return true;
    }

    return requested.windowInfoHandle &&
            requested.windowInfoHandle->getInfo()->inputConfig.test(
                    gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL);
}

void clearChanges(LayerSnapshot& snapshot) {
    snapshot.changes.clear();
    snapshot.contentDirty = false;
    snapshot.hasReadyFrame = false;
    snapshot.sidebandStreamHasFrame = false;
    snapshot.surfaceDamage.clear();
}

} // namespace

LayerSnapshot LayerSnapshotBuilder::getRootSnapshot() {
    LayerSnapshot snapshot;
    snapshot.changes = ftl::Flags<RequestedLayerState::Changes>();
    snapshot.isHiddenByPolicyFromParent = false;
    snapshot.isHiddenByPolicyFromRelativeParent = false;
    snapshot.parentTransform.reset();
    snapshot.geomLayerTransform.reset();
    snapshot.geomInverseLayerTransform.reset();
    snapshot.geomLayerBounds = getMaxDisplayBounds({});
    snapshot.roundedCorner = RoundedCornerState();
    snapshot.stretchEffect = {};
    snapshot.outputFilter.layerStack = ui::DEFAULT_LAYER_STACK;
    snapshot.outputFilter.toInternalDisplay = false;
    snapshot.isSecure = false;
    snapshot.color.a = 1.0_hf;
    snapshot.colorTransformIsIdentity = true;
    snapshot.shadowRadius = 0.f;
    snapshot.layerMetadata.mMap.clear();
    snapshot.relativeLayerMetadata.mMap.clear();
    snapshot.inputInfo.touchOcclusionMode = gui::TouchOcclusionMode::BLOCK_UNTRUSTED;
    snapshot.dropInputMode = gui::DropInputMode::NONE;
    snapshot.isTrustedOverlay = false;
    snapshot.gameMode = gui::GameMode::Unsupported;
    snapshot.frameRate = {};
    snapshot.fixedTransformHint = ui::Transform::ROT_INVALID;
    return snapshot;
}

LayerSnapshotBuilder::LayerSnapshotBuilder() : mRootSnapshot(getRootSnapshot()) {}

LayerSnapshotBuilder::LayerSnapshotBuilder(Args args) : LayerSnapshotBuilder() {
    args.forceUpdate = true;
    updateSnapshots(args);
}

bool LayerSnapshotBuilder::tryFastUpdate(const Args& args) {
    if (args.forceUpdate || args.displayChanges) {
        // force update requested, or we have display changes, so skip the fast path
        return false;
    }

    if (args.layerLifecycleManager.getGlobalChanges().get() == 0) {
        // there are no changes, so just clear the change flags from before.
        for (auto& snapshot : mSnapshots) {
            clearChanges(*snapshot);
        }
        return true;
    }

    if (args.layerLifecycleManager.getGlobalChanges() != RequestedLayerState::Changes::Content) {
        // We have changes that require us to walk the hierarchy and update child layers.
        // No fast path for you.
        return false;
    }

    // There are only content changes which do not require any child layer snapshots to be updated.
    ALOGV("%s", __func__);
    ATRACE_NAME("FastPath");

    // Collect layers with changes
    ftl::SmallMap<uint32_t, RequestedLayerState*, 10> layersWithChanges;
    for (auto& layer : args.layerLifecycleManager.getLayers()) {
        if (layer->changes.test(RequestedLayerState::Changes::Content)) {
            layersWithChanges.emplace_or_replace(layer->id, layer.get());
        }
    }

    // Walk through the snapshots, clearing previous change flags and updating the snapshots
    // if needed.
    for (auto& snapshot : mSnapshots) {
        clearChanges(*snapshot);
        auto it = layersWithChanges.find(snapshot->path.id);
        if (it != layersWithChanges.end()) {
            ALOGV("%s fast path snapshot changes = %s", __func__,
                  mRootSnapshot.changes.string().c_str());
            LayerHierarchy::TraversalPath root = LayerHierarchy::TraversalPath::ROOT;
            updateSnapshot(*snapshot, args, *it->second, mRootSnapshot, root,
                           /*newSnapshot=*/false);
        }
    }
    return true;
}

void LayerSnapshotBuilder::updateSnapshots(const Args& args) {
    ATRACE_NAME("UpdateSnapshots");
    if (args.parentCrop) {
        mRootSnapshot.geomLayerBounds = *args.parentCrop;
    } else if (args.forceUpdate || args.displayChanges) {
        mRootSnapshot.geomLayerBounds = getMaxDisplayBounds(args.displays);
    }
    if (args.displayChanges) {
        mRootSnapshot.changes = RequestedLayerState::Changes::AffectsChildren |
                RequestedLayerState::Changes::Geometry;
    }
    LayerHierarchy::TraversalPath root = LayerHierarchy::TraversalPath::ROOT;
    for (auto& [childHierarchy, variant] : args.root.mChildren) {
        LayerHierarchy::ScopedAddToTraversalPath addChildToPath(root,
                                                                childHierarchy->getLayer()->id,
                                                                variant);
        updateSnapshotsInHierarchy(args, *childHierarchy, root, mRootSnapshot);
    }

    sortSnapshotsByZ(args);
    clearChanges(mRootSnapshot);

    // Destroy unreachable snapshots
    if (args.layerLifecycleManager.getDestroyedLayers().empty()) {
        return;
    }

    std::unordered_set<uint32_t> destroyedLayerIds;
    for (auto& destroyedLayer : args.layerLifecycleManager.getDestroyedLayers()) {
        destroyedLayerIds.emplace(destroyedLayer->id);
    }
    auto it = mSnapshots.begin();
    while (it < mSnapshots.end()) {
        auto& traversalPath = it->get()->path;
        if (destroyedLayerIds.find(traversalPath.id) == destroyedLayerIds.end()) {
            it++;
            continue;
        }

        mIdToSnapshot.erase(traversalPath);
        mSnapshots.back()->globalZ = it->get()->globalZ;
        std::iter_swap(it, mSnapshots.end() - 1);
        mSnapshots.erase(mSnapshots.end() - 1);
    }
}

void LayerSnapshotBuilder::update(const Args& args) {
    if (tryFastUpdate(args)) {
        return;
    }
    updateSnapshots(args);
}

const LayerSnapshot& LayerSnapshotBuilder::updateSnapshotsInHierarchy(
        const Args& args, const LayerHierarchy& hierarchy,
        LayerHierarchy::TraversalPath& traversalPath, const LayerSnapshot& parentSnapshot) {
    const RequestedLayerState* layer = hierarchy.getLayer();
    LayerSnapshot* snapshot = getSnapshot(traversalPath);
    const bool newSnapshot = snapshot == nullptr;
    if (newSnapshot) {
        snapshot = createSnapshot(traversalPath, *layer);
    }
    if (traversalPath.isRelative()) {
        bool parentIsRelative = traversalPath.variant == LayerHierarchy::Variant::Relative;
        updateRelativeState(*snapshot, parentSnapshot, parentIsRelative, args);
    } else {
        if (traversalPath.isAttached()) {
            resetRelativeState(*snapshot);
        }
        updateSnapshot(*snapshot, args, *layer, parentSnapshot, traversalPath, newSnapshot);
    }

    for (auto& [childHierarchy, variant] : hierarchy.mChildren) {
        LayerHierarchy::ScopedAddToTraversalPath addChildToPath(traversalPath,
                                                                childHierarchy->getLayer()->id,
                                                                variant);
        const LayerSnapshot& childSnapshot =
                updateSnapshotsInHierarchy(args, *childHierarchy, traversalPath, *snapshot);
        updateChildState(*snapshot, childSnapshot, args);
    }
    return *snapshot;
}

LayerSnapshot* LayerSnapshotBuilder::getSnapshot(uint32_t layerId) const {
    if (layerId == UNASSIGNED_LAYER_ID) {
        return nullptr;
    }
    LayerHierarchy::TraversalPath path{.id = layerId};
    return getSnapshot(path);
}

LayerSnapshot* LayerSnapshotBuilder::getSnapshot(const LayerHierarchy::TraversalPath& id) const {
    auto it = mIdToSnapshot.find(id);
    return it == mIdToSnapshot.end() ? nullptr : it->second;
}

LayerSnapshot* LayerSnapshotBuilder::createSnapshot(const LayerHierarchy::TraversalPath& id,
                                                    const RequestedLayerState& layer) {
    mSnapshots.emplace_back(std::make_unique<LayerSnapshot>(layer, id));
    LayerSnapshot* snapshot = mSnapshots.back().get();
    snapshot->globalZ = static_cast<size_t>(mSnapshots.size()) - 1;
    mIdToSnapshot[id] = snapshot;
    return snapshot;
}

void LayerSnapshotBuilder::sortSnapshotsByZ(const Args& args) {
    if (!mResortSnapshots && !args.forceUpdate &&
        !args.layerLifecycleManager.getGlobalChanges().any(
                RequestedLayerState::Changes::Hierarchy |
                RequestedLayerState::Changes::Visibility)) {
        // We are not force updating and there are no hierarchy or visibility changes. Avoid sorting
        // the snapshots.
        return;
    }

    mResortSnapshots = false;

    size_t globalZ = 0;
    args.root.traverseInZOrder(
            [this, &globalZ](const LayerHierarchy&,
                             const LayerHierarchy::TraversalPath& traversalPath) -> bool {
                LayerSnapshot* snapshot = getSnapshot(traversalPath);
                if (!snapshot) {
                    return false;
                }

                if (snapshot->isHiddenByPolicy() &&
                    !snapshot->changes.test(RequestedLayerState::Changes::Visibility)) {
                    return false;
                }

                if (snapshot->getIsVisible() || snapshot->hasInputInfo()) {
                    updateVisibility(*snapshot, snapshot->getIsVisible());
                    size_t oldZ = snapshot->globalZ;
                    size_t newZ = globalZ++;
                    snapshot->globalZ = newZ;
                    if (oldZ == newZ) {
                        return true;
                    }
                    mSnapshots[newZ]->globalZ = oldZ;
                    LLOGV(snapshot->sequence, "Made visible z=%zu -> %zu %s", oldZ, newZ,
                          snapshot->getDebugString().c_str());
                    std::iter_swap(mSnapshots.begin() + static_cast<ssize_t>(oldZ),
                                   mSnapshots.begin() + static_cast<ssize_t>(newZ));
                }
                return true;
            });
    mNumInterestingSnapshots = (int)globalZ;
    while (globalZ < mSnapshots.size()) {
        mSnapshots[globalZ]->globalZ = globalZ;
        /* mark unreachable snapshots as explicitly invisible */
        updateVisibility(*mSnapshots[globalZ], false);
        globalZ++;
    }
}

void LayerSnapshotBuilder::updateRelativeState(LayerSnapshot& snapshot,
                                               const LayerSnapshot& parentSnapshot,
                                               bool parentIsRelative, const Args& args) {
    if (parentIsRelative) {
        snapshot.isHiddenByPolicyFromRelativeParent =
                parentSnapshot.isHiddenByPolicyFromParent || parentSnapshot.invalidTransform;
        if (args.includeMetadata) {
            snapshot.relativeLayerMetadata = parentSnapshot.layerMetadata;
        }
    } else {
        snapshot.isHiddenByPolicyFromRelativeParent =
                parentSnapshot.isHiddenByPolicyFromRelativeParent;
        if (args.includeMetadata) {
            snapshot.relativeLayerMetadata = parentSnapshot.relativeLayerMetadata;
        }
    }
    snapshot.isVisible = snapshot.getIsVisible();
}

void LayerSnapshotBuilder::updateChildState(LayerSnapshot& snapshot,
                                            const LayerSnapshot& childSnapshot, const Args& args) {
    if (snapshot.childState.hasValidFrameRate) {
        return;
    }
    if (args.forceUpdate || childSnapshot.changes.test(RequestedLayerState::Changes::FrameRate)) {
        // We return whether this layer ot its children has a vote. We ignore ExactOrMultiple votes
        // for the same reason we are allowing touch boost for those layers. See
        // RefreshRateSelector::rankFrameRates for details.
        using FrameRateCompatibility = scheduler::LayerInfo::FrameRateCompatibility;
        const auto layerVotedWithDefaultCompatibility = childSnapshot.frameRate.rate.isValid() &&
                childSnapshot.frameRate.type == FrameRateCompatibility::Default;
        const auto layerVotedWithNoVote =
                childSnapshot.frameRate.type == FrameRateCompatibility::NoVote;
        const auto layerVotedWithExactCompatibility = childSnapshot.frameRate.rate.isValid() &&
                childSnapshot.frameRate.type == FrameRateCompatibility::Exact;

        snapshot.childState.hasValidFrameRate |= layerVotedWithDefaultCompatibility ||
                layerVotedWithNoVote || layerVotedWithExactCompatibility;

        // If we don't have a valid frame rate, but the children do, we set this
        // layer as NoVote to allow the children to control the refresh rate
        if (!snapshot.frameRate.rate.isValid() &&
            snapshot.frameRate.type != FrameRateCompatibility::NoVote &&
            snapshot.childState.hasValidFrameRate) {
            snapshot.frameRate =
                    scheduler::LayerInfo::FrameRate(Fps(), FrameRateCompatibility::NoVote);
            snapshot.changes |= childSnapshot.changes & RequestedLayerState::Changes::FrameRate;
        }
    }
}

void LayerSnapshotBuilder::resetRelativeState(LayerSnapshot& snapshot) {
    snapshot.isHiddenByPolicyFromRelativeParent = false;
    snapshot.relativeLayerMetadata.mMap.clear();
}

uint32_t getDisplayRotationFlags(
        const display::DisplayMap<ui::LayerStack, frontend::DisplayInfo>& displays,
        const ui::LayerStack& layerStack) {
    static frontend::DisplayInfo sDefaultDisplayInfo = {.isPrimary = false};
    auto display = displays.get(layerStack).value_or(sDefaultDisplayInfo).get();
    return display.isPrimary ? display.rotationFlags : 0;
}

void LayerSnapshotBuilder::updateSnapshot(LayerSnapshot& snapshot, const Args& args,
                                          const RequestedLayerState& requested,
                                          const LayerSnapshot& parentSnapshot,
                                          const LayerHierarchy::TraversalPath& path,
                                          bool newSnapshot) {
    // Always update flags and visibility
    ftl::Flags<RequestedLayerState::Changes> parentChanges = parentSnapshot.changes &
            (RequestedLayerState::Changes::Hierarchy | RequestedLayerState::Changes::Geometry |
             RequestedLayerState::Changes::Visibility | RequestedLayerState::Changes::Metadata |
             RequestedLayerState::Changes::AffectsChildren);
    snapshot.changes = parentChanges | requested.changes;
    snapshot.isHiddenByPolicyFromParent = parentSnapshot.isHiddenByPolicyFromParent ||
            parentSnapshot.invalidTransform || requested.isHiddenByPolicy() ||
            (args.excludeLayerIds.find(path.id) != args.excludeLayerIds.end());
    snapshot.contentDirty = requested.what & layer_state_t::CONTENT_DIRTY;
    // TODO(b/238781169) scope down the changes to only buffer updates.
    snapshot.hasReadyFrame =
            (snapshot.contentDirty || requested.autoRefresh) && (requested.externalTexture);
    // TODO(b/238781169) how is this used? ag/15523870
    snapshot.sidebandStreamHasFrame = false;
    updateSurfaceDamage(requested, snapshot.hasReadyFrame, args.forceFullDamage,
                        snapshot.surfaceDamage);

    const bool forceUpdate = newSnapshot || args.forceUpdate ||
            snapshot.changes.any(RequestedLayerState::Changes::Visibility |
                                 RequestedLayerState::Changes::Created);
    snapshot.outputFilter.layerStack = requested.parentId != UNASSIGNED_LAYER_ID
            ? parentSnapshot.outputFilter.layerStack
            : requested.layerStack;

    uint32_t displayRotationFlags =
            getDisplayRotationFlags(args.displays, snapshot.outputFilter.layerStack);

    // always update the buffer regardless of visibility
    if (forceUpdate || requested.what & layer_state_t::BUFFER_CHANGES || args.displayChanges) {
        snapshot.acquireFence =
                (requested.externalTexture &&
                 requested.bufferData->flags.test(BufferData::BufferDataChange::fenceChanged))
                ? requested.bufferData->acquireFence
                : Fence::NO_FENCE;
        snapshot.buffer =
                requested.externalTexture ? requested.externalTexture->getBuffer() : nullptr;
        snapshot.bufferSize = requested.getBufferSize(displayRotationFlags);
        snapshot.geomBufferSize = snapshot.bufferSize;
        snapshot.croppedBufferSize = requested.getCroppedBufferSize(snapshot.bufferSize);
        snapshot.dataspace = requested.dataspace;
        snapshot.externalTexture = requested.externalTexture;
        snapshot.frameNumber = (requested.bufferData) ? requested.bufferData->frameNumber : 0;
        snapshot.geomBufferTransform = requested.bufferTransform;
        snapshot.geomBufferUsesDisplayInverseTransform = requested.transformToDisplayInverse;
        snapshot.geomContentCrop = requested.getBufferCrop();
        snapshot.geomUsesSourceCrop = snapshot.hasBufferOrSidebandStream();
        snapshot.hasProtectedContent = requested.externalTexture &&
                requested.externalTexture->getUsage() & GRALLOC_USAGE_PROTECTED;
        snapshot.isHdrY410 = requested.dataspace == ui::Dataspace::BT2020_ITU_PQ &&
                requested.api == NATIVE_WINDOW_API_MEDIA &&
                requested.bufferData->getPixelFormat() == HAL_PIXEL_FORMAT_RGBA_1010102;
        snapshot.sidebandStream = requested.sidebandStream;
        snapshot.transparentRegionHint = requested.transparentRegion;
        snapshot.color.rgb = requested.getColor().rgb;
        snapshot.currentSdrHdrRatio = requested.currentSdrHdrRatio;
        snapshot.desiredSdrHdrRatio = requested.desiredSdrHdrRatio;
    }

    if (snapshot.isHiddenByPolicyFromParent && !newSnapshot) {
        if (forceUpdate ||
            snapshot.changes.any(RequestedLayerState::Changes::Hierarchy |
                                 RequestedLayerState::Changes::Geometry |
                                 RequestedLayerState::Changes::Input)) {
            updateInput(snapshot, requested, parentSnapshot, path, args);
        }
        return;
    }

    if (forceUpdate || snapshot.changes.any(RequestedLayerState::Changes::AffectsChildren)) {
        // If root layer, use the layer stack otherwise get the parent's layer stack.
        snapshot.color.a = parentSnapshot.color.a * requested.color.a;
        snapshot.alpha = snapshot.color.a;
        snapshot.isSecure =
                parentSnapshot.isSecure || (requested.flags & layer_state_t::eLayerSecure);
        snapshot.isTrustedOverlay = parentSnapshot.isTrustedOverlay || requested.isTrustedOverlay;
        snapshot.outputFilter.layerStack = requested.parentId != UNASSIGNED_LAYER_ID
                ? parentSnapshot.outputFilter.layerStack
                : requested.layerStack;
        snapshot.outputFilter.toInternalDisplay = parentSnapshot.outputFilter.toInternalDisplay ||
                (requested.flags & layer_state_t::eLayerSkipScreenshot);
        snapshot.stretchEffect = (requested.stretchEffect.hasEffect())
                ? requested.stretchEffect
                : parentSnapshot.stretchEffect;
        if (!parentSnapshot.colorTransformIsIdentity) {
            snapshot.colorTransform = parentSnapshot.colorTransform * requested.colorTransform;
            snapshot.colorTransformIsIdentity = false;
        } else {
            snapshot.colorTransform = requested.colorTransform;
            snapshot.colorTransformIsIdentity = !requested.hasColorTransform;
        }
        snapshot.gameMode = requested.metadata.has(gui::METADATA_GAME_MODE)
                ? requested.gameMode
                : parentSnapshot.gameMode;
        snapshot.frameRate = (requested.requestedFrameRate.rate.isValid() ||
                              (requested.requestedFrameRate.type ==
                               scheduler::LayerInfo::FrameRateCompatibility::NoVote))
                ? requested.requestedFrameRate
                : parentSnapshot.frameRate;
        snapshot.fixedTransformHint = requested.fixedTransformHint != ui::Transform::ROT_INVALID
                ? requested.fixedTransformHint
                : parentSnapshot.fixedTransformHint;
        // Display mirrors are always placed in a VirtualDisplay so we never want to capture layers
        // marked as skip capture
        snapshot.handleSkipScreenshotFlag = parentSnapshot.handleSkipScreenshotFlag ||
                (requested.layerStackToMirror != ui::INVALID_LAYER_STACK);
    }

    if (forceUpdate || requested.changes.get() != 0) {
        snapshot.compositionType = requested.getCompositionType();
        snapshot.dimmingEnabled = requested.dimmingEnabled;
        snapshot.layerOpaqueFlagSet =
                (requested.flags & layer_state_t::eLayerOpaque) == layer_state_t::eLayerOpaque;
        snapshot.cachingHint = requested.cachingHint;
    }

    if (forceUpdate || snapshot.changes.any(RequestedLayerState::Changes::Content)) {
        snapshot.color.rgb = requested.getColor().rgb;
        snapshot.isColorspaceAgnostic = requested.colorSpaceAgnostic;
        snapshot.backgroundBlurRadius = args.supportsBlur
                ? static_cast<int>(parentSnapshot.color.a * (float)requested.backgroundBlurRadius)
                : 0;
        snapshot.blurRegions = requested.blurRegions;
        for (auto& region : snapshot.blurRegions) {
            region.alpha = region.alpha * snapshot.color.a;
        }
        snapshot.hdrMetadata = requested.hdrMetadata;
    }

    if (forceUpdate ||
        snapshot.changes.any(RequestedLayerState::Changes::Hierarchy |
                             RequestedLayerState::Changes::Geometry)) {
        updateLayerBounds(snapshot, requested, parentSnapshot, displayRotationFlags);
        updateRoundedCorner(snapshot, requested, parentSnapshot);
    }

    if (forceUpdate ||
        snapshot.changes.any(RequestedLayerState::Changes::Hierarchy |
                             RequestedLayerState::Changes::Geometry |
                             RequestedLayerState::Changes::Input)) {
        updateInput(snapshot, requested, parentSnapshot, path, args);
    }

    // computed snapshot properties
    updateShadows(snapshot, requested, args.globalShadowSettings);
    if (args.includeMetadata) {
        snapshot.layerMetadata = parentSnapshot.layerMetadata;
        snapshot.layerMetadata.merge(requested.metadata);
    }
    snapshot.forceClientComposition = snapshot.isHdrY410 || snapshot.shadowSettings.length > 0 ||
            requested.blurRegions.size() > 0 || snapshot.stretchEffect.hasEffect();
    snapshot.isOpaque = snapshot.isContentOpaque() && !snapshot.roundedCorner.hasRoundedCorners() &&
            snapshot.color.a == 1.f;
    snapshot.blendMode = getBlendMode(snapshot, requested);
    // TODO(b/238781169) pass this from flinger
    // snapshot.fps;
    // snapshot.metadata;
    LLOGV(snapshot.sequence,
          "%supdated [%d]%s changes parent:%s global:%s local:%s requested:%s %s from parent %s",
          args.forceUpdate ? "Force " : "", requested.id, requested.name.c_str(),
          parentSnapshot.changes.string().c_str(), snapshot.changes.string().c_str(),
          requested.changes.string().c_str(), std::to_string(requested.what).c_str(),
          snapshot.getDebugString().c_str(), parentSnapshot.getDebugString().c_str());
}

void LayerSnapshotBuilder::updateRoundedCorner(LayerSnapshot& snapshot,
                                               const RequestedLayerState& requested,
                                               const LayerSnapshot& parentSnapshot) {
    snapshot.roundedCorner = RoundedCornerState();
    RoundedCornerState parentRoundedCorner;
    if (parentSnapshot.roundedCorner.hasRoundedCorners()) {
        parentRoundedCorner = parentSnapshot.roundedCorner;
        ui::Transform t = snapshot.localTransform.inverse();
        parentRoundedCorner.cropRect = t.transform(parentRoundedCorner.cropRect);
        parentRoundedCorner.radius.x *= t.getScaleX();
        parentRoundedCorner.radius.y *= t.getScaleY();
    }

    FloatRect layerCropRect = snapshot.croppedBufferSize.toFloatRect();
    const vec2 radius(requested.cornerRadius, requested.cornerRadius);
    RoundedCornerState layerSettings(layerCropRect, radius);
    const bool layerSettingsValid = layerSettings.hasRoundedCorners() && !layerCropRect.isEmpty();
    const bool parentRoundedCornerValid = parentRoundedCorner.hasRoundedCorners();
    if (layerSettingsValid && parentRoundedCornerValid) {
        // If the parent and the layer have rounded corner settings, use the parent settings if
        // the parent crop is entirely inside the layer crop. This has limitations and cause
        // rendering artifacts. See b/200300845 for correct fix.
        if (parentRoundedCorner.cropRect.left > layerCropRect.left &&
            parentRoundedCorner.cropRect.top > layerCropRect.top &&
            parentRoundedCorner.cropRect.right < layerCropRect.right &&
            parentRoundedCorner.cropRect.bottom < layerCropRect.bottom) {
            snapshot.roundedCorner = parentRoundedCorner;
        } else {
            snapshot.roundedCorner = layerSettings;
        }
    } else if (layerSettingsValid) {
        snapshot.roundedCorner = layerSettings;
    } else if (parentRoundedCornerValid) {
        snapshot.roundedCorner = parentRoundedCorner;
    }
}

void LayerSnapshotBuilder::updateLayerBounds(LayerSnapshot& snapshot,
                                             const RequestedLayerState& requested,
                                             const LayerSnapshot& parentSnapshot,
                                             uint32_t displayRotationFlags) {
    snapshot.croppedBufferSize = requested.getCroppedBufferSize(snapshot.bufferSize);
    snapshot.geomCrop = requested.crop;
    snapshot.localTransform = requested.getTransform(displayRotationFlags);
    snapshot.localTransformInverse = snapshot.localTransform.inverse();
    snapshot.geomLayerTransform = parentSnapshot.geomLayerTransform * snapshot.localTransform;
    const bool transformWasInvalid = snapshot.invalidTransform;
    snapshot.invalidTransform = !LayerSnapshot::isTransformValid(snapshot.geomLayerTransform);
    if (snapshot.invalidTransform) {
        auto& t = snapshot.geomLayerTransform;
        auto& requestedT = requested.requestedTransform;
        std::string transformDebug =
                base::StringPrintf(" transform={%f,%f,%f,%f}  requestedTransform={%f,%f,%f,%f}",
                                   t.dsdx(), t.dsdy(), t.dtdx(), t.dtdy(), requestedT.dsdx(),
                                   requestedT.dsdy(), requestedT.dtdx(), requestedT.dtdy());
        std::string bufferDebug;
        if (requested.externalTexture) {
            auto unRotBuffer = requested.getUnrotatedBufferSize(displayRotationFlags);
            auto& destFrame = requested.destinationFrame;
            bufferDebug = base::StringPrintf(" buffer={%d,%d}  displayRot=%d"
                                             " destFrame={%d,%d,%d,%d} unRotBuffer={%d,%d}",
                                             requested.externalTexture->getWidth(),
                                             requested.externalTexture->getHeight(),
                                             displayRotationFlags, destFrame.left, destFrame.top,
                                             destFrame.right, destFrame.bottom,
                                             unRotBuffer.getHeight(), unRotBuffer.getWidth());
        }
        ALOGW("Resetting transform for %s because it is invalid.%s%s",
              snapshot.getDebugString().c_str(), transformDebug.c_str(), bufferDebug.c_str());
        snapshot.geomLayerTransform.reset();
    }
    if (transformWasInvalid != snapshot.invalidTransform) {
        // If transform is invalid, the layer will be hidden.
        mResortSnapshots = true;
    }
    snapshot.geomInverseLayerTransform = snapshot.geomLayerTransform.inverse();

    FloatRect parentBounds = parentSnapshot.geomLayerBounds;
    parentBounds = snapshot.localTransform.inverse().transform(parentBounds);
    snapshot.geomLayerBounds =
            (requested.externalTexture) ? snapshot.bufferSize.toFloatRect() : parentBounds;
    if (!requested.crop.isEmpty()) {
        snapshot.geomLayerBounds = snapshot.geomLayerBounds.intersect(requested.crop.toFloatRect());
    }
    snapshot.geomLayerBounds = snapshot.geomLayerBounds.intersect(parentBounds);
    snapshot.transformedBounds = snapshot.geomLayerTransform.transform(snapshot.geomLayerBounds);
    const Rect geomLayerBoundsWithoutTransparentRegion =
            RequestedLayerState::reduce(Rect(snapshot.geomLayerBounds),
                                        requested.transparentRegion);
    snapshot.transformedBoundsWithoutTransparentRegion =
            snapshot.geomLayerTransform.transform(geomLayerBoundsWithoutTransparentRegion);
    snapshot.parentTransform = parentSnapshot.geomLayerTransform;

    // Subtract the transparent region and snap to the bounds
    const Rect bounds =
            RequestedLayerState::reduce(snapshot.croppedBufferSize, requested.transparentRegion);
    if (requested.potentialCursor) {
        snapshot.cursorFrame = snapshot.geomLayerTransform.transform(bounds);
    }
}

void LayerSnapshotBuilder::updateShadows(LayerSnapshot& snapshot,
                                         const RequestedLayerState& requested,
                                         const renderengine::ShadowSettings& globalShadowSettings) {
    snapshot.shadowRadius = requested.shadowRadius;
    snapshot.shadowSettings.length = requested.shadowRadius;
    if (snapshot.shadowRadius > 0.f) {
        snapshot.shadowSettings = globalShadowSettings;

        // Note: this preserves existing behavior of shadowing the entire layer and not cropping
        // it if transparent regions are present. This may not be necessary since shadows are
        // typically cast by layers without transparent regions.
        snapshot.shadowSettings.boundaries = snapshot.geomLayerBounds;

        // If the casting layer is translucent, we need to fill in the shadow underneath the
        // layer. Otherwise the generated shadow will only be shown around the casting layer.
        snapshot.shadowSettings.casterIsTranslucent =
                !snapshot.isContentOpaque() || (snapshot.alpha < 1.0f);
        snapshot.shadowSettings.ambientColor *= snapshot.alpha;
        snapshot.shadowSettings.spotColor *= snapshot.alpha;
    }
}

void LayerSnapshotBuilder::updateInput(LayerSnapshot& snapshot,
                                       const RequestedLayerState& requested,
                                       const LayerSnapshot& parentSnapshot,
                                       const LayerHierarchy::TraversalPath& path,
                                       const Args& args) {
    if (requested.windowInfoHandle) {
        snapshot.inputInfo = *requested.windowInfoHandle->getInfo();
    } else {
        snapshot.inputInfo = {};
    }
    snapshot.inputInfo.displayId = static_cast<int32_t>(snapshot.outputFilter.layerStack.id);

    if (!needsInputInfo(snapshot, requested)) {
        return;
    }

    static frontend::DisplayInfo sDefaultInfo = {.isSecure = false};
    const std::optional<frontend::DisplayInfo> displayInfoOpt =
            args.displays.get(snapshot.outputFilter.layerStack);
    bool noValidDisplay = !displayInfoOpt.has_value();
    auto displayInfo = displayInfoOpt.value_or(sDefaultInfo);

    if (!requested.windowInfoHandle) {
        snapshot.inputInfo.inputConfig = gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL;
    }
    fillInputFrameInfo(snapshot.inputInfo, displayInfo.transform, snapshot);

    if (noValidDisplay) {
        // Do not let the window receive touches if it is not associated with a valid display
        // transform. We still allow the window to receive keys and prevent ANRs.
        snapshot.inputInfo.inputConfig |= gui::WindowInfo::InputConfig::NOT_TOUCHABLE;
    }

    snapshot.inputInfo.alpha = snapshot.color.a;
    snapshot.inputInfo.touchOcclusionMode = parentSnapshot.inputInfo.touchOcclusionMode;
    if (requested.dropInputMode == gui::DropInputMode::ALL ||
        parentSnapshot.dropInputMode == gui::DropInputMode::ALL) {
        snapshot.dropInputMode = gui::DropInputMode::ALL;
    } else if (requested.dropInputMode == gui::DropInputMode::OBSCURED ||
               parentSnapshot.dropInputMode == gui::DropInputMode::OBSCURED) {
        snapshot.dropInputMode = gui::DropInputMode::OBSCURED;
    } else {
        snapshot.dropInputMode = gui::DropInputMode::NONE;
    }

    handleDropInputMode(snapshot, parentSnapshot);

    // If the window will be blacked out on a display because the display does not have the secure
    // flag and the layer has the secure flag set, then drop input.
    if (!displayInfo.isSecure && snapshot.isSecure) {
        snapshot.inputInfo.inputConfig |= gui::WindowInfo::InputConfig::DROP_INPUT;
    }

    auto cropLayerSnapshot = getSnapshot(requested.touchCropId);
    if (snapshot.inputInfo.replaceTouchableRegionWithCrop) {
        const Rect bounds(cropLayerSnapshot ? cropLayerSnapshot->transformedBounds
                                            : snapshot.transformedBounds);
        snapshot.inputInfo.touchableRegion = Region(displayInfo.transform.transform(bounds));
    } else if (cropLayerSnapshot) {
        snapshot.inputInfo.touchableRegion = snapshot.inputInfo.touchableRegion.intersect(
                displayInfo.transform.transform(Rect{cropLayerSnapshot->transformedBounds}));
    }

    // Inherit the trusted state from the parent hierarchy, but don't clobber the trusted state
    // if it was set by WM for a known system overlay
    if (snapshot.isTrustedOverlay) {
        snapshot.inputInfo.inputConfig |= gui::WindowInfo::InputConfig::TRUSTED_OVERLAY;
    }

    // If the layer is a clone, we need to crop the input region to cloned root to prevent
    // touches from going outside the cloned area.
    if (path.isClone()) {
        snapshot.inputInfo.inputConfig |= gui::WindowInfo::InputConfig::CLONE;
        auto clonedRootSnapshot = getSnapshot(path.getMirrorRoot());
        if (clonedRootSnapshot) {
            const Rect rect =
                    displayInfo.transform.transform(Rect{clonedRootSnapshot->transformedBounds});
            snapshot.inputInfo.touchableRegion = snapshot.inputInfo.touchableRegion.intersect(rect);
        }
    }
}

std::vector<std::unique_ptr<LayerSnapshot>>& LayerSnapshotBuilder::getSnapshots() {
    return mSnapshots;
}

void LayerSnapshotBuilder::forEachVisibleSnapshot(const ConstVisitor& visitor) const {
    for (int i = 0; i < mNumInterestingSnapshots; i++) {
        LayerSnapshot& snapshot = *mSnapshots[(size_t)i];
        if (!snapshot.isVisible) continue;
        visitor(snapshot);
    }
}

// Visit each visible snapshot in z-order
void LayerSnapshotBuilder::forEachVisibleSnapshot(const ConstVisitor& visitor,
                                                  const LayerHierarchy& root) const {
    root.traverseInZOrder(
            [this, visitor](const LayerHierarchy&,
                            const LayerHierarchy::TraversalPath& traversalPath) -> bool {
                LayerSnapshot* snapshot = getSnapshot(traversalPath);
                if (snapshot && snapshot->isVisible) {
                    visitor(*snapshot);
                }
                return true;
            });
}

void LayerSnapshotBuilder::forEachVisibleSnapshot(const Visitor& visitor) {
    for (int i = 0; i < mNumInterestingSnapshots; i++) {
        std::unique_ptr<LayerSnapshot>& snapshot = mSnapshots.at((size_t)i);
        if (!snapshot->isVisible) continue;
        visitor(snapshot);
    }
}

void LayerSnapshotBuilder::forEachInputSnapshot(const ConstVisitor& visitor) const {
    for (int i = mNumInterestingSnapshots - 1; i >= 0; i--) {
        LayerSnapshot& snapshot = *mSnapshots[(size_t)i];
        if (!snapshot.hasInputInfo()) continue;
        visitor(snapshot);
    }
}

} // namespace android::surfaceflinger::frontend
