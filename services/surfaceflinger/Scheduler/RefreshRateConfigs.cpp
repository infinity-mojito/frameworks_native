/*
 * Copyright 2019 The Android Open Source Project
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

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra"

#include <chrono>
#include <cmath>

#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <ftl/enum.h>
#include <utils/Trace.h>

#include "../SurfaceFlingerProperties.h"
#include "RefreshRateConfigs.h"

#undef LOG_TAG
#define LOG_TAG "RefreshRateConfigs"

namespace android::scheduler {
namespace {

struct RefreshRateScore {
    DisplayModeIterator modeIt;
    float score;
};

template <typename Iterator>
const DisplayModePtr& getMaxScoreRefreshRate(Iterator begin, Iterator end) {
    const auto it =
            std::max_element(begin, end, [](RefreshRateScore max, RefreshRateScore current) {
                const auto& [modeIt, score] = current;

                std::string name = to_string(modeIt->second->getFps());
                ALOGV("%s scores %.2f", name.c_str(), score);

                ATRACE_INT(name.c_str(), static_cast<int>(std::round(score * 100)));

                constexpr float kEpsilon = 0.0001f;
                return score > max.score * (1 + kEpsilon);
            });

    return it->modeIt->second;
}

constexpr RefreshRateConfigs::GlobalSignals kNoSignals;

std::string formatLayerInfo(const RefreshRateConfigs::LayerRequirement& layer, float weight) {
    return base::StringPrintf("%s (type=%s, weight=%.2f, seamlessness=%s) %s", layer.name.c_str(),
                              ftl::enum_string(layer.vote).c_str(), weight,
                              ftl::enum_string(layer.seamlessness).c_str(),
                              to_string(layer.desiredRefreshRate).c_str());
}

std::vector<Fps> constructKnownFrameRates(const DisplayModes& modes) {
    std::vector<Fps> knownFrameRates = {24_Hz, 30_Hz, 45_Hz, 60_Hz, 72_Hz};
    knownFrameRates.reserve(knownFrameRates.size() + modes.size());

    // Add all supported refresh rates.
    for (const auto& [id, mode] : modes) {
        knownFrameRates.push_back(mode->getFps());
    }

    // Sort and remove duplicates.
    std::sort(knownFrameRates.begin(), knownFrameRates.end(), isStrictlyLess);
    knownFrameRates.erase(std::unique(knownFrameRates.begin(), knownFrameRates.end(),
                                      isApproxEqual),
                          knownFrameRates.end());
    return knownFrameRates;
}

// The Filter is a `bool(const DisplayMode&)` predicate.
template <typename Filter>
std::vector<DisplayModeIterator> sortByRefreshRate(const DisplayModes& modes, Filter&& filter) {
    std::vector<DisplayModeIterator> sortedModes;
    sortedModes.reserve(modes.size());

    for (auto it = modes.begin(); it != modes.end(); ++it) {
        const auto& [id, mode] = *it;

        if (filter(*mode)) {
            ALOGV("%s: including mode %d", __func__, id.value());
            sortedModes.push_back(it);
        }
    }

    std::sort(sortedModes.begin(), sortedModes.end(), [](auto it1, auto it2) {
        const auto& mode1 = it1->second;
        const auto& mode2 = it2->second;

        if (mode1->getVsyncPeriod() == mode2->getVsyncPeriod()) {
            return mode1->getGroup() > mode2->getGroup();
        }

        return mode1->getVsyncPeriod() > mode2->getVsyncPeriod();
    });

    return sortedModes;
}

bool canModesSupportFrameRateOverride(const std::vector<DisplayModeIterator>& sortedModes) {
    for (const auto it1 : sortedModes) {
        const auto& mode1 = it1->second;
        for (const auto it2 : sortedModes) {
            const auto& mode2 = it2->second;

            if (RefreshRateConfigs::getFrameRateDivisor(mode1->getFps(), mode2->getFps()) >= 2) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

std::string RefreshRateConfigs::Policy::toString() const {
    return base::StringPrintf("default mode ID: %d, allowGroupSwitching = %d"
                              ", primary range: %s, app request range: %s",
                              defaultMode.value(), allowGroupSwitching,
                              to_string(primaryRange).c_str(), to_string(appRequestRange).c_str());
}

std::pair<nsecs_t, nsecs_t> RefreshRateConfigs::getDisplayFrames(nsecs_t layerPeriod,
                                                                 nsecs_t displayPeriod) const {
    auto [quotient, remainder] = std::div(layerPeriod, displayPeriod);
    if (remainder <= MARGIN_FOR_PERIOD_CALCULATION ||
        std::abs(remainder - displayPeriod) <= MARGIN_FOR_PERIOD_CALCULATION) {
        quotient++;
        remainder = 0;
    }

    return {quotient, remainder};
}

bool RefreshRateConfigs::isVoteAllowed(const LayerRequirement& layer, Fps refreshRate) const {
    using namespace fps_approx_ops;

    switch (layer.vote) {
        case LayerVoteType::ExplicitExactOrMultiple:
        case LayerVoteType::Heuristic:
            if (mConfig.frameRateMultipleThreshold != 0 &&
                refreshRate >= Fps::fromValue(mConfig.frameRateMultipleThreshold) &&
                layer.desiredRefreshRate < Fps::fromValue(mConfig.frameRateMultipleThreshold / 2)) {
                // Don't vote high refresh rates past the threshold for layers with a low desired
                // refresh rate. For example, desired 24 fps with 120 Hz threshold means no vote for
                // 120 Hz, but desired 60 fps should have a vote.
                return false;
            }
            break;
        case LayerVoteType::ExplicitDefault:
        case LayerVoteType::ExplicitExact:
        case LayerVoteType::Max:
        case LayerVoteType::Min:
        case LayerVoteType::NoVote:
            break;
    }
    return true;
}

float RefreshRateConfigs::calculateNonExactMatchingLayerScoreLocked(const LayerRequirement& layer,
                                                                    Fps refreshRate) const {
    constexpr float kScoreForFractionalPairs = .8f;

    const auto displayPeriod = refreshRate.getPeriodNsecs();
    const auto layerPeriod = layer.desiredRefreshRate.getPeriodNsecs();
    if (layer.vote == LayerVoteType::ExplicitDefault) {
        // Find the actual rate the layer will render, assuming
        // that layerPeriod is the minimal period to render a frame.
        // For example if layerPeriod is 20ms and displayPeriod is 16ms,
        // then the actualLayerPeriod will be 32ms, because it is the
        // smallest multiple of the display period which is >= layerPeriod.
        auto actualLayerPeriod = displayPeriod;
        int multiplier = 1;
        while (layerPeriod > actualLayerPeriod + MARGIN_FOR_PERIOD_CALCULATION) {
            multiplier++;
            actualLayerPeriod = displayPeriod * multiplier;
        }

        // Because of the threshold we used above it's possible that score is slightly
        // above 1.
        return std::min(1.0f,
                        static_cast<float>(layerPeriod) / static_cast<float>(actualLayerPeriod));
    }

    if (layer.vote == LayerVoteType::ExplicitExactOrMultiple ||
        layer.vote == LayerVoteType::Heuristic) {
        if (isFractionalPairOrMultiple(refreshRate, layer.desiredRefreshRate)) {
            return kScoreForFractionalPairs;
        }

        // Calculate how many display vsyncs we need to present a single frame for this
        // layer
        const auto [displayFramesQuotient, displayFramesRemainder] =
                getDisplayFrames(layerPeriod, displayPeriod);
        static constexpr size_t MAX_FRAMES_TO_FIT = 10; // Stop calculating when score < 0.1
        if (displayFramesRemainder == 0) {
            // Layer desired refresh rate matches the display rate.
            return 1.0f;
        }

        if (displayFramesQuotient == 0) {
            // Layer desired refresh rate is higher than the display rate.
            return (static_cast<float>(layerPeriod) / static_cast<float>(displayPeriod)) *
                    (1.0f / (MAX_FRAMES_TO_FIT + 1));
        }

        // Layer desired refresh rate is lower than the display rate. Check how well it fits
        // the cadence.
        auto diff = std::abs(displayFramesRemainder - (displayPeriod - displayFramesRemainder));
        int iter = 2;
        while (diff > MARGIN_FOR_PERIOD_CALCULATION && iter < MAX_FRAMES_TO_FIT) {
            diff = diff - (displayPeriod - diff);
            iter++;
        }

        return (1.0f / iter);
    }

    return 0;
}

float RefreshRateConfigs::calculateLayerScoreLocked(const LayerRequirement& layer, Fps refreshRate,
                                                    bool isSeamlessSwitch) const {
    if (!isVoteAllowed(layer, refreshRate)) {
        return 0;
    }

    // Slightly prefer seamless switches.
    constexpr float kSeamedSwitchPenalty = 0.95f;
    const float seamlessness = isSeamlessSwitch ? 1.0f : kSeamedSwitchPenalty;

    // If the layer wants Max, give higher score to the higher refresh rate
    if (layer.vote == LayerVoteType::Max) {
        const auto& maxRefreshRate = mAppRequestRefreshRates.back()->second;
        const auto ratio = refreshRate.getValue() / maxRefreshRate->getFps().getValue();
        // use ratio^2 to get a lower score the more we get further from peak
        return ratio * ratio;
    }

    if (layer.vote == LayerVoteType::ExplicitExact) {
        const int divisor = getFrameRateDivisor(refreshRate, layer.desiredRefreshRate);
        if (mSupportsFrameRateOverrideByContent) {
            // Since we support frame rate override, allow refresh rates which are
            // multiples of the layer's request, as those apps would be throttled
            // down to run at the desired refresh rate.
            return divisor > 0;
        }

        return divisor == 1;
    }

    // If the layer frame rate is a divisor of the refresh rate it should score
    // the highest score.
    if (getFrameRateDivisor(refreshRate, layer.desiredRefreshRate) > 0) {
        return 1.0f * seamlessness;
    }

    // The layer frame rate is not a divisor of the refresh rate,
    // there is a small penalty attached to the score to favor the frame rates
    // the exactly matches the display refresh rate or a multiple.
    constexpr float kNonExactMatchingPenalty = 0.95f;
    return calculateNonExactMatchingLayerScoreLocked(layer, refreshRate) * seamlessness *
            kNonExactMatchingPenalty;
}

auto RefreshRateConfigs::getBestRefreshRate(const std::vector<LayerRequirement>& layers,
                                            GlobalSignals signals) const
        -> std::pair<DisplayModePtr, GlobalSignals> {
    std::lock_guard lock(mLock);

    if (mGetBestRefreshRateCache &&
        mGetBestRefreshRateCache->arguments == std::make_pair(layers, signals)) {
        return mGetBestRefreshRateCache->result;
    }

    const auto result = getBestRefreshRateLocked(layers, signals);
    mGetBestRefreshRateCache = GetBestRefreshRateCache{{layers, signals}, result};
    return result;
}

auto RefreshRateConfigs::getBestRefreshRateLocked(const std::vector<LayerRequirement>& layers,
                                                  GlobalSignals signals) const
        -> std::pair<DisplayModePtr, GlobalSignals> {
    ATRACE_CALL();
    ALOGV("%s: %zu layers", __func__, layers.size());

    int noVoteLayers = 0;
    int minVoteLayers = 0;
    int maxVoteLayers = 0;
    int explicitDefaultVoteLayers = 0;
    int explicitExactOrMultipleVoteLayers = 0;
    int explicitExact = 0;
    float maxExplicitWeight = 0;
    int seamedFocusedLayers = 0;

    for (const auto& layer : layers) {
        switch (layer.vote) {
            case LayerVoteType::NoVote:
                noVoteLayers++;
                break;
            case LayerVoteType::Min:
                minVoteLayers++;
                break;
            case LayerVoteType::Max:
                maxVoteLayers++;
                break;
            case LayerVoteType::ExplicitDefault:
                explicitDefaultVoteLayers++;
                maxExplicitWeight = std::max(maxExplicitWeight, layer.weight);
                break;
            case LayerVoteType::ExplicitExactOrMultiple:
                explicitExactOrMultipleVoteLayers++;
                maxExplicitWeight = std::max(maxExplicitWeight, layer.weight);
                break;
            case LayerVoteType::ExplicitExact:
                explicitExact++;
                maxExplicitWeight = std::max(maxExplicitWeight, layer.weight);
                break;
            case LayerVoteType::Heuristic:
                break;
        }

        if (layer.seamlessness == Seamlessness::SeamedAndSeamless && layer.focused) {
            seamedFocusedLayers++;
        }
    }

    const bool hasExplicitVoteLayers = explicitDefaultVoteLayers > 0 ||
            explicitExactOrMultipleVoteLayers > 0 || explicitExact > 0;

    const Policy* policy = getCurrentPolicyLocked();
    const auto& defaultMode = mDisplayModes.get(policy->defaultMode)->get();
    // If the default mode group is different from the group of current mode,
    // this means a layer requesting a seamed mode switch just disappeared and
    // we should switch back to the default group.
    // However if a seamed layer is still present we anchor around the group
    // of the current mode, in order to prevent unnecessary seamed mode switches
    // (e.g. when pausing a video playback).
    const auto anchorGroup =
            seamedFocusedLayers > 0 ? mActiveModeIt->second->getGroup() : defaultMode->getGroup();

    // Consider the touch event if there are no Explicit* layers. Otherwise wait until after we've
    // selected a refresh rate to see if we should apply touch boost.
    if (signals.touch && !hasExplicitVoteLayers) {
        const DisplayModePtr& max = getMaxRefreshRateByPolicyLocked(anchorGroup);
        ALOGV("TouchBoost - choose %s", to_string(max->getFps()).c_str());
        return {max, GlobalSignals{.touch = true}};
    }

    // If the primary range consists of a single refresh rate then we can only
    // move out the of range if layers explicitly request a different refresh
    // rate.
    const bool primaryRangeIsSingleRate =
            isApproxEqual(policy->primaryRange.min, policy->primaryRange.max);

    if (!signals.touch && signals.idle && !(primaryRangeIsSingleRate && hasExplicitVoteLayers)) {
        const DisplayModePtr& min = getMinRefreshRateByPolicyLocked();
        ALOGV("Idle - choose %s", to_string(min->getFps()).c_str());
        return {min, GlobalSignals{.idle = true}};
    }

    if (layers.empty() || noVoteLayers == layers.size()) {
        const DisplayModePtr& max = getMaxRefreshRateByPolicyLocked(anchorGroup);
        ALOGV("no layers with votes - choose %s", to_string(max->getFps()).c_str());
        return {max, kNoSignals};
    }

    // Only if all layers want Min we should return Min
    if (noVoteLayers + minVoteLayers == layers.size()) {
        const DisplayModePtr& min = getMinRefreshRateByPolicyLocked();
        ALOGV("all layers Min - choose %s", to_string(min->getFps()).c_str());
        return {min, kNoSignals};
    }

    // Find the best refresh rate based on score
    std::vector<RefreshRateScore> scores;
    scores.reserve(mAppRequestRefreshRates.size());

    for (const DisplayModeIterator modeIt : mAppRequestRefreshRates) {
        scores.emplace_back(RefreshRateScore{modeIt, 0.0f});
    }

    for (const auto& layer : layers) {
        ALOGV("Calculating score for %s (%s, weight %.2f, desired %.2f) ", layer.name.c_str(),
              ftl::enum_string(layer.vote).c_str(), layer.weight,
              layer.desiredRefreshRate.getValue());
        if (layer.vote == LayerVoteType::NoVote || layer.vote == LayerVoteType::Min) {
            continue;
        }

        const auto weight = layer.weight;

        for (auto& [modeIt, score] : scores) {
            const auto& [id, mode] = *modeIt;
            const bool isSeamlessSwitch = mode->getGroup() == mActiveModeIt->second->getGroup();

            if (layer.seamlessness == Seamlessness::OnlySeamless && !isSeamlessSwitch) {
                ALOGV("%s ignores %s to avoid non-seamless switch. Current mode = %s",
                      formatLayerInfo(layer, weight).c_str(), to_string(*mode).c_str(),
                      to_string(*mActiveModeIt->second).c_str());
                continue;
            }

            if (layer.seamlessness == Seamlessness::SeamedAndSeamless && !isSeamlessSwitch &&
                !layer.focused) {
                ALOGV("%s ignores %s because it's not focused and the switch is going to be seamed."
                      " Current mode = %s",
                      formatLayerInfo(layer, weight).c_str(), to_string(*mode).c_str(),
                      to_string(*mActiveModeIt->second).c_str());
                continue;
            }

            // Layers with default seamlessness vote for the current mode group if
            // there are layers with seamlessness=SeamedAndSeamless and for the default
            // mode group otherwise. In second case, if the current mode group is different
            // from the default, this means a layer with seamlessness=SeamedAndSeamless has just
            // disappeared.
            const bool isInPolicyForDefault = mode->getGroup() == anchorGroup;
            if (layer.seamlessness == Seamlessness::Default && !isInPolicyForDefault) {
                ALOGV("%s ignores %s. Current mode = %s", formatLayerInfo(layer, weight).c_str(),
                      to_string(*mode).c_str(), to_string(*mActiveModeIt->second).c_str());
                continue;
            }

            const bool inPrimaryRange = policy->primaryRange.includes(mode->getFps());
            if ((primaryRangeIsSingleRate || !inPrimaryRange) &&
                !(layer.focused &&
                  (layer.vote == LayerVoteType::ExplicitDefault ||
                   layer.vote == LayerVoteType::ExplicitExact))) {
                // Only focused layers with ExplicitDefault frame rate settings are allowed to score
                // refresh rates outside the primary range.
                continue;
            }

            const auto layerScore =
                    calculateLayerScoreLocked(layer, mode->getFps(), isSeamlessSwitch);
            ALOGV("%s gives %s score of %.4f", formatLayerInfo(layer, weight).c_str(),
                  to_string(mode->getFps()).c_str(), layerScore);

            score += weight * layerScore;
        }
    }

    // Now that we scored all the refresh rates we need to pick the one that got the highest score.
    // In case of a tie we will pick the higher refresh rate if any of the layers wanted Max,
    // or the lower otherwise.
    const DisplayModePtr& bestRefreshRate = maxVoteLayers > 0
            ? getMaxScoreRefreshRate(scores.rbegin(), scores.rend())
            : getMaxScoreRefreshRate(scores.begin(), scores.end());

    if (primaryRangeIsSingleRate) {
        // If we never scored any layers, then choose the rate from the primary
        // range instead of picking a random score from the app range.
        if (std::all_of(scores.begin(), scores.end(),
                        [](RefreshRateScore score) { return score.score == 0; })) {
            const DisplayModePtr& max = getMaxRefreshRateByPolicyLocked(anchorGroup);
            ALOGV("layers not scored - choose %s", to_string(max->getFps()).c_str());
            return {max, kNoSignals};
        } else {
            return {bestRefreshRate, kNoSignals};
        }
    }

    // Consider the touch event if there are no ExplicitDefault layers. ExplicitDefault are mostly
    // interactive (as opposed to ExplicitExactOrMultiple) and therefore if those posted an explicit
    // vote we should not change it if we get a touch event. Only apply touch boost if it will
    // actually increase the refresh rate over the normal selection.
    const DisplayModePtr& touchRefreshRate = getMaxRefreshRateByPolicyLocked(anchorGroup);

    const bool touchBoostForExplicitExact = [&] {
        if (mSupportsFrameRateOverrideByContent) {
            // Enable touch boost if there are other layers besides exact
            return explicitExact + noVoteLayers != layers.size();
        } else {
            // Enable touch boost if there are no exact layers
            return explicitExact == 0;
        }
    }();

    using fps_approx_ops::operator<;

    if (signals.touch && explicitDefaultVoteLayers == 0 && touchBoostForExplicitExact &&
        bestRefreshRate->getFps() < touchRefreshRate->getFps()) {
        ALOGV("TouchBoost - choose %s", to_string(touchRefreshRate->getFps()).c_str());
        return {touchRefreshRate, GlobalSignals{.touch = true}};
    }

    return {bestRefreshRate, kNoSignals};
}

std::unordered_map<uid_t, std::vector<const RefreshRateConfigs::LayerRequirement*>>
groupLayersByUid(const std::vector<RefreshRateConfigs::LayerRequirement>& layers) {
    std::unordered_map<uid_t, std::vector<const RefreshRateConfigs::LayerRequirement*>> layersByUid;
    for (const auto& layer : layers) {
        auto iter = layersByUid.emplace(layer.ownerUid,
                                        std::vector<const RefreshRateConfigs::LayerRequirement*>());
        auto& layersWithSameUid = iter.first->second;
        layersWithSameUid.push_back(&layer);
    }

    // Remove uids that can't have a frame rate override
    for (auto iter = layersByUid.begin(); iter != layersByUid.end();) {
        const auto& layersWithSameUid = iter->second;
        bool skipUid = false;
        for (const auto& layer : layersWithSameUid) {
            if (layer->vote == RefreshRateConfigs::LayerVoteType::Max ||
                layer->vote == RefreshRateConfigs::LayerVoteType::Heuristic) {
                skipUid = true;
                break;
            }
        }
        if (skipUid) {
            iter = layersByUid.erase(iter);
        } else {
            ++iter;
        }
    }

    return layersByUid;
}

RefreshRateConfigs::UidToFrameRateOverride RefreshRateConfigs::getFrameRateOverrides(
        const std::vector<LayerRequirement>& layers, Fps displayRefreshRate,
        GlobalSignals globalSignals) const {
    ATRACE_CALL();

    ALOGV("%s: %zu layers", __func__, layers.size());

    std::lock_guard lock(mLock);

    std::vector<RefreshRateScore> scores;
    scores.reserve(mDisplayModes.size());

    for (auto it = mDisplayModes.begin(); it != mDisplayModes.end(); ++it) {
        scores.emplace_back(RefreshRateScore{it, 0.0f});
    }

    std::sort(scores.begin(), scores.end(), [](const auto& lhs, const auto& rhs) {
        const auto& mode1 = lhs.modeIt->second;
        const auto& mode2 = rhs.modeIt->second;
        return isStrictlyLess(mode1->getFps(), mode2->getFps());
    });

    std::unordered_map<uid_t, std::vector<const LayerRequirement*>> layersByUid =
            groupLayersByUid(layers);
    UidToFrameRateOverride frameRateOverrides;
    for (const auto& [uid, layersWithSameUid] : layersByUid) {
        // Layers with ExplicitExactOrMultiple expect touch boost
        const bool hasExplicitExactOrMultiple =
                std::any_of(layersWithSameUid.cbegin(), layersWithSameUid.cend(),
                            [](const auto& layer) {
                                return layer->vote == LayerVoteType::ExplicitExactOrMultiple;
                            });

        if (globalSignals.touch && hasExplicitExactOrMultiple) {
            continue;
        }

        for (auto& [_, score] : scores) {
            score = 0;
        }

        for (const auto& layer : layersWithSameUid) {
            if (layer->vote == LayerVoteType::NoVote || layer->vote == LayerVoteType::Min) {
                continue;
            }

            LOG_ALWAYS_FATAL_IF(layer->vote != LayerVoteType::ExplicitDefault &&
                                layer->vote != LayerVoteType::ExplicitExactOrMultiple &&
                                layer->vote != LayerVoteType::ExplicitExact);
            for (auto& [modeIt, score] : scores) {
                constexpr bool isSeamlessSwitch = true;
                const auto layerScore = calculateLayerScoreLocked(*layer, modeIt->second->getFps(),
                                                                  isSeamlessSwitch);
                score += layer->weight * layerScore;
            }
        }

        // We just care about the refresh rates which are a divisor of the
        // display refresh rate
        const auto it = std::remove_if(scores.begin(), scores.end(), [&](RefreshRateScore score) {
            const auto& [id, mode] = *score.modeIt;
            return getFrameRateDivisor(displayRefreshRate, mode->getFps()) == 0;
        });
        scores.erase(it, scores.end());

        // If we never scored any layers, we don't have a preferred frame rate
        if (std::all_of(scores.begin(), scores.end(),
                        [](RefreshRateScore score) { return score.score == 0; })) {
            continue;
        }

        // Now that we scored all the refresh rates we need to pick the one that got the highest
        // score.
        const DisplayModePtr& bestRefreshRate =
                getMaxScoreRefreshRate(scores.begin(), scores.end());

        frameRateOverrides.emplace(uid, bestRefreshRate->getFps());
    }

    return frameRateOverrides;
}

std::optional<Fps> RefreshRateConfigs::onKernelTimerChanged(
        std::optional<DisplayModeId> desiredActiveModeId, bool timerExpired) const {
    std::lock_guard lock(mLock);

    const DisplayModePtr& current = desiredActiveModeId
            ? mDisplayModes.get(*desiredActiveModeId)->get()
            : mActiveModeIt->second;

    const DisplayModePtr& min = mMinRefreshRateModeIt->second;
    if (current == min) {
        return {};
    }

    const auto& mode = timerExpired ? min : current;
    return mode->getFps();
}

const DisplayModePtr& RefreshRateConfigs::getMinRefreshRateByPolicyLocked() const {
    for (const DisplayModeIterator modeIt : mPrimaryRefreshRates) {
        const auto& mode = modeIt->second;
        if (mActiveModeIt->second->getGroup() == mode->getGroup()) {
            return mode;
        }
    }

    ALOGE("Can't find min refresh rate by policy with the same mode group"
          " as the current mode %s",
          to_string(*mActiveModeIt->second).c_str());

    // Default to the lowest refresh rate.
    return mPrimaryRefreshRates.front()->second;
}

DisplayModePtr RefreshRateConfigs::getMaxRefreshRateByPolicy() const {
    std::lock_guard lock(mLock);
    return getMaxRefreshRateByPolicyLocked();
}

const DisplayModePtr& RefreshRateConfigs::getMaxRefreshRateByPolicyLocked(int anchorGroup) const {
    for (auto it = mPrimaryRefreshRates.rbegin(); it != mPrimaryRefreshRates.rend(); ++it) {
        const auto& mode = (*it)->second;
        if (anchorGroup == mode->getGroup()) {
            return mode;
        }
    }

    ALOGE("Can't find max refresh rate by policy with the same mode group"
          " as the current mode %s",
          to_string(*mActiveModeIt->second).c_str());

    // Default to the highest refresh rate.
    return mPrimaryRefreshRates.back()->second;
}

DisplayModePtr RefreshRateConfigs::getActiveMode() const {
    std::lock_guard lock(mLock);
    return mActiveModeIt->second;
}

void RefreshRateConfigs::setActiveModeId(DisplayModeId modeId) {
    std::lock_guard lock(mLock);

    // Invalidate the cached invocation to getBestRefreshRate. This forces
    // the refresh rate to be recomputed on the next call to getBestRefreshRate.
    mGetBestRefreshRateCache.reset();

    mActiveModeIt = mDisplayModes.find(modeId);
    LOG_ALWAYS_FATAL_IF(mActiveModeIt == mDisplayModes.end());
}

RefreshRateConfigs::RefreshRateConfigs(DisplayModes modes, DisplayModeId activeModeId,
                                       Config config)
      : mKnownFrameRates(constructKnownFrameRates(modes)), mConfig(config) {
    initializeIdleTimer();
    updateDisplayModes(std::move(modes), activeModeId);
}

void RefreshRateConfigs::initializeIdleTimer() {
    if (mConfig.idleTimerTimeout > 0ms) {
        mIdleTimer.emplace(
                "IdleTimer", mConfig.idleTimerTimeout,
                [this] {
                    std::scoped_lock lock(mIdleTimerCallbacksMutex);
                    if (const auto callbacks = getIdleTimerCallbacks()) {
                        callbacks->onReset();
                    }
                },
                [this] {
                    std::scoped_lock lock(mIdleTimerCallbacksMutex);
                    if (const auto callbacks = getIdleTimerCallbacks()) {
                        callbacks->onExpired();
                    }
                });
    }
}

void RefreshRateConfigs::updateDisplayModes(DisplayModes modes, DisplayModeId activeModeId) {
    std::lock_guard lock(mLock);

    // Invalidate the cached invocation to getBestRefreshRate. This forces
    // the refresh rate to be recomputed on the next call to getBestRefreshRate.
    mGetBestRefreshRateCache.reset();

    mDisplayModes = std::move(modes);
    mActiveModeIt = mDisplayModes.find(activeModeId);
    LOG_ALWAYS_FATAL_IF(mActiveModeIt == mDisplayModes.end());

    const auto sortedModes =
            sortByRefreshRate(mDisplayModes, [](const DisplayMode&) { return true; });
    mMinRefreshRateModeIt = sortedModes.front();
    mMaxRefreshRateModeIt = sortedModes.back();

    // Reset the policy because the old one may no longer be valid.
    mDisplayManagerPolicy = {};
    mDisplayManagerPolicy.defaultMode = activeModeId;

    mSupportsFrameRateOverrideByContent =
            mConfig.enableFrameRateOverride && canModesSupportFrameRateOverride(sortedModes);

    constructAvailableRefreshRates();
}

bool RefreshRateConfigs::isPolicyValidLocked(const Policy& policy) const {
    // defaultMode must be a valid mode, and within the given refresh rate range.
    if (const auto mode = mDisplayModes.get(policy.defaultMode)) {
        if (!policy.primaryRange.includes(mode->get()->getFps())) {
            ALOGE("Default mode is not in the primary range.");
            return false;
        }
    } else {
        ALOGE("Default mode is not found.");
        return false;
    }

    using namespace fps_approx_ops;
    return policy.appRequestRange.min <= policy.primaryRange.min &&
            policy.appRequestRange.max >= policy.primaryRange.max;
}

status_t RefreshRateConfigs::setDisplayManagerPolicy(const Policy& policy) {
    std::lock_guard lock(mLock);
    if (!isPolicyValidLocked(policy)) {
        ALOGE("Invalid refresh rate policy: %s", policy.toString().c_str());
        return BAD_VALUE;
    }
    mGetBestRefreshRateCache.reset();
    Policy previousPolicy = *getCurrentPolicyLocked();
    mDisplayManagerPolicy = policy;
    if (*getCurrentPolicyLocked() == previousPolicy) {
        return CURRENT_POLICY_UNCHANGED;
    }
    constructAvailableRefreshRates();
    return NO_ERROR;
}

status_t RefreshRateConfigs::setOverridePolicy(const std::optional<Policy>& policy) {
    std::lock_guard lock(mLock);
    if (policy && !isPolicyValidLocked(*policy)) {
        return BAD_VALUE;
    }
    mGetBestRefreshRateCache.reset();
    Policy previousPolicy = *getCurrentPolicyLocked();
    mOverridePolicy = policy;
    if (*getCurrentPolicyLocked() == previousPolicy) {
        return CURRENT_POLICY_UNCHANGED;
    }
    constructAvailableRefreshRates();
    return NO_ERROR;
}

const RefreshRateConfigs::Policy* RefreshRateConfigs::getCurrentPolicyLocked() const {
    return mOverridePolicy ? &mOverridePolicy.value() : &mDisplayManagerPolicy;
}

RefreshRateConfigs::Policy RefreshRateConfigs::getCurrentPolicy() const {
    std::lock_guard lock(mLock);
    return *getCurrentPolicyLocked();
}

RefreshRateConfigs::Policy RefreshRateConfigs::getDisplayManagerPolicy() const {
    std::lock_guard lock(mLock);
    return mDisplayManagerPolicy;
}

bool RefreshRateConfigs::isModeAllowed(DisplayModeId modeId) const {
    std::lock_guard lock(mLock);
    return std::any_of(mAppRequestRefreshRates.begin(), mAppRequestRefreshRates.end(),
                       [modeId](DisplayModeIterator modeIt) {
                           return modeIt->second->getId() == modeId;
                       });
}

void RefreshRateConfigs::constructAvailableRefreshRates() {
    // Filter modes based on current policy and sort on refresh rate.
    const Policy* policy = getCurrentPolicyLocked();
    ALOGV("%s: %s ", __func__, policy->toString().c_str());

    const auto& defaultMode = mDisplayModes.get(policy->defaultMode)->get();

    const auto filterRefreshRates = [&](FpsRange range, const char* rangeName) REQUIRES(mLock) {
        const auto filter = [&](const DisplayMode& mode) {
            return mode.getResolution() == defaultMode->getResolution() &&
                    mode.getDpi() == defaultMode->getDpi() &&
                    (policy->allowGroupSwitching || mode.getGroup() == defaultMode->getGroup()) &&
                    range.includes(mode.getFps());
        };

        const auto modes = sortByRefreshRate(mDisplayModes, filter);
        LOG_ALWAYS_FATAL_IF(modes.empty(), "No matching modes for %s range %s", rangeName,
                            to_string(range).c_str());

        const auto stringifyModes = [&] {
            std::string str;
            for (const auto modeIt : modes) {
                str += to_string(modeIt->second->getFps());
                str.push_back(' ');
            }
            return str;
        };
        ALOGV("%s refresh rates: %s", rangeName, stringifyModes().c_str());

        return modes;
    };

    mPrimaryRefreshRates = filterRefreshRates(policy->primaryRange, "primary");
    mAppRequestRefreshRates = filterRefreshRates(policy->appRequestRange, "app request");
}

Fps RefreshRateConfigs::findClosestKnownFrameRate(Fps frameRate) const {
    using namespace fps_approx_ops;

    if (frameRate <= mKnownFrameRates.front()) {
        return mKnownFrameRates.front();
    }

    if (frameRate >= mKnownFrameRates.back()) {
        return mKnownFrameRates.back();
    }

    auto lowerBound = std::lower_bound(mKnownFrameRates.begin(), mKnownFrameRates.end(), frameRate,
                                       isStrictlyLess);

    const auto distance1 = std::abs(frameRate.getValue() - lowerBound->getValue());
    const auto distance2 = std::abs(frameRate.getValue() - std::prev(lowerBound)->getValue());
    return distance1 < distance2 ? *lowerBound : *std::prev(lowerBound);
}

RefreshRateConfigs::KernelIdleTimerAction RefreshRateConfigs::getIdleTimerAction() const {
    std::lock_guard lock(mLock);

    const Fps deviceMinFps = mMinRefreshRateModeIt->second->getFps();
    const DisplayModePtr& minByPolicy = getMinRefreshRateByPolicyLocked();

    // Kernel idle timer will set the refresh rate to the device min. If DisplayManager says that
    // the min allowed refresh rate is higher than the device min, we do not want to enable the
    // timer.
    if (isStrictlyLess(deviceMinFps, minByPolicy->getFps())) {
        return KernelIdleTimerAction::TurnOff;
    }

    const DisplayModePtr& maxByPolicy = getMaxRefreshRateByPolicyLocked();
    if (minByPolicy == maxByPolicy) {
        // Turn on the timer when the min of the primary range is below the device min.
        if (const Policy* currentPolicy = getCurrentPolicyLocked();
            isApproxLess(currentPolicy->primaryRange.min, deviceMinFps)) {
            return KernelIdleTimerAction::TurnOn;
        }
        return KernelIdleTimerAction::TurnOff;
    }

    // Turn on the timer in all other cases.
    return KernelIdleTimerAction::TurnOn;
}

int RefreshRateConfigs::getFrameRateDivisor(Fps displayRefreshRate, Fps layerFrameRate) {
    // This calculation needs to be in sync with the java code
    // in DisplayManagerService.getDisplayInfoForFrameRateOverride

    // The threshold must be smaller than 0.001 in order to differentiate
    // between the fractional pairs (e.g. 59.94 and 60).
    constexpr float kThreshold = 0.0009f;
    const auto numPeriods = displayRefreshRate.getValue() / layerFrameRate.getValue();
    const auto numPeriodsRounded = std::round(numPeriods);
    if (std::abs(numPeriods - numPeriodsRounded) > kThreshold) {
        return 0;
    }

    return static_cast<int>(numPeriodsRounded);
}

bool RefreshRateConfigs::isFractionalPairOrMultiple(Fps smaller, Fps bigger) {
    if (isStrictlyLess(bigger, smaller)) {
        return isFractionalPairOrMultiple(bigger, smaller);
    }

    const auto multiplier = std::round(bigger.getValue() / smaller.getValue());
    constexpr float kCoef = 1000.f / 1001.f;
    return isApproxEqual(bigger, Fps::fromValue(smaller.getValue() * multiplier / kCoef)) ||
            isApproxEqual(bigger, Fps::fromValue(smaller.getValue() * multiplier * kCoef));
}

void RefreshRateConfigs::dump(std::string& result) const {
    std::lock_guard lock(mLock);
    base::StringAppendF(&result, "DesiredDisplayModeSpecs (DisplayManager): %s\n\n",
                        mDisplayManagerPolicy.toString().c_str());
    scheduler::RefreshRateConfigs::Policy currentPolicy = *getCurrentPolicyLocked();
    if (mOverridePolicy && currentPolicy != mDisplayManagerPolicy) {
        base::StringAppendF(&result, "DesiredDisplayModeSpecs (Override): %s\n\n",
                            currentPolicy.toString().c_str());
    }

    base::StringAppendF(&result, "Active mode: %s\n", to_string(*mActiveModeIt->second).c_str());

    result.append("Display modes:\n");
    for (const auto& [id, mode] : mDisplayModes) {
        result.push_back('\t');
        result.append(to_string(*mode));
        result.push_back('\n');
    }

    base::StringAppendF(&result, "Supports Frame Rate Override By Content: %s\n",
                        mSupportsFrameRateOverrideByContent ? "yes" : "no");

    result.append("Idle timer: ");
    if (const auto controller = mConfig.kernelIdleTimerController) {
        base::StringAppendF(&result, "(kernel via %s) ", ftl::enum_string(*controller).c_str());
    } else {
        result.append("(platform) ");
    }

    if (mIdleTimer) {
        result.append(mIdleTimer->dump());
    } else {
        result.append("off");
    }

    result.append("\n\n");
}

std::chrono::milliseconds RefreshRateConfigs::getIdleTimerTimeout() {
    return mConfig.idleTimerTimeout;
}

} // namespace android::scheduler

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wextra"
