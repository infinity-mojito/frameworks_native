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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "FrontEnd/LayerHandle.h"
#include "FrontEnd/LayerHierarchy.h"
#include "FrontEnd/LayerLifecycleManager.h"
#include "FrontEnd/LayerSnapshotBuilder.h"
#include "Layer.h"
#include "LayerHierarchyTest.h"

#define UPDATE_AND_VERIFY(BUILDER, ...)                                    \
    ({                                                                     \
        SCOPED_TRACE("");                                                  \
        updateAndVerify((BUILDER), /*displayChanges=*/false, __VA_ARGS__); \
    })

#define UPDATE_AND_VERIFY_WITH_DISPLAY_CHANGES(BUILDER, ...)              \
    ({                                                                    \
        SCOPED_TRACE("");                                                 \
        updateAndVerify((BUILDER), /*displayChanges=*/true, __VA_ARGS__); \
    })

namespace android::surfaceflinger::frontend {

// To run test:
/**
 mp :libsurfaceflinger_unittest && adb sync; adb shell \
    /data/nativetest/libsurfaceflinger_unittest/libsurfaceflinger_unittest \
    --gtest_filter="LayerSnapshotTest.*" --gtest_brief=1
*/

class LayerSnapshotTest : public LayerHierarchyTestBase {
protected:
    LayerSnapshotTest() : LayerHierarchyTestBase() {
        UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    }

    void createRootLayer(uint32_t id) override {
        LayerHierarchyTestBase::createRootLayer(id);
        setColor(id);
    }

    void createLayer(uint32_t id, uint32_t parentId) override {
        LayerHierarchyTestBase::createLayer(id, parentId);
        setColor(parentId);
    }

    void mirrorLayer(uint32_t id, uint32_t parent, uint32_t layerToMirror) override {
        LayerHierarchyTestBase::mirrorLayer(id, parent, layerToMirror);
        setColor(id);
    }

    void updateAndVerify(LayerSnapshotBuilder& actualBuilder, bool hasDisplayChanges,
                         const std::vector<uint32_t> expectedVisibleLayerIdsInZOrder) {
        if (mLifecycleManager.getGlobalChanges().test(RequestedLayerState::Changes::Hierarchy)) {
            mHierarchyBuilder.update(mLifecycleManager.getLayers(),
                                     mLifecycleManager.getDestroyedLayers());
        }
        LayerSnapshotBuilder::Args args{
                .root = mHierarchyBuilder.getHierarchy(),
                .layerLifecycleManager = mLifecycleManager,
                .includeMetadata = false,
                .displays = mFrontEndDisplayInfos,
                .displayChanges = hasDisplayChanges,
                .globalShadowSettings = globalShadowSettings,
        };
        actualBuilder.update(args);

        // rebuild layer snapshots from scratch and verify that it matches the updated state.
        LayerSnapshotBuilder expectedBuilder(args);
        mLifecycleManager.commitChanges();
        ASSERT_TRUE(expectedBuilder.getSnapshots().size() > 0);
        ASSERT_TRUE(actualBuilder.getSnapshots().size() > 0);

        std::vector<std::unique_ptr<LayerSnapshot>>& snapshots = actualBuilder.getSnapshots();
        std::vector<uint32_t> actualVisibleLayerIdsInZOrder;
        for (auto& snapshot : snapshots) {
            if (!snapshot->isVisible) {
                break;
            }
            actualVisibleLayerIdsInZOrder.push_back(snapshot->path.id);
        }
        EXPECT_EQ(expectedVisibleLayerIdsInZOrder, actualVisibleLayerIdsInZOrder);
    }

    LayerSnapshot* getSnapshot(uint32_t layerId) { return mSnapshotBuilder.getSnapshot(layerId); }

    LayerHierarchyBuilder mHierarchyBuilder{{}};
    LayerSnapshotBuilder mSnapshotBuilder;
    std::unordered_map<uint32_t, sp<LayerHandle>> mHandles;
    display::DisplayMap<ui::LayerStack, frontend::DisplayInfo> mFrontEndDisplayInfos;
    renderengine::ShadowSettings globalShadowSettings;
    static const std::vector<uint32_t> STARTING_ZORDER;
};
const std::vector<uint32_t> LayerSnapshotTest::STARTING_ZORDER = {1,   11,   111, 12, 121,
                                                                  122, 1221, 13,  2};

TEST_F(LayerSnapshotTest, buildSnapshot) {
    LayerSnapshotBuilder::Args args{
            .root = mHierarchyBuilder.getHierarchy(),
            .layerLifecycleManager = mLifecycleManager,
            .includeMetadata = false,
            .displays = mFrontEndDisplayInfos,
            .globalShadowSettings = globalShadowSettings,
    };
    LayerSnapshotBuilder builder(args);
}

TEST_F(LayerSnapshotTest, updateSnapshot) {
    LayerSnapshotBuilder::Args args{
            .root = mHierarchyBuilder.getHierarchy(),
            .layerLifecycleManager = mLifecycleManager,
            .includeMetadata = false,
            .displays = mFrontEndDisplayInfos,
            .globalShadowSettings = globalShadowSettings,
    };

    LayerSnapshotBuilder builder;
    builder.update(args);
}

// update using parent snapshot data
TEST_F(LayerSnapshotTest, croppedByParent) {
    /// MAKE ALL LAYERS VISIBLE BY DEFAULT
    DisplayInfo info;
    info.info.logicalHeight = 100;
    info.info.logicalWidth = 200;
    mFrontEndDisplayInfos.emplace_or_replace(ui::LayerStack::fromValue(1), info);
    Rect layerCrop(0, 0, 10, 20);
    setCrop(11, layerCrop);
    EXPECT_TRUE(mLifecycleManager.getGlobalChanges().test(RequestedLayerState::Changes::Geometry));
    UPDATE_AND_VERIFY_WITH_DISPLAY_CHANGES(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(11)->geomCrop, layerCrop);
    EXPECT_EQ(getSnapshot(111)->geomLayerBounds, layerCrop.toFloatRect());
    float maxHeight = static_cast<float>(info.info.logicalHeight * 10);
    float maxWidth = static_cast<float>(info.info.logicalWidth * 10);

    FloatRect maxDisplaySize(-maxWidth, -maxHeight, maxWidth, maxHeight);
    EXPECT_EQ(getSnapshot(1)->geomLayerBounds, maxDisplaySize);
}

// visibility tests
TEST_F(LayerSnapshotTest, newLayerHiddenByPolicy) {
    createLayer(112, 11);
    hideLayer(112);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    showLayer(112);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 11, 111, 112, 12, 121, 122, 1221, 13, 2});
}

TEST_F(LayerSnapshotTest, hiddenByParent) {
    hideLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 13, 2});
}

TEST_F(LayerSnapshotTest, reparentShowsChild) {
    hideLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 13, 2});

    showLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
}

TEST_F(LayerSnapshotTest, reparentHidesChild) {
    hideLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 13, 2});

    reparentLayer(121, 11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 122, 1221, 13, 2});
}

TEST_F(LayerSnapshotTest, unHidingUpdatesSnapshot) {
    hideLayer(11);
    Rect crop(1, 2, 3, 4);
    setCrop(111, crop);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 13, 2});

    showLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(111)->geomLayerBounds, crop.toFloatRect());
}

TEST_F(LayerSnapshotTest, childBehindParentCanBeHiddenByParent) {
    setZ(111, -1);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 111, 11, 12, 121, 122, 1221, 13, 2});

    hideLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 13, 2});
}

// relative tests
TEST_F(LayerSnapshotTest, RelativeParentCanHideChild) {
    reparentRelativeLayer(13, 11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 11, 13, 111, 12, 121, 122, 1221, 2});

    hideLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 2});
}

TEST_F(LayerSnapshotTest, ReparentingToHiddenRelativeParentHidesChild) {
    hideLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 13, 2});
    reparentRelativeLayer(13, 11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 2});
}

TEST_F(LayerSnapshotTest, AlphaInheritedByChildren) {
    setAlpha(1, 0.5);
    setAlpha(122, 0.5);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(12)->alpha, 0.5f);
    EXPECT_EQ(getSnapshot(1221)->alpha, 0.25f);
}

// Change states
TEST_F(LayerSnapshotTest, UpdateClearsPreviousChangeStates) {
    setCrop(1, Rect(1, 2, 3, 4));
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot(1)->changes.get() != 0);
    EXPECT_TRUE(getSnapshot(11)->changes.get() != 0);
    setCrop(2, Rect(1, 2, 3, 4));
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot(2)->changes.get() != 0);
    EXPECT_TRUE(getSnapshot(1)->changes.get() == 0);
    EXPECT_TRUE(getSnapshot(11)->changes.get() == 0);
}

TEST_F(LayerSnapshotTest, FastPathClearsPreviousChangeStates) {
    setColor(11, {1._hf, 0._hf, 0._hf});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot(11)->changes.get() != 0);
    EXPECT_TRUE(getSnapshot(1)->changes.get() == 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot(11)->changes.get() == 0);
}

TEST_F(LayerSnapshotTest, FastPathSetsChangeFlagToContent) {
    setColor(1, {1._hf, 0._hf, 0._hf});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(1)->changes, RequestedLayerState::Changes::Content);
}

} // namespace android::surfaceflinger::frontend
