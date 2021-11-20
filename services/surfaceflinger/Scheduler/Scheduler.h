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

#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wextra"
#include <ui/GraphicTypes.h>
#pragma clang diagnostic pop // ignored "-Wconversion -Wextra"

#include <scheduler/Features.h>

#include "EventThread.h"
#include "LayerHistory.h"
#include "MessageQueue.h"
#include "OneShotTimer.h"
#include "RefreshRateConfigs.h"
#include "SchedulerUtils.h"
#include "VsyncSchedule.h"

namespace android {

class FenceTime;
class InjectVSyncSource;

namespace frametimeline {
class TokenManager;
} // namespace frametimeline

namespace scheduler {

struct ISchedulerCallback {
    // Indicates frame activity, i.e. whether commit and/or composite is taking place.
    enum class FrameHint { kNone, kActive };

    using RefreshRate = RefreshRateConfigs::RefreshRate;
    using DisplayModeEvent = scheduler::DisplayModeEvent;

    virtual void scheduleComposite(FrameHint) = 0;
    virtual void setVsyncEnabled(bool) = 0;
    virtual void changeRefreshRate(const RefreshRate&, DisplayModeEvent) = 0;
    virtual void kernelTimerChanged(bool expired) = 0;
    virtual void triggerOnFrameRateOverridesChanged() = 0;

protected:
    ~ISchedulerCallback() = default;
};

class Scheduler : impl::MessageQueue {
    using Impl = impl::MessageQueue;

public:
    Scheduler(ICompositor&, ISchedulerCallback&, FeatureFlags);
    ~Scheduler();

    void createVsyncSchedule(FeatureFlags);
    void startTimers();
    void run();

    using Impl::initVsync;
    using Impl::setInjector;

    using Impl::getScheduledFrameTime;
    using Impl::setDuration;

    using Impl::scheduleFrame;

    // Schedule an asynchronous or synchronous task on the main thread.
    template <typename F, typename T = std::invoke_result_t<F>>
    [[nodiscard]] std::future<T> schedule(F&& f) {
        auto [task, future] = makeTask(std::move(f));
        postMessage(std::move(task));
        return std::move(future);
    }

    ConnectionHandle createConnection(const char* connectionName, frametimeline::TokenManager*,
                                      std::chrono::nanoseconds workDuration,
                                      std::chrono::nanoseconds readyDuration,
                                      impl::EventThread::InterceptVSyncsCallback);

    sp<IDisplayEventConnection> createDisplayEventConnection(
            ConnectionHandle, ISurfaceComposer::EventRegistrationFlags eventRegistration = {});

    sp<EventThreadConnection> getEventConnection(ConnectionHandle);

    void onHotplugReceived(ConnectionHandle, PhysicalDisplayId, bool connected);
    void onPrimaryDisplayModeChanged(ConnectionHandle, DisplayModePtr) EXCLUDES(mPolicyLock);
    void onNonPrimaryDisplayModeChanged(ConnectionHandle, DisplayModePtr);
    void onScreenAcquired(ConnectionHandle);
    void onScreenReleased(ConnectionHandle);

    void onFrameRateOverridesChanged(ConnectionHandle, PhysicalDisplayId)
            EXCLUDES(mFrameRateOverridesLock) EXCLUDES(mConnectionsLock);

    // Modifies work duration in the event thread.
    void setDuration(ConnectionHandle, std::chrono::nanoseconds workDuration,
                     std::chrono::nanoseconds readyDuration);

    DisplayStatInfo getDisplayStatInfo(nsecs_t now);

    // Returns injector handle if injection has toggled, or an invalid handle otherwise.
    ConnectionHandle enableVSyncInjection(bool enable);
    // Returns false if injection is disabled.
    bool injectVSync(nsecs_t when, nsecs_t expectedVSyncTime, nsecs_t deadlineTimestamp);
    void enableHardwareVsync();
    void disableHardwareVsync(bool makeUnavailable);

    // Resyncs the scheduler to hardware vsync.
    // If makeAvailable is true, then hardware vsync will be turned on.
    // Otherwise, if hardware vsync is not already enabled then this method will
    // no-op.
    // The period is the vsync period from the current display configuration.
    void resyncToHardwareVsync(bool makeAvailable, nsecs_t period);
    void resync() EXCLUDES(mRefreshRateConfigsLock);

    // Passes a vsync sample to VsyncController. periodFlushed will be true if
    // VsyncController detected that the vsync period changed, and false otherwise.
    void addResyncSample(nsecs_t timestamp, std::optional<nsecs_t> hwcVsyncPeriod,
                         bool* periodFlushed);
    void addPresentFence(std::shared_ptr<FenceTime>);

    // Layers are registered on creation, and unregistered when the weak reference expires.
    void registerLayer(Layer*);
    void recordLayerHistory(Layer*, nsecs_t presentTime, LayerHistory::LayerUpdateType updateType)
            EXCLUDES(mRefreshRateConfigsLock);
    void setModeChangePending(bool pending);
    void deregisterLayer(Layer*);

    // Detects content using layer history, and selects a matching refresh rate.
    void chooseRefreshRateForContent() EXCLUDES(mRefreshRateConfigsLock);

    void resetIdleTimer();

    // Indicates that touch interaction is taking place.
    void onTouchHint();

    void setDisplayPowerState(bool normal);

    VSyncDispatch& getVsyncDispatch() { return mVsyncSchedule->getDispatch(); }

    // Returns true if a given vsync timestamp is considered valid vsync
    // for a given uid
    bool isVsyncValid(nsecs_t expectedVsyncTimestamp, uid_t uid) const
            EXCLUDES(mFrameRateOverridesLock);

    std::chrono::steady_clock::time_point getPreviousVsyncFrom(nsecs_t expectedPresentTime) const;

    void dump(std::string&) const;
    void dump(ConnectionHandle, std::string&) const;
    void dumpVsync(std::string&) const;

    // Get the appropriate refresh for current conditions.
    DisplayModePtr getPreferredDisplayMode();

    // Notifies the scheduler about a refresh rate timeline change.
    void onNewVsyncPeriodChangeTimeline(const hal::VsyncPeriodChangeTimeline& timeline);

    // Notifies the scheduler post composition.
    void onPostComposition(nsecs_t presentTime);

    // Notifies the scheduler when the display size has changed. Called from SF's main thread
    void onActiveDisplayAreaChanged(uint32_t displayArea);

    size_t getEventThreadConnectionCount(ConnectionHandle handle);

    std::unique_ptr<VSyncSource> makePrimaryDispSyncSource(const char* name,
                                                           std::chrono::nanoseconds workDuration,
                                                           std::chrono::nanoseconds readyDuration,
                                                           bool traceVsync = true);

    // Stores the preferred refresh rate that an app should run at.
    // FrameRateOverride.refreshRateHz == 0 means no preference.
    void setPreferredRefreshRateForUid(FrameRateOverride) EXCLUDES(mFrameRateOverridesLock);
    // Retrieves the overridden refresh rate for a given uid.
    std::optional<Fps> getFrameRateOverride(uid_t uid) const
            EXCLUDES(mRefreshRateConfigsLock, mFrameRateOverridesLock);

    void setRefreshRateConfigs(std::shared_ptr<RefreshRateConfigs> refreshRateConfigs)
            EXCLUDES(mRefreshRateConfigsLock) {
        // We need to stop the idle timer on the previous RefreshRateConfigs instance
        // and cleanup the scheduler's state before we switch to the other RefreshRateConfigs.
        {
            std::scoped_lock lock(mRefreshRateConfigsLock);
            if (mRefreshRateConfigs) mRefreshRateConfigs->stopIdleTimer();
        }
        {
            std::scoped_lock lock(mPolicyLock);
            mPolicy = {};
        }
        {
            std::scoped_lock lock(mRefreshRateConfigsLock);
            mRefreshRateConfigs = std::move(refreshRateConfigs);
            mRefreshRateConfigs->setIdleTimerCallbacks(
                    [this] { std::invoke(&Scheduler::idleTimerCallback, this, TimerState::Reset); },
                    [this] {
                        std::invoke(&Scheduler::idleTimerCallback, this, TimerState::Expired);
                    },
                    [this] {
                        std::invoke(&Scheduler::kernelIdleTimerCallback, this, TimerState::Reset);
                    },
                    [this] {
                        std::invoke(&Scheduler::kernelIdleTimerCallback, this, TimerState::Expired);
                    });
            mRefreshRateConfigs->startIdleTimer();
        }
    }

    nsecs_t getVsyncPeriodFromRefreshRateConfigs() const EXCLUDES(mRefreshRateConfigsLock) {
        std::scoped_lock lock(mRefreshRateConfigsLock);
        return mRefreshRateConfigs->getCurrentRefreshRate().getVsyncPeriod();
    }

private:
    friend class TestableScheduler;

    using FrameHint = ISchedulerCallback::FrameHint;

    enum class ContentDetectionState { Off, On };
    enum class TimerState { Reset, Expired };
    enum class TouchState { Inactive, Active };

    // Create a connection on the given EventThread.
    ConnectionHandle createConnection(std::unique_ptr<EventThread>);
    sp<EventThreadConnection> createConnectionInternal(
            EventThread*, ISurfaceComposer::EventRegistrationFlags eventRegistration = {});

    // Update feature state machine to given state when corresponding timer resets or expires.
    void kernelIdleTimerCallback(TimerState) EXCLUDES(mRefreshRateConfigsLock);
    void idleTimerCallback(TimerState);
    void touchTimerCallback(TimerState);
    void displayPowerTimerCallback(TimerState);

    // handles various timer features to change the refresh rate.
    template <class T>
    bool handleTimerStateChanged(T* currentState, T newState);

    void setVsyncPeriod(nsecs_t period);

    // This function checks whether individual features that are affecting the refresh rate
    // selection were initialized, prioritizes them, and calculates the DisplayModeId
    // for the suggested refresh rate.
    DisplayModePtr calculateRefreshRateModeId(
            RefreshRateConfigs::GlobalSignals* consideredSignals = nullptr) REQUIRES(mPolicyLock);

    void dispatchCachedReportedMode() REQUIRES(mPolicyLock) EXCLUDES(mRefreshRateConfigsLock);
    bool updateFrameRateOverrides(RefreshRateConfigs::GlobalSignals, Fps displayRefreshRate)
            REQUIRES(mPolicyLock) EXCLUDES(mFrameRateOverridesLock);

    impl::EventThread::ThrottleVsyncCallback makeThrottleVsyncCallback() const
            EXCLUDES(mRefreshRateConfigsLock);
    impl::EventThread::GetVsyncPeriodFunction makeGetVsyncPeriodFunction() const;

    std::shared_ptr<RefreshRateConfigs> holdRefreshRateConfigs() const
            EXCLUDES(mRefreshRateConfigsLock) {
        std::scoped_lock lock(mRefreshRateConfigsLock);
        return mRefreshRateConfigs;
    }

    // Stores EventThread associated with a given VSyncSource, and an initial EventThreadConnection.
    struct Connection {
        sp<EventThreadConnection> connection;
        std::unique_ptr<EventThread> thread;
    };

    ConnectionHandle::Id mNextConnectionHandleId = 0;
    mutable std::mutex mConnectionsLock;
    std::unordered_map<ConnectionHandle, Connection> mConnections GUARDED_BY(mConnectionsLock);

    bool mInjectVSyncs = false;
    InjectVSyncSource* mVSyncInjector = nullptr;
    ConnectionHandle mInjectorConnectionHandle;

    mutable std::mutex mHWVsyncLock;
    bool mPrimaryHWVsyncEnabled GUARDED_BY(mHWVsyncLock) = false;
    bool mHWVsyncAvailable GUARDED_BY(mHWVsyncLock) = false;

    std::atomic<nsecs_t> mLastResyncTime = 0;

    const FeatureFlags mFeatures;
    std::optional<VsyncSchedule> mVsyncSchedule;

    // Used to choose refresh rate if content detection is enabled.
    LayerHistory mLayerHistory;

    // Timer used to monitor touch events.
    std::optional<OneShotTimer> mTouchTimer;
    // Timer used to monitor display power mode.
    std::optional<OneShotTimer> mDisplayPowerTimer;

    ISchedulerCallback& mSchedulerCallback;

    mutable std::mutex mPolicyLock;

    struct {
        // Policy for choosing the display mode.
        LayerHistory::Summary contentRequirements;
        TimerState idleTimer = TimerState::Reset;
        TouchState touch = TouchState::Inactive;
        TimerState displayPowerTimer = TimerState::Expired;
        bool isDisplayPowerStateNormal = true;

        // Chosen display mode.
        DisplayModePtr mode;

        struct ModeChangedParams {
            ConnectionHandle handle;
            DisplayModePtr mode;
        };

        // Parameters for latest dispatch of mode change event.
        std::optional<ModeChangedParams> cachedModeChangedParams;
    } mPolicy GUARDED_BY(mPolicyLock);

    mutable std::mutex mRefreshRateConfigsLock;
    std::shared_ptr<RefreshRateConfigs> mRefreshRateConfigs GUARDED_BY(mRefreshRateConfigsLock);

    std::mutex mVsyncTimelineLock;
    std::optional<hal::VsyncPeriodChangeTimeline> mLastVsyncPeriodChangeTimeline
            GUARDED_BY(mVsyncTimelineLock);
    static constexpr std::chrono::nanoseconds MAX_VSYNC_APPLIED_TIME = 200ms;

    // The frame rate override lists need their own mutex as they are being read
    // by SurfaceFlinger, Scheduler and EventThread (as a callback) to prevent deadlocks
    mutable std::mutex mFrameRateOverridesLock;

    // mappings between a UID and a preferred refresh rate that this app would
    // run at.
    RefreshRateConfigs::UidToFrameRateOverride mFrameRateOverridesByContent
            GUARDED_BY(mFrameRateOverridesLock);
    RefreshRateConfigs::UidToFrameRateOverride mFrameRateOverridesFromBackdoor
            GUARDED_BY(mFrameRateOverridesLock);

    // Keeps track of whether the screen is acquired for debug
    std::atomic<bool> mScreenAcquired = false;
};

} // namespace scheduler
} // namespace android
