/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include <input/Input.h>
#include <utils/BitSet.h>
#include <utils/Timers.h>
#include <map>
#include <set>

namespace android {

class VelocityTrackerStrategy;

/*
 * Calculates the velocity of pointer movements over time.
 */
class VelocityTracker {
public:
    enum class Strategy : int32_t {
        DEFAULT = -1,
        MIN = 0,
        IMPULSE = 0,
        LSQ1 = 1,
        LSQ2 = 2,
        LSQ3 = 3,
        WLSQ2_DELTA = 4,
        WLSQ2_CENTRAL = 5,
        WLSQ2_RECENT = 6,
        INT1 = 7,
        INT2 = 8,
        LEGACY = 9,
        MAX = LEGACY,
    };

    struct Estimator {
        static const size_t MAX_DEGREE = 4;

        // Estimator time base.
        nsecs_t time;

        // Polynomial coefficients describing motion.
        float coeff[MAX_DEGREE + 1];

        // Polynomial degree (number of coefficients), or zero if no information is
        // available.
        uint32_t degree;

        // Confidence (coefficient of determination), between 0 (no fit) and 1 (perfect fit).
        float confidence;

        inline void clear() {
            time = 0;
            degree = 0;
            confidence = 0;
            for (size_t i = 0; i <= MAX_DEGREE; i++) {
                coeff[i] = 0;
            }
        }
    };

    /*
     * Contains all available velocity data from a VelocityTracker.
     */
    struct ComputedVelocity {
        inline std::optional<float> getVelocity(int32_t axis, uint32_t id) const {
            const auto& axisVelocities = mVelocities.find(axis);
            if (axisVelocities == mVelocities.end()) {
                return {};
            }

            const auto& axisIdVelocity = axisVelocities->second.find(id);
            if (axisIdVelocity == axisVelocities->second.end()) {
                return {};
            }

            return axisIdVelocity->second;
        }

        inline void addVelocity(int32_t axis, uint32_t id, float velocity) {
            mVelocities[axis][id] = velocity;
        }

    private:
        std::map<int32_t /*axis*/, std::map<int32_t /*pointerId*/, float /*velocity*/>> mVelocities;
    };

    // Creates a velocity tracker using the specified strategy for each supported axis.
    // If strategy is not provided, uses the default strategy for the platform.
    // TODO(b/32830165): support axis-specific strategies.
    VelocityTracker(const Strategy strategy = Strategy::DEFAULT);

    ~VelocityTracker();

    /** Return true if the axis is supported for velocity tracking, false otherwise. */
    static bool isAxisSupported(int32_t axis);

    // Resets the velocity tracker state.
    void clear();

    // Resets the velocity tracker state for specific pointers.
    // Call this method when some pointers have changed and may be reusing
    // an id that was assigned to a different pointer earlier.
    void clearPointers(BitSet32 idBits);

    // Adds movement information for a set of pointers.
    // The idBits bitfield specifies the pointer ids of the pointers whose data points
    // are included in the movement.
    // The positions map contains a mapping of an axis to positions array.
    // The positions arrays contain information for each pointer in order by increasing id.
    // Each array's size should be equal to the number of one bits in idBits.
    void addMovement(nsecs_t eventTime, BitSet32 idBits,
                     const std::map<int32_t, std::vector<float>>& positions);

    // Adds movement information for all pointers in a MotionEvent, including historical samples.
    void addMovement(const MotionEvent* event);

    // Returns the velocity of the specified pointer id and axis in position units per second.
    // Returns empty optional if there is insufficient movement information for the pointer, or if
    // the given axis is not supported for velocity tracking.
    std::optional<float> getVelocity(int32_t axis, uint32_t id) const;

    // Returns a ComputedVelocity instance with all available velocity data, using the given units
    // (reference: units == 1 means "per millisecond"), and clamping each velocity between
    // [-maxVelocity, maxVelocity], inclusive.
    ComputedVelocity getComputedVelocity(int32_t units, float maxVelocity);

    // Gets an estimator for the recent movements of the specified pointer id for the given axis.
    // Returns false and clears the estimator if there is no information available
    // about the pointer.
    bool getEstimator(int32_t axis, uint32_t id, Estimator* outEstimator) const;

    // Gets the active pointer id, or -1 if none.
    inline int32_t getActivePointerId() const { return mActivePointerId; }

private:
    nsecs_t mLastEventTime;
    BitSet32 mCurrentPointerIdBits;
    int32_t mActivePointerId;

    // An override strategy passed in the constructor to be used for all axes.
    // This strategy will apply to all axes, unless the default strategy is specified here.
    // When default strategy is specified, then each axis will use a potentially different strategy
    // based on a hardcoded mapping.
    const Strategy mOverrideStrategy;
    // Maps axes to their respective VelocityTrackerStrategy instances.
    // Note that, only axes that have had MotionEvents (and not all supported axes) will be here.
    std::map<int32_t /*axis*/, std::unique_ptr<VelocityTrackerStrategy>> mConfiguredStrategies;

    void configureStrategy(int32_t axis);

    // Generates a VelocityTrackerStrategy instance for the given Strategy type.
    // The `deltaValues` parameter indicates whether or not the created strategy should treat motion
    // values as deltas (and not as absolute values). This the parameter is applicable only for
    // strategies that support differential axes.
    static std::unique_ptr<VelocityTrackerStrategy> createStrategy(const Strategy strategy,
                                                                   bool deltaValues);
};


/*
 * Implements a particular velocity tracker algorithm.
 */
class VelocityTrackerStrategy {
protected:
    VelocityTrackerStrategy() { }

public:
    virtual ~VelocityTrackerStrategy() { }

    virtual void clearPointers(BitSet32 idBits) = 0;
    virtual void addMovement(nsecs_t eventTime, BitSet32 idBits,
                             const std::vector<float>& positions) = 0;
    virtual bool getEstimator(uint32_t id, VelocityTracker::Estimator* outEstimator) const = 0;
};


/*
 * Velocity tracker algorithm based on least-squares linear regression.
 */
class LeastSquaresVelocityTrackerStrategy : public VelocityTrackerStrategy {
public:
    enum Weighting {
        // No weights applied.  All data points are equally reliable.
        WEIGHTING_NONE,

        // Weight by time delta.  Data points clustered together are weighted less.
        WEIGHTING_DELTA,

        // Weight such that points within a certain horizon are weighed more than those
        // outside of that horizon.
        WEIGHTING_CENTRAL,

        // Weight such that points older than a certain amount are weighed less.
        WEIGHTING_RECENT,
    };

    // Degree must be no greater than Estimator::MAX_DEGREE.
    LeastSquaresVelocityTrackerStrategy(uint32_t degree, Weighting weighting = WEIGHTING_NONE);
    virtual ~LeastSquaresVelocityTrackerStrategy();

    virtual void clearPointers(BitSet32 idBits);
    void addMovement(nsecs_t eventTime, BitSet32 idBits,
                     const std::vector<float>& positions) override;
    virtual bool getEstimator(uint32_t id, VelocityTracker::Estimator* outEstimator) const;

private:
    // Sample horizon.
    // We don't use too much history by default since we want to react to quick
    // changes in direction.
    static const nsecs_t HORIZON = 100 * 1000000; // 100 ms

    // Number of samples to keep.
    static const uint32_t HISTORY_SIZE = 20;

    struct Movement {
        nsecs_t eventTime;
        BitSet32 idBits;
        float positions[MAX_POINTERS];

        inline float getPosition(uint32_t id) const { return positions[idBits.getIndexOfBit(id)]; }
    };

    float chooseWeight(uint32_t index) const;

    const uint32_t mDegree;
    const Weighting mWeighting;
    uint32_t mIndex;
    Movement mMovements[HISTORY_SIZE];
};


/*
 * Velocity tracker algorithm that uses an IIR filter.
 */
class IntegratingVelocityTrackerStrategy : public VelocityTrackerStrategy {
public:
    // Degree must be 1 or 2.
    IntegratingVelocityTrackerStrategy(uint32_t degree);
    ~IntegratingVelocityTrackerStrategy();

    virtual void clearPointers(BitSet32 idBits);
    void addMovement(nsecs_t eventTime, BitSet32 idBits,
                     const std::vector<float>& positions) override;
    virtual bool getEstimator(uint32_t id, VelocityTracker::Estimator* outEstimator) const;

private:
    // Current state estimate for a particular pointer.
    struct State {
        nsecs_t updateTime;
        uint32_t degree;

        float pos, vel, accel;
    };

    const uint32_t mDegree;
    BitSet32 mPointerIdBits;
    State mPointerState[MAX_POINTER_ID + 1];

    void initState(State& state, nsecs_t eventTime, float pos) const;
    void updateState(State& state, nsecs_t eventTime, float pos) const;
    void populateEstimator(const State& state, VelocityTracker::Estimator* outEstimator) const;
};


/*
 * Velocity tracker strategy used prior to ICS.
 */
class LegacyVelocityTrackerStrategy : public VelocityTrackerStrategy {
public:
    LegacyVelocityTrackerStrategy();
    virtual ~LegacyVelocityTrackerStrategy();

    virtual void clearPointers(BitSet32 idBits);
    void addMovement(nsecs_t eventTime, BitSet32 idBits,
                     const std::vector<float>& positions) override;
    virtual bool getEstimator(uint32_t id, VelocityTracker::Estimator* outEstimator) const;

private:
    // Oldest sample to consider when calculating the velocity.
    static const nsecs_t HORIZON = 200 * 1000000; // 100 ms

    // Number of samples to keep.
    static const uint32_t HISTORY_SIZE = 20;

    // The minimum duration between samples when estimating velocity.
    static const nsecs_t MIN_DURATION = 10 * 1000000; // 10 ms

    struct Movement {
        nsecs_t eventTime;
        BitSet32 idBits;
        float positions[MAX_POINTERS];

        inline float getPosition(uint32_t id) const { return positions[idBits.getIndexOfBit(id)]; }
    };

    uint32_t mIndex;
    Movement mMovements[HISTORY_SIZE];
};

class ImpulseVelocityTrackerStrategy : public VelocityTrackerStrategy {
public:
    ImpulseVelocityTrackerStrategy(bool deltaValues);
    virtual ~ImpulseVelocityTrackerStrategy();

    virtual void clearPointers(BitSet32 idBits);
    void addMovement(nsecs_t eventTime, BitSet32 idBits,
                     const std::vector<float>& positions) override;
    virtual bool getEstimator(uint32_t id, VelocityTracker::Estimator* outEstimator) const;

private:
    // Sample horizon.
    // We don't use too much history by default since we want to react to quick
    // changes in direction.
    static constexpr nsecs_t HORIZON = 100 * 1000000; // 100 ms

    // Number of samples to keep.
    static constexpr size_t HISTORY_SIZE = 20;

    struct Movement {
        nsecs_t eventTime;
        BitSet32 idBits;
        float positions[MAX_POINTERS];

        inline float getPosition(uint32_t id) const { return positions[idBits.getIndexOfBit(id)]; }
    };

    // Whether or not the input movement values for the strategy come in the form of delta values.
    // If the input values are not deltas, the strategy needs to calculate deltas as part of its
    // velocity calculation.
    const bool mDeltaValues;

    size_t mIndex;
    Movement mMovements[HISTORY_SIZE];
};

} // namespace android
