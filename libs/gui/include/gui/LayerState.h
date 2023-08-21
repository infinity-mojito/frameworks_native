/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef ANDROID_SF_LAYER_STATE_H
#define ANDROID_SF_LAYER_STATE_H


#include <stdint.h>
#include <sys/types.h>

#include <android/gui/IWindowInfosReportedListener.h>
#include <android/gui/TrustedPresentationThresholds.h>
#include <android/native_window.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/ITransactionCompletedListener.h>
#include <math/mat4.h>

#include <android/gui/DropInputMode.h>
#include <android/gui/FocusRequest.h>

#include <ftl/flags.h>
#include <gui/DisplayCaptureArgs.h>
#include <gui/ISurfaceComposer.h>
#include <gui/LayerCaptureArgs.h>
#include <gui/LayerMetadata.h>
#include <gui/SpHash.h>
#include <gui/SurfaceControl.h>
#include <gui/WindowInfo.h>
#include <math/vec3.h>
#include <ui/BlurRegion.h>
#include <ui/GraphicTypes.h>
#include <ui/LayerStack.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <ui/Rotation.h>
#include <ui/StretchEffect.h>
#include <ui/Transform.h>
#include <utils/Errors.h>

namespace android {

class Parcel;

using gui::ISurfaceComposerClient;
using gui::LayerMetadata;

using gui::TrustedPresentationThresholds;

struct client_cache_t {
    wp<IBinder> token = nullptr;
    uint64_t id;

    bool operator==(const client_cache_t& other) const { return id == other.id; }

    bool isValid() const { return token != nullptr; }
};

class TrustedPresentationListener : public Parcelable {
public:
    sp<ITransactionCompletedListener> callbackInterface;
    int callbackId = -1;

    void invoke(bool presentedWithinThresholds) {
        callbackInterface->onTrustedPresentationChanged(callbackId, presentedWithinThresholds);
    }

    status_t writeToParcel(Parcel* parcel) const;
    status_t readFromParcel(const Parcel* parcel);
};

class BufferData : public Parcelable {
public:
    virtual ~BufferData() = default;
    virtual bool hasBuffer() const { return buffer != nullptr; }
    virtual bool hasSameBuffer(const BufferData& other) const {
        return buffer == other.buffer && frameNumber == other.frameNumber;
    }
    virtual uint32_t getWidth() const { return buffer->getWidth(); }
    virtual uint32_t getHeight() const { return buffer->getHeight(); }
    Rect getBounds() const {
        return {0, 0, static_cast<int32_t>(getWidth()), static_cast<int32_t>(getHeight())};
    }
    virtual uint64_t getId() const { return buffer->getId(); }
    virtual PixelFormat getPixelFormat() const { return buffer->getPixelFormat(); }
    virtual uint64_t getUsage() const { return buffer->getUsage(); }

    enum class BufferDataChange : uint32_t {
        fenceChanged = 0x01,
        frameNumberChanged = 0x02,
        cachedBufferChanged = 0x04,
    };

    sp<GraphicBuffer> buffer;
    sp<Fence> acquireFence;

    // Used by BlastBufferQueue to forward the framenumber generated by the
    // graphics producer.
    uint64_t frameNumber = 0;
    bool hasBarrier = false;
    uint64_t barrierFrameNumber = 0;
    uint32_t producerId = 0;

    // Listens to when the buffer is safe to be released. This is used for blast
    // layers only. The callback includes a release fence as well as the graphic
    // buffer id to identify the buffer.
    sp<ITransactionCompletedListener> releaseBufferListener = nullptr;

    // Stores which endpoint the release information should be sent to. We don't want to send the
    // releaseCallbackId and release fence to all listeners so we store which listener the setBuffer
    // was called with.
    sp<IBinder> releaseBufferEndpoint;

    ftl::Flags<BufferDataChange> flags;

    client_cache_t cachedBuffer;

    // Generates the release callback id based on the buffer id and frame number.
    // This is used as an identifier when release callbacks are invoked.
    ReleaseCallbackId generateReleaseCallbackId() const;

    status_t writeToParcel(Parcel* parcel) const override;
    status_t readFromParcel(const Parcel* parcel) override;
};

/*
 * Used to communicate layer information between SurfaceFlinger and its clients.
 */
struct layer_state_t {
    enum Permission {
        ACCESS_SURFACE_FLINGER = 0x1,
        ROTATE_SURFACE_FLINGER = 0x2,
        INTERNAL_SYSTEM_WINDOW = 0x4,
    };

    enum {
        eLayerHidden = 0x01,         // SURFACE_HIDDEN in SurfaceControl.java
        eLayerOpaque = 0x02,         // SURFACE_OPAQUE
        eLayerSkipScreenshot = 0x40, // SKIP_SCREENSHOT
        eLayerSecure = 0x80,         // SECURE
        // Queue up layer buffers instead of dropping the oldest buffer when this flag is
        // set. This blocks the client until all the buffers have been presented. If the buffers
        // have presentation timestamps, then we may drop buffers.
        eEnableBackpressure = 0x100,       // ENABLE_BACKPRESSURE
        eLayerIsDisplayDecoration = 0x200, // DISPLAY_DECORATION
        // Ignore any destination frame set on the layer. This is used when the buffer scaling mode
        // is freeze and the destination frame is applied asynchronously with the buffer submission.
        // This is needed to maintain compatibility for SurfaceView scaling behavior.
        // See SurfaceView scaling behavior for more details.
        eIgnoreDestinationFrame = 0x400,
        eLayerIsRefreshRateIndicator = 0x800, // REFRESH_RATE_INDICATOR
    };

    enum {
        ePositionChanged = 0x00000001,
        eLayerChanged = 0x00000002,
        eTrustedPresentationInfoChanged = 0x00000004,
        eAlphaChanged = 0x00000008,
        eMatrixChanged = 0x00000010,
        eTransparentRegionChanged = 0x00000020,
        eFlagsChanged = 0x00000040,
        eLayerStackChanged = 0x00000080,
        eFlushJankData = 0x00000100,
        eCachingHintChanged = 0x00000200,
        eDimmingEnabledChanged = 0x00000400,
        eShadowRadiusChanged = 0x00000800,
        eRenderBorderChanged = 0x00001000,
        eBufferCropChanged = 0x00002000,
        eRelativeLayerChanged = 0x00004000,
        eReparent = 0x00008000,
        eColorChanged = 0x00010000,
        /* unused = 0x00020000, */
        eBufferTransformChanged = 0x00040000,
        eTransformToDisplayInverseChanged = 0x00080000,
        eCropChanged = 0x00100000,
        eBufferChanged = 0x00200000,
        eDefaultFrameRateCompatibilityChanged = 0x00400000,
        eDataspaceChanged = 0x00800000,
        eHdrMetadataChanged = 0x01000000,
        eSurfaceDamageRegionChanged = 0x02000000,
        eApiChanged = 0x04000000,
        eSidebandStreamChanged = 0x08000000,
        eColorTransformChanged = 0x10000000,
        eHasListenerCallbacksChanged = 0x20000000,
        eInputInfoChanged = 0x40000000,
        eCornerRadiusChanged = 0x80000000,
        eDestinationFrameChanged = 0x1'00000000,
        /* unused = 0x2'00000000, */
        eBackgroundColorChanged = 0x4'00000000,
        eMetadataChanged = 0x8'00000000,
        eColorSpaceAgnosticChanged = 0x10'00000000,
        eFrameRateSelectionPriority = 0x20'00000000,
        eFrameRateChanged = 0x40'00000000,
        eBackgroundBlurRadiusChanged = 0x80'00000000,
        eProducerDisconnect = 0x100'00000000,
        eFixedTransformHintChanged = 0x200'00000000,
        /* unused 0x400'00000000, */
        eBlurRegionsChanged = 0x800'00000000,
        eAutoRefreshChanged = 0x1000'00000000,
        eStretchChanged = 0x2000'00000000,
        eTrustedOverlayChanged = 0x4000'00000000,
        eDropInputModeChanged = 0x8000'00000000,
        eExtendedRangeBrightnessChanged = 0x10000'00000000,

    };

    layer_state_t();

    void merge(const layer_state_t& other);
    status_t write(Parcel& output) const;
    status_t read(const Parcel& input);
    // Compares two layer_state_t structs and returns a set of change flags describing all the
    // states that are different.
    uint64_t diff(const layer_state_t& other) const;
    bool hasBufferChanges() const;

    // Layer hierarchy updates.
    static constexpr uint64_t HIERARCHY_CHANGES = layer_state_t::eLayerChanged |
            layer_state_t::eRelativeLayerChanged | layer_state_t::eReparent |
            layer_state_t::eLayerStackChanged;

    // Geometry updates.
    static constexpr uint64_t GEOMETRY_CHANGES = layer_state_t::eBufferCropChanged |
            layer_state_t::eBufferTransformChanged | layer_state_t::eCornerRadiusChanged |
            layer_state_t::eCropChanged | layer_state_t::eDestinationFrameChanged |
            layer_state_t::eMatrixChanged | layer_state_t::ePositionChanged |
            layer_state_t::eTransformToDisplayInverseChanged |
            layer_state_t::eTransparentRegionChanged;

    // Buffer and related updates.
    static constexpr uint64_t BUFFER_CHANGES = layer_state_t::eApiChanged |
            layer_state_t::eBufferChanged | layer_state_t::eBufferCropChanged |
            layer_state_t::eBufferTransformChanged | layer_state_t::eDataspaceChanged |
            layer_state_t::eSidebandStreamChanged | layer_state_t::eSurfaceDamageRegionChanged |
            layer_state_t::eTransformToDisplayInverseChanged |
            layer_state_t::eTransparentRegionChanged |
            layer_state_t::eExtendedRangeBrightnessChanged;

    // Content updates.
    static constexpr uint64_t CONTENT_CHANGES = layer_state_t::BUFFER_CHANGES |
            layer_state_t::eAlphaChanged | layer_state_t::eAutoRefreshChanged |
            layer_state_t::eBackgroundBlurRadiusChanged | layer_state_t::eBackgroundColorChanged |
            layer_state_t::eBlurRegionsChanged | layer_state_t::eColorChanged |
            layer_state_t::eColorSpaceAgnosticChanged | layer_state_t::eColorTransformChanged |
            layer_state_t::eCornerRadiusChanged | layer_state_t::eDimmingEnabledChanged |
            layer_state_t::eHdrMetadataChanged | layer_state_t::eRenderBorderChanged |
            layer_state_t::eShadowRadiusChanged | layer_state_t::eStretchChanged;

    // Changes which invalidates the layer's visible region in CE.
    static constexpr uint64_t CONTENT_DIRTY = layer_state_t::CONTENT_CHANGES |
            layer_state_t::GEOMETRY_CHANGES | layer_state_t::HIERARCHY_CHANGES;

    // Changes affecting child states.
    static constexpr uint64_t AFFECTS_CHILDREN = layer_state_t::GEOMETRY_CHANGES |
            layer_state_t::HIERARCHY_CHANGES | layer_state_t::eAlphaChanged |
            layer_state_t::eBackgroundBlurRadiusChanged | layer_state_t::eBlurRegionsChanged |
            layer_state_t::eColorTransformChanged | layer_state_t::eCornerRadiusChanged |
            layer_state_t::eFlagsChanged | layer_state_t::eTrustedOverlayChanged |
            layer_state_t::eFrameRateChanged | layer_state_t::eFrameRateSelectionPriority |
            layer_state_t::eFixedTransformHintChanged;

    // Changes affecting data sent to input.
    static constexpr uint64_t INPUT_CHANGES = layer_state_t::eInputInfoChanged |
            layer_state_t::eDropInputModeChanged | layer_state_t::eTrustedOverlayChanged |
            layer_state_t::eLayerStackChanged;

    // Changes that affect the visible region on a display.
    static constexpr uint64_t VISIBLE_REGION_CHANGES =
            layer_state_t::GEOMETRY_CHANGES | layer_state_t::HIERARCHY_CHANGES;

    bool hasValidBuffer() const;
    void sanitize(int32_t permissions);

    struct matrix22_t {
        float dsdx{0};
        float dtdx{0};
        float dtdy{0};
        float dsdy{0};
        status_t write(Parcel& output) const;
        status_t read(const Parcel& input);
        inline bool operator==(const matrix22_t& other) const {
            return std::tie(dsdx, dtdx, dtdy, dsdy) ==
                    std::tie(other.dsdx, other.dtdx, other.dtdy, other.dsdy);
        }
        inline bool operator!=(const matrix22_t& other) const { return !(*this == other); }
    };
    sp<IBinder> surface;
    int32_t layerId;
    uint64_t what;
    float x;
    float y;
    int32_t z;
    ui::LayerStack layerStack = ui::DEFAULT_LAYER_STACK;
    uint32_t flags;
    uint32_t mask;
    uint8_t reserved;
    matrix22_t matrix;
    float cornerRadius;
    uint32_t backgroundBlurRadius;

    sp<SurfaceControl> relativeLayerSurfaceControl;

    sp<SurfaceControl> parentSurfaceControlForChild;

    half4 color;

    // non POD must be last. see write/read
    Region transparentRegion;
    uint32_t bufferTransform;
    bool transformToDisplayInverse;
    Rect crop;
    std::shared_ptr<BufferData> bufferData = nullptr;
    ui::Dataspace dataspace;
    HdrMetadata hdrMetadata;
    Region surfaceDamageRegion;
    int32_t api;
    sp<NativeHandle> sidebandStream;
    mat4 colorTransform;
    std::vector<BlurRegion> blurRegions;

    sp<gui::WindowInfoHandle> windowInfoHandle = sp<gui::WindowInfoHandle>::make();

    LayerMetadata metadata;

    // The following refer to the alpha, and dataspace, respectively of
    // the background color layer
    half4 bgColor;
    ui::Dataspace bgColorDataspace;

    // A color space agnostic layer means the color of this layer can be
    // interpreted in any color space.
    bool colorSpaceAgnostic;

    std::vector<ListenerCallbacks> listeners;

    // Draws a shadow around the surface.
    float shadowRadius;

    // Priority of the layer assigned by Window Manager.
    int32_t frameRateSelectionPriority;

    // Layer frame rate and compatibility. See ANativeWindow_setFrameRate().
    float frameRate;
    int8_t frameRateCompatibility;
    int8_t changeFrameRateStrategy;

    // Default frame rate compatibility used to set the layer refresh rate votetype.
    int8_t defaultFrameRateCompatibility;

    // Set by window manager indicating the layer and all its children are
    // in a different orientation than the display. The hint suggests that
    // the graphic producers should receive a transform hint as if the
    // display was in this orientation. When the display changes to match
    // the layer orientation, the graphic producer may not need to allocate
    // a buffer of a different size. -1 means the transform hint is not set,
    // otherwise the value will be a valid ui::Rotation.
    ui::Transform::RotationFlags fixedTransformHint;

    // Indicates that the consumer should acquire the next frame as soon as it
    // can and not wait for a frame to become available. This is only relevant
    // in shared buffer mode.
    bool autoRefresh;

    // An inherited state that indicates that this surface control and its children
    // should be trusted for input occlusion detection purposes
    bool isTrustedOverlay;

    // Flag to indicate if border needs to be enabled on the layer
    bool borderEnabled;
    float borderWidth;
    half4 borderColor;

    // Stretch effect to be applied to this layer
    StretchEffect stretchEffect;

    Rect bufferCrop;
    Rect destinationFrame;

    // Force inputflinger to drop all input events for the layer and its children.
    gui::DropInputMode dropInputMode;

    bool dimmingEnabled;
    float currentHdrSdrRatio = 1.f;
    float desiredHdrSdrRatio = 1.f;

    gui::CachingHint cachingHint = gui::CachingHint::Enabled;

    TrustedPresentationThresholds trustedPresentationThresholds;
    TrustedPresentationListener trustedPresentationListener;
};

class ComposerState {
public:
    layer_state_t state;
    status_t write(Parcel& output) const;
    status_t read(const Parcel& input);
};

struct DisplayState {
    enum {
        eSurfaceChanged = 0x01,
        eLayerStackChanged = 0x02,
        eDisplayProjectionChanged = 0x04,
        eDisplaySizeChanged = 0x08,
        eFlagsChanged = 0x10
    };

    DisplayState();
    void merge(const DisplayState& other);
    void sanitize(int32_t permissions);

    uint32_t what = 0;
    uint32_t flags = 0;
    sp<IBinder> token;
    sp<IGraphicBufferProducer> surface;

    ui::LayerStack layerStack = ui::DEFAULT_LAYER_STACK;

    // These states define how layers are projected onto the physical display.
    //
    // Layers are first clipped to `layerStackSpaceRect'.  They are then translated and
    // scaled from `layerStackSpaceRect' to `orientedDisplaySpaceRect'.  Finally, they are rotated
    // according to `orientation', `width', and `height'.
    //
    // For example, assume layerStackSpaceRect is Rect(0, 0, 200, 100), orientedDisplaySpaceRect is
    // Rect(20, 10, 420, 210), and the size of the display is WxH.  When orientation is 0, layers
    // will be scaled by a factor of 2 and translated by (20, 10). When orientation is 1, layers
    // will be additionally rotated by 90 degrees around the origin clockwise and translated by (W,
    // 0).
    ui::Rotation orientation = ui::ROTATION_0;
    Rect layerStackSpaceRect = Rect::EMPTY_RECT;
    Rect orientedDisplaySpaceRect = Rect::EMPTY_RECT;

    uint32_t width = 0;
    uint32_t height = 0;

    status_t write(Parcel& output) const;
    status_t read(const Parcel& input);
};

struct InputWindowCommands {
    std::vector<gui::FocusRequest> focusRequests;
    std::unordered_set<sp<gui::IWindowInfosReportedListener>,
                       SpHash<gui::IWindowInfosReportedListener>>
            windowInfosReportedListeners;

    // Merges the passed in commands and returns true if there were any changes.
    bool merge(const InputWindowCommands& other);
    bool empty() const;
    void clear();
    status_t write(Parcel& output) const;
    status_t read(const Parcel& input);
};

static inline int compare_type(const ComposerState& lhs, const ComposerState& rhs) {
    if (lhs.state.surface < rhs.state.surface) return -1;
    if (lhs.state.surface > rhs.state.surface) return 1;
    return 0;
}

static inline int compare_type(const DisplayState& lhs, const DisplayState& rhs) {
    return compare_type(lhs.token, rhs.token);
}

// Returns true if the frameRate is valid.
//
// @param frameRate the frame rate in Hz
// @param compatibility a ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_*
// @param changeFrameRateStrategy a ANATIVEWINDOW_CHANGE_FRAME_RATE_*
// @param functionName calling function or nullptr. Used for logging
// @param privileged whether caller has unscoped surfaceflinger access
bool ValidateFrameRate(float frameRate, int8_t compatibility, int8_t changeFrameRateStrategy,
                       const char* functionName, bool privileged = false);

}; // namespace android

#endif // ANDROID_SF_LAYER_STATE_H
