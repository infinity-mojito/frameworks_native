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

#ifndef ANDROID_LAYER_H
#define ANDROID_LAYER_H

#include <sys/types.h>

#include <gui/BufferQueue.h>
#include <gui/ISurfaceComposerClient.h>
#include <gui/LayerState.h>
#include <layerproto/LayerProtoHeader.h>
#include <math/vec4.h>
#include <renderengine/Mesh.h>
#include <renderengine/Texture.h>
#include <ui/FloatRect.h>
#include <ui/FrameStats.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>
#include <ui/Region.h>
#include <ui/Transform.h>
#include <utils/RefBase.h>
#include <utils/String8.h>
#include <utils/Timers.h>

#include <cstdint>
#include <list>
#include <optional>
#include <vector>

#include "Client.h"
#include "FrameTracker.h"
#include "LayerBE.h"
#include "LayerVector.h"
#include "MonitoredProducer.h"
#include "SurfaceFlinger.h"
#include "TimeStats/TimeStats.h"
#include "TransactionCompletedThread.h"

#include "DisplayHardware/HWComposer.h"
#include "DisplayHardware/HWComposerBufferCache.h"
#include "RenderArea.h"

using namespace android::surfaceflinger;

namespace android {

// ---------------------------------------------------------------------------

class Client;
class Colorizer;
class DisplayDevice;
class GraphicBuffer;
class SurfaceFlinger;
class LayerDebugInfo;
class LayerBE;

namespace impl {
class SurfaceInterceptor;
}

// ---------------------------------------------------------------------------

struct LayerCreationArgs {
    LayerCreationArgs(SurfaceFlinger* flinger, const sp<Client>& client, const String8& name,
                      uint32_t w, uint32_t h, uint32_t flags)
          : flinger(flinger), client(client), name(name), w(w), h(h), flags(flags) {}

    SurfaceFlinger* flinger;
    const sp<Client>& client;
    const String8& name;
    uint32_t w;
    uint32_t h;
    uint32_t flags;
};

class Layer : public virtual RefBase {
    static std::atomic<int32_t> sSequence;

public:
    friend class LayerBE;
    LayerBE& getBE() { return mBE; }
    LayerBE& getBE() const { return mBE; }
    mutable bool contentDirty{false};
    // regions below are in window-manager space
    Region visibleRegion;
    Region coveredRegion;
    Region visibleNonTransparentRegion;
    Region surfaceDamageRegion;

    // Layer serial number.  This gives layers an explicit ordering, so we
    // have a stable sort order when their layer stack and Z-order are
    // the same.
    int32_t sequence{sSequence++};

    enum { // flags for doTransaction()
        eDontUpdateGeometryState = 0x00000001,
        eVisibleRegion = 0x00000002,
    };

    struct Geometry {
        uint32_t w;
        uint32_t h;
        ui::Transform transform;

        inline bool operator==(const Geometry& rhs) const {
            return (w == rhs.w && h == rhs.h) && (transform.tx() == rhs.transform.tx()) &&
                    (transform.ty() == rhs.transform.ty());
        }
        inline bool operator!=(const Geometry& rhs) const { return !operator==(rhs); }
    };

    struct State {
        Geometry active_legacy;
        Geometry requested_legacy;
        int32_t z;

        // The identifier of the layer stack this layer belongs to. A layer can
        // only be associated to a single layer stack. A layer stack is a
        // z-ordered group of layers which can be associated to one or more
        // displays. Using the same layer stack on different displays is a way
        // to achieve mirroring.
        uint32_t layerStack;

        uint8_t flags;
        uint8_t reserved[2];
        int32_t sequence; // changes when visible regions can change
        bool modified;

        // Crop is expressed in layer space coordinate.
        Rect crop_legacy;
        Rect requestedCrop_legacy;

        // If set, defers this state update until the identified Layer
        // receives a frame with the given frameNumber
        wp<Layer> barrierLayer_legacy;
        uint64_t frameNumber_legacy;

        // the transparentRegion hint is a bit special, it's latched only
        // when we receive a buffer -- this is because it's "content"
        // dependent.
        Region activeTransparentRegion_legacy;
        Region requestedTransparentRegion_legacy;

        int32_t appId;
        int32_t type;

        // If non-null, a Surface this Surface's Z-order is interpreted relative to.
        wp<Layer> zOrderRelativeOf;

        // A list of surfaces whose Z-order is interpreted relative to ours.
        SortedVector<wp<Layer>> zOrderRelatives;

        half4 color;

        // The fields below this point are only used by BufferStateLayer
        Geometry active;

        uint32_t transform;
        bool transformToDisplayInverse;

        Rect crop;
        Region transparentRegionHint;

        sp<GraphicBuffer> buffer;
        sp<Fence> acquireFence;
        ui::Dataspace dataspace;
        HdrMetadata hdrMetadata;
        Region surfaceDamageRegion;
        int32_t api;

        sp<NativeHandle> sidebandStream;
        mat4 colorTransform;
        bool hasColorTransform;

        // The deque of callback handles for this frame. The back of the deque contains the most
        // recent callback handle.
        std::deque<sp<CallbackHandle>> callbackHandles;
    };

    explicit Layer(const LayerCreationArgs& args);
    virtual ~Layer();

    void setPrimaryDisplayOnly() { mPrimaryDisplayOnly = true; }

    // ------------------------------------------------------------------------
    // Geometry setting functions.
    //
    // The following group of functions are used to specify the layers
    // bounds, and the mapping of the texture on to those bounds. According
    // to various settings changes to them may apply immediately, or be delayed until
    // a pending resize is completed by the producer submitting a buffer. For example
    // if we were to change the buffer size, and update the matrix ahead of the
    // new buffer arriving, then we would be stretching the buffer to a different
    // aspect before and after the buffer arriving, which probably isn't what we wanted.
    //
    // The first set of geometry functions are controlled by the scaling mode, described
    // in window.h. The scaling mode may be set by the client, as it submits buffers.
    // This value may be overriden through SurfaceControl, with setOverrideScalingMode.
    //
    // Put simply, if our scaling mode is SCALING_MODE_FREEZE, then
    // matrix updates will not be applied while a resize is pending
    // and the size and transform will remain in their previous state
    // until a new buffer is submitted. If the scaling mode is another value
    // then the old-buffer will immediately be scaled to the pending size
    // and the new matrix will be immediately applied following this scaling
    // transformation.

    // Set the default buffer size for the assosciated Producer, in pixels. This is
    // also the rendered size of the layer prior to any transformations. Parent
    // or local matrix transformations will not affect the size of the buffer,
    // but may affect it's on-screen size or clipping.
    virtual bool setSize(uint32_t w, uint32_t h);
    // Set a 2x2 transformation matrix on the layer. This transform
    // will be applied after parent transforms, but before any final
    // producer specified transform.
    virtual bool setMatrix(const layer_state_t::matrix22_t& matrix,
                           bool allowNonRectPreservingTransforms);

    // This second set of geometry attributes are controlled by
    // setGeometryAppliesWithResize, and their default mode is to be
    // immediate. If setGeometryAppliesWithResize is specified
    // while a resize is pending, then update of these attributes will
    // be delayed until the resize completes.

    // setPosition operates in parent buffer space (pre parent-transform) or display
    // space for top-level layers.
    virtual bool setPosition(float x, float y, bool immediate);
    // Buffer space
    virtual bool setCrop_legacy(const Rect& crop, bool immediate);

    // TODO(b/38182121): Could we eliminate the various latching modes by
    // using the layer hierarchy?
    // -----------------------------------------------------------------------
    virtual bool setLayer(int32_t z);
    virtual bool setRelativeLayer(const sp<IBinder>& relativeToHandle, int32_t relativeZ);

    virtual bool setAlpha(float alpha);
    virtual bool setColor(const half3& color);
    virtual bool setTransparentRegionHint(const Region& transparent);
    virtual bool setFlags(uint8_t flags, uint8_t mask);
    virtual bool setLayerStack(uint32_t layerStack);
    virtual uint32_t getLayerStack() const;
    virtual void deferTransactionUntil_legacy(const sp<IBinder>& barrierHandle,
                                              uint64_t frameNumber);
    virtual void deferTransactionUntil_legacy(const sp<Layer>& barrierLayer, uint64_t frameNumber);
    virtual bool setOverrideScalingMode(int32_t overrideScalingMode);
    virtual void setInfo(int32_t type, int32_t appId);
    virtual bool reparentChildren(const sp<IBinder>& layer);
    virtual void setChildrenDrawingParent(const sp<Layer>& layer);
    virtual bool reparent(const sp<IBinder>& newParentHandle);
    virtual bool detachChildren();
    virtual bool setColorTransform(const mat4& matrix);
    virtual const mat4& getColorTransform() const;
    virtual bool hasColorTransform() const;

    // Used only to set BufferStateLayer state
    virtual bool setTransform(uint32_t /*transform*/) { return false; };
    virtual bool setTransformToDisplayInverse(bool /*transformToDisplayInverse*/) { return false; };
    virtual bool setCrop(const Rect& /*crop*/) { return false; };
    virtual bool setBuffer(const sp<GraphicBuffer>& /*buffer*/) { return false; };
    virtual bool setAcquireFence(const sp<Fence>& /*fence*/) { return false; };
    virtual bool setDataspace(ui::Dataspace /*dataspace*/) { return false; };
    virtual bool setHdrMetadata(const HdrMetadata& /*hdrMetadata*/) { return false; };
    virtual bool setSurfaceDamageRegion(const Region& /*surfaceDamage*/) { return false; };
    virtual bool setApi(int32_t /*api*/) { return false; };
    virtual bool setSidebandStream(const sp<NativeHandle>& /*sidebandStream*/) { return false; };
    virtual bool setTransactionCompletedListeners(
            const std::vector<sp<CallbackHandle>>& /*handles*/) {
        return false;
    };

    ui::Dataspace getDataSpace() const { return mCurrentDataSpace; }

    // Before color management is introduced, contents on Android have to be
    // desaturated in order to match what they appears like visually.
    // With color management, these contents will appear desaturated, thus
    // needed to be saturated so that they match what they are designed for
    // visually.
    bool isLegacyDataSpace() const;

    // If we have received a new buffer this frame, we will pass its surface
    // damage down to hardware composer. Otherwise, we must send a region with
    // one empty rect.
    virtual void useSurfaceDamage() {}
    virtual void useEmptyDamage() {}

    uint32_t getTransactionFlags() const { return mTransactionFlags; }
    uint32_t getTransactionFlags(uint32_t flags);
    uint32_t setTransactionFlags(uint32_t flags);

    bool belongsToDisplay(uint32_t layerStack, bool isPrimaryDisplay) const {
        return getLayerStack() == layerStack && (!mPrimaryDisplayOnly || isPrimaryDisplay);
    }

    void computeGeometry(const RenderArea& renderArea, renderengine::Mesh& mesh,
                         bool useIdentityTransform) const;
    FloatRect computeBounds(const Region& activeTransparentRegion) const;
    FloatRect computeBounds() const;

    int32_t getSequence() const { return sequence; }

    // -----------------------------------------------------------------------
    // Virtuals
    virtual const char* getTypeId() const = 0;

    /*
     * isOpaque - true if this surface is opaque
     *
     * This takes into account the buffer format (i.e. whether or not the
     * pixel format includes an alpha channel) and the "opaque" flag set
     * on the layer.  It does not examine the current plane alpha value.
     */
    virtual bool isOpaque(const Layer::State&) const { return false; }

    /*
     * isSecure - true if this surface is secure, that is if it prevents
     * screenshots or VNC servers.
     */
    bool isSecure() const;

    /*
     * isVisible - true if this layer is visible, false otherwise
     */
    virtual bool isVisible() const = 0;

    /*
     * isHiddenByPolicy - true if this layer has been forced invisible.
     * just because this is false, doesn't mean isVisible() is true.
     * For example if this layer has no active buffer, it may not be hidden by
     * policy, but it still can not be visible.
     */
    bool isHiddenByPolicy() const;

    /*
     * isFixedSize - true if content has a fixed size
     */
    virtual bool isFixedSize() const { return true; }

    // Most layers aren't created from the main thread, and therefore need to
    // grab the SF state lock to access HWC, but ContainerLayer does, so we need
    // to avoid grabbing the lock again to avoid deadlock
    virtual bool isCreatedFromMainThread() const { return false; }


    bool isRemovedFromCurrentState() const;

    void writeToProto(LayerProto* layerInfo,
                      LayerVector::StateSet stateSet = LayerVector::StateSet::Drawing);

    void writeToProto(LayerProto* layerInfo, DisplayId displayId);

    virtual Geometry getActiveGeometry(const Layer::State& s) const { return s.active_legacy; }
    virtual uint32_t getActiveWidth(const Layer::State& s) const { return s.active_legacy.w; }
    virtual uint32_t getActiveHeight(const Layer::State& s) const { return s.active_legacy.h; }
    virtual ui::Transform getActiveTransform(const Layer::State& s) const {
        return s.active_legacy.transform;
    }
    virtual Region getActiveTransparentRegion(const Layer::State& s) const {
        return s.activeTransparentRegion_legacy;
    }
    virtual Rect getCrop(const Layer::State& s) const { return s.crop_legacy; }

protected:
    /*
     * onDraw - draws the surface.
     */
    virtual void onDraw(const RenderArea& renderArea, const Region& clip,
                        bool useIdentityTransform) = 0;

public:
    virtual void setDefaultBufferSize(uint32_t /*w*/, uint32_t /*h*/) {}

    virtual bool isHdrY410() const { return false; }

    void setGeometry(const sp<const DisplayDevice>& display, uint32_t z);
    void forceClientComposition(DisplayId displayId);
    bool getForceClientComposition(DisplayId displayId);
    virtual void setPerFrameData(DisplayId displayId, const ui::Transform& transform,
                                 const Rect& viewport, int32_t supportedPerFrameMetadata) = 0;

    // callIntoHwc exists so we can update our local state and call
    // acceptDisplayChanges without unnecessarily updating the device's state
    void setCompositionType(DisplayId displayId, HWC2::Composition type, bool callIntoHwc = true);
    HWC2::Composition getCompositionType(const std::optional<DisplayId>& displayId) const;
    void setClearClientTarget(DisplayId displayId, bool clear);
    bool getClearClientTarget(DisplayId displayId) const;
    void updateCursorPosition(const sp<const DisplayDevice>& display);

    /*
     * called after page-flip
     */
    virtual void onLayerDisplayed(const sp<Fence>& releaseFence);

    virtual bool shouldPresentNow(nsecs_t /*expectedPresentTime*/) const { return false; }
    virtual void setTransformHint(uint32_t /*orientation*/) const { }

    /*
     * called before composition.
     * returns true if the layer has pending updates.
     */
    virtual bool onPreComposition(nsecs_t refreshStartTime) = 0;

    /*
     * called after composition.
     * returns true if the layer latched a new buffer this frame.
     */
    virtual bool onPostComposition(const std::optional<DisplayId>& /*displayId*/,
                                   const std::shared_ptr<FenceTime>& /*glDoneFence*/,
                                   const std::shared_ptr<FenceTime>& /*presentFence*/,
                                   const CompositorTiming& /*compositorTiming*/) {
        return false;
    }

    // If a buffer was replaced this frame, release the former buffer
    virtual void releasePendingBuffer(nsecs_t /*dequeueReadyTime*/) { }


    /*
     * draw - performs some global clipping optimizations
     * and calls onDraw().
     */
    void draw(const RenderArea& renderArea, const Region& clip);
    void draw(const RenderArea& renderArea, bool useIdentityTransform);

    /*
     * doTransaction - process the transaction. This is a good place to figure
     * out which attributes of the surface have changed.
     */
    uint32_t doTransaction(uint32_t transactionFlags);

    /*
     * setVisibleRegion - called to set the new visible region. This gives
     * a chance to update the new visible region or record the fact it changed.
     */
    void setVisibleRegion(const Region& visibleRegion);

    /*
     * setCoveredRegion - called when the covered region changes. The covered
     * region corresponds to any area of the surface that is covered
     * (transparently or not) by another surface.
     */
    void setCoveredRegion(const Region& coveredRegion);

    /*
     * setVisibleNonTransparentRegion - called when the visible and
     * non-transparent region changes.
     */
    void setVisibleNonTransparentRegion(const Region& visibleNonTransparentRegion);

    /*
     * Clear the visible, covered, and non-transparent regions.
     */
    void clearVisibilityRegions();

    /*
     * latchBuffer - called each time the screen is redrawn and returns whether
     * the visible regions need to be recomputed (this is a fairly heavy
     * operation, so this should be set only if needed). Typically this is used
     * to figure out if the content or size of a surface has changed.
     */
    virtual Region latchBuffer(bool& /*recomputeVisibleRegions*/, nsecs_t /*latchTime*/,
                               const sp<Fence>& /*releaseFence*/) {
        return {};
    }

    virtual bool isBufferLatched() const { return false; }

    /*
     * called with the state lock from a binder thread when the layer is
     * removed from the current list to the pending removal list
     */
    void onRemovedFromCurrentState();

    // Updates the transform hint in our SurfaceFlingerConsumer to match
    // the current orientation of the display device.
    void updateTransformHint(const sp<const DisplayDevice>& display) const;

    /*
     * returns the rectangle that crops the content of the layer and scales it
     * to the layer's size.
     */
    Rect getContentCrop() const;

    /*
     * Returns if a frame is ready
     */
    virtual bool hasReadyFrame() const { return false; }

    virtual int32_t getQueuedFrameCount() const { return 0; }

    // -----------------------------------------------------------------------

    bool createHwcLayer(HWComposer* hwc, DisplayId displayId);
    bool destroyHwcLayer(DisplayId displayId);
    void destroyAllHwcLayers();

    bool hasHwcLayer(DisplayId displayId) const { return getBE().mHwcLayers.count(displayId) > 0; }

    HWC2::Layer* getHwcLayer(DisplayId displayId) {
        if (!hasHwcLayer(displayId)) {
            return nullptr;
        }
        return getBE().mHwcLayers[displayId].layer.get();
    }

    bool setHwcLayer(DisplayId displayId) {
        if (!hasHwcLayer(displayId)) {
            return false;
        }
        getBE().compositionInfo.hwc.hwcLayer = getBE().mHwcLayers[displayId].layer;
        return true;
    }

    // -----------------------------------------------------------------------
    void clearWithOpenGL(const RenderArea& renderArea) const;

    inline const State& getDrawingState() const { return mDrawingState; }
    inline const State& getCurrentState() const { return mCurrentState; }
    inline State& getCurrentState() { return mCurrentState; }

    LayerDebugInfo getLayerDebugInfo() const;

    /* always call base class first */
    static void miniDumpHeader(String8& result);
    void miniDump(String8& result, DisplayId displayId) const;
    void dumpFrameStats(String8& result) const;
    void dumpFrameEvents(String8& result);
    void clearFrameStats();
    void logFrameStats();
    void getFrameStats(FrameStats* outStats) const;

    virtual std::vector<OccupancyTracker::Segment> getOccupancyHistory(bool /*forceFlush*/) {
        return {};
    }

    void onDisconnect();
    void addAndGetFrameTimestamps(const NewFrameEventsEntry* newEntry,
                                  FrameEventHistoryDelta* outDelta);

    virtual bool getTransformToDisplayInverse() const { return false; }

    ui::Transform getTransform() const;

    // Returns the Alpha of the Surface, accounting for the Alpha
    // of parent Surfaces in the hierarchy (alpha's will be multiplied
    // down the hierarchy).
    half getAlpha() const;
    half4 getColor() const;

    void traverseInReverseZOrder(LayerVector::StateSet stateSet,
                                 const LayerVector::Visitor& visitor);
    void traverseInZOrder(LayerVector::StateSet stateSet, const LayerVector::Visitor& visitor);

    /**
     * Traverse only children in z order, ignoring relative layers that are not children of the
     * parent.
     */
    void traverseChildrenInZOrder(LayerVector::StateSet stateSet,
                                  const LayerVector::Visitor& visitor);

    size_t getChildrenCount() const;
    void addChild(const sp<Layer>& layer);
    // Returns index if removed, or negative value otherwise
    // for symmetry with Vector::remove
    ssize_t removeChild(const sp<Layer>& layer);
    sp<Layer> getParent() const { return mCurrentParent.promote(); }
    bool hasParent() const { return getParent() != nullptr; }
    Rect computeScreenBounds() const;
    bool setChildLayer(const sp<Layer>& childLayer, int32_t z);
    bool setChildRelativeLayer(const sp<Layer>& childLayer,
            const sp<IBinder>& relativeToHandle, int32_t relativeZ);

    // Copy the current list of children to the drawing state. Called by
    // SurfaceFlinger to complete a transaction.
    void commitChildList();
    int32_t getZ() const;
    virtual void pushPendingState();

protected:
    // constant
    sp<SurfaceFlinger> mFlinger;
    /*
     * Trivial class, used to ensure that mFlinger->onLayerDestroyed(mLayer)
     * is called.
     */
    class LayerCleaner {
        sp<SurfaceFlinger> mFlinger;
        sp<Layer> mLayer;

    protected:
        ~LayerCleaner() {
            // destroy client resources
            mFlinger->onHandleDestroyed(mLayer);
        }

    public:
        LayerCleaner(const sp<SurfaceFlinger>& flinger, const sp<Layer>& layer)
              : mFlinger(flinger), mLayer(layer) {}
    };

    friend class impl::SurfaceInterceptor;

    // For unit tests
    friend class TestableSurfaceFlinger;

    void commitTransaction(const State& stateToCommit);

    uint32_t getEffectiveUsage(uint32_t usage) const;

    virtual FloatRect computeCrop(const sp<const DisplayDevice>& display) const;
    // Compute the initial crop as specified by parent layers and the
    // SurfaceControl for this layer. Does not include buffer crop from the
    // IGraphicBufferProducer client, as that should not affect child clipping.
    // Returns in screen space.
    Rect computeInitialCrop(const sp<const DisplayDevice>& display) const;

    // drawing
    void clearWithOpenGL(const RenderArea& renderArea, float r, float g, float b,
                         float alpha) const;

    void setParent(const sp<Layer>& layer);

    LayerVector makeTraversalList(LayerVector::StateSet stateSet, bool* outSkipRelativeZUsers);
    void addZOrderRelative(const wp<Layer>& relative);
    void removeZOrderRelative(const wp<Layer>& relative);

    class SyncPoint {
    public:
        explicit SyncPoint(uint64_t frameNumber)
              : mFrameNumber(frameNumber), mFrameIsAvailable(false), mTransactionIsApplied(false) {}

        uint64_t getFrameNumber() const { return mFrameNumber; }

        bool frameIsAvailable() const { return mFrameIsAvailable; }

        void setFrameAvailable() { mFrameIsAvailable = true; }

        bool transactionIsApplied() const { return mTransactionIsApplied; }

        void setTransactionApplied() { mTransactionIsApplied = true; }

    private:
        const uint64_t mFrameNumber;
        std::atomic<bool> mFrameIsAvailable;
        std::atomic<bool> mTransactionIsApplied;
    };

    // SyncPoints which will be signaled when the correct frame is at the head
    // of the queue and dropped after the frame has been latched. Protected by
    // mLocalSyncPointMutex.
    Mutex mLocalSyncPointMutex;
    std::list<std::shared_ptr<SyncPoint>> mLocalSyncPoints;

    // SyncPoints which will be signaled and then dropped when the transaction
    // is applied
    std::list<std::shared_ptr<SyncPoint>> mRemoteSyncPoints;

    // Returns false if the relevant frame has already been latched
    bool addSyncPoint(const std::shared_ptr<SyncPoint>& point);

    void popPendingState(State* stateToCommit);
    virtual bool applyPendingStates(State* stateToCommit);
    virtual uint32_t doTransactionResize(uint32_t flags, Layer::State* stateToCommit);

    void clearSyncPoints();

    // Returns mCurrentScaling mode (originating from the
    // Client) or mOverrideScalingMode mode (originating from
    // the Surface Controller) if set.
    virtual uint32_t getEffectiveScalingMode() const { return 0; }

public:
    /*
     * The layer handle is just a BBinder object passed to the client
     * (remote process) -- we don't keep any reference on our side such that
     * the dtor is called when the remote side let go of its reference.
     *
     * LayerCleaner ensures that mFlinger->onLayerDestroyed() is called for
     * this layer when the handle is destroyed.
     */
    class Handle : public BBinder, public LayerCleaner {
    public:
        Handle(const sp<SurfaceFlinger>& flinger, const sp<Layer>& layer)
              : LayerCleaner(flinger, layer), owner(layer) {}

        wp<Layer> owner;
    };

    sp<IBinder> getHandle();
    const String8& getName() const;
    virtual void notifyAvailableFrames() {}
    virtual PixelFormat getPixelFormat() const { return PIXEL_FORMAT_NONE; }
    bool getPremultipledAlpha() const;

    bool mPendingHWCDestroy{false};

protected:
    // -----------------------------------------------------------------------
    bool usingRelativeZ(LayerVector::StateSet stateSet);

    bool mPremultipliedAlpha{true};
    String8 mName;
    String8 mTransactionName; // A cached version of "TX - " + mName for systraces

    bool mPrimaryDisplayOnly = false;

    // these are protected by an external lock
    State mCurrentState;
    State mDrawingState;
    std::atomic<uint32_t> mTransactionFlags{0};

    // Accessed from main thread and binder threads
    Mutex mPendingStateMutex;
    Vector<State> mPendingStates;

    // Timestamp history for UIAutomation. Thread safe.
    FrameTracker mFrameTracker;

    // Timestamp history for the consumer to query.
    // Accessed by both consumer and producer on main and binder threads.
    Mutex mFrameEventHistoryMutex;
    ConsumerFrameEventHistory mFrameEventHistory;
    FenceTimeline mAcquireTimeline;
    FenceTimeline mReleaseTimeline;

    TimeStats& mTimeStats = TimeStats::getInstance();

    // main thread
    sp<GraphicBuffer> mActiveBuffer;
    ui::Dataspace mCurrentDataSpace = ui::Dataspace::UNKNOWN;
    Rect mCurrentCrop;
    uint32_t mCurrentTransform{0};
    // We encode unset as -1.
    int32_t mOverrideScalingMode{-1};
    std::atomic<uint64_t> mCurrentFrameNumber{0};
    bool mFrameLatencyNeeded{false};
    // Whether filtering is needed b/c of the drawingstate
    bool mNeedsFiltering{false};

    std::atomic<bool> mRemovedFromCurrentState{false};

    // page-flip thread (currently main thread)
    bool mProtectedByApp{false}; // application requires protected path to external sink

    // protected by mLock
    mutable Mutex mLock;

    const wp<Client> mClientRef;

    // This layer can be a cursor on some displays.
    bool mPotentialCursor{false};

    bool mFreezeGeometryUpdates{false};

    // Child list about to be committed/used for editing.
    LayerVector mCurrentChildren{LayerVector::StateSet::Current};
    // Child list used for rendering.
    LayerVector mDrawingChildren{LayerVector::StateSet::Drawing};

    wp<Layer> mCurrentParent;
    wp<Layer> mDrawingParent;

    mutable LayerBE mBE;

private:
    /**
     * Returns an unsorted vector of all layers that are part of this tree.
     * That includes the current layer and all its descendants.
     */
    std::vector<Layer*> getLayersInTree(LayerVector::StateSet stateSet);
    /**
     * Traverses layers that are part of this tree in the correct z order.
     * layersInTree must be sorted before calling this method.
     */
    void traverseChildrenInZOrderInner(const std::vector<Layer*>& layersInTree,
                                       LayerVector::StateSet stateSet,
                                       const LayerVector::Visitor& visitor);
    LayerVector makeChildrenTraversalList(LayerVector::StateSet stateSet,
                                          const std::vector<Layer*>& layersInTree);

    /**
     * Retuns the child bounds in layer space cropped to its bounds as well all its parent bounds.
     * The cropped bounds must be transformed back from parent layer space to child layer space by
     * applying the inverse of the child's transformation.
     */
    FloatRect cropChildBounds(const FloatRect& childBounds) const;

    /**
     * Returns the cropped buffer size or the layer crop if the layer has no buffer. Return
     * INVALID_RECT if the layer has no buffer and no crop.
     * A layer with an invalid buffer size and no crop is considered to be boundless. The layer
     * bounds are constrained by its parent bounds.
     */
    Rect getCroppedBufferSize(const Layer::State& s) const;

    /**
     * Returns active buffer size in the correct orientation. Buffer size is determined by undoing
     * any buffer transformations. If the layer has no buffer then return INVALID_RECT.
     */
    virtual Rect getBufferSize(const Layer::State&) const { return Rect::INVALID_RECT; }
};

} // namespace android

#define RETURN_IF_NO_HWC_LAYER(displayId, ...)                                         \
    do {                                                                               \
        if (!hasHwcLayer(displayId)) {                                                 \
            ALOGE("[%s] %s failed: no HWC layer found for display %s", mName.string(), \
                  __FUNCTION__, to_string(displayId).c_str());                         \
            return __VA_ARGS__;                                                        \
        }                                                                              \
    } while (false)

#endif // ANDROID_LAYER_H
