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

#undef LOG_TAG
#define LOG_TAG "FlagManagerTest"

#include <common/FlagManager.h>
#include "FlagUtils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <log/log.h>

#include <com_android_graphics_surfaceflinger_flags.h>

namespace android {

using testing::Return;

class TestableFlagManager : public FlagManager {
public:
    TestableFlagManager() : FlagManager(ConstructorTag{}) { markBootCompleted(); }
    ~TestableFlagManager() = default;

    MOCK_METHOD(std::optional<bool>, getBoolProperty, (const char*), (const, override));
    MOCK_METHOD(bool, getServerConfigurableFlag, (const char*), (const, override));

    void markBootIncomplete() { mBootCompleted = false; }
};

class FlagManagerTest : public testing::Test {
public:
    FlagManagerTest() {
        const ::testing::TestInfo* const test_info =
                ::testing::UnitTest::GetInstance()->current_test_info();
        ALOGD("**** Setting up for %s.%s\n", test_info->test_case_name(), test_info->name());
    }
    ~FlagManagerTest() override {
        const ::testing::TestInfo* const test_info =
                ::testing::UnitTest::GetInstance()->current_test_info();
        ALOGD("**** Tearing down after %s.%s\n", test_info->test_case_name(), test_info->name());
    }

    TestableFlagManager mFlagManager;
};

TEST_F(FlagManagerTest, isSingleton) {
    EXPECT_EQ(&FlagManager::getInstance(), &FlagManager::getInstance());
}

TEST_F(FlagManagerTest, legacyCreashesIfQueriedBeforeBoot) {
    mFlagManager.markBootIncomplete();
    EXPECT_DEATH(FlagManager::getInstance().test_flag(), "");
}

TEST_F(FlagManagerTest, legacyReturnsOverride) {
    EXPECT_CALL(mFlagManager, getBoolProperty).WillOnce(Return(true));
    EXPECT_EQ(true, mFlagManager.test_flag());

    EXPECT_CALL(mFlagManager, getBoolProperty).WillOnce(Return(false));
    EXPECT_EQ(false, mFlagManager.test_flag());
}

TEST_F(FlagManagerTest, legacyReturnsValue) {
    EXPECT_CALL(mFlagManager, getBoolProperty).WillRepeatedly(Return(std::nullopt));

    EXPECT_CALL(mFlagManager, getServerConfigurableFlag).WillOnce(Return(true));
    EXPECT_EQ(true, mFlagManager.test_flag());

    EXPECT_CALL(mFlagManager, getServerConfigurableFlag).WillOnce(Return(false));
    EXPECT_EQ(false, mFlagManager.test_flag());
}

TEST_F(FlagManagerTest, creashesIfQueriedBeforeBoot) {
    mFlagManager.markBootIncomplete();
    EXPECT_DEATH(FlagManager::getInstance().late_boot_misc2(), "");
}

TEST_F(FlagManagerTest, returnsOverride) {
    mFlagManager.setUnitTestMode();

    // Twice, since the first call is to initialize the static variable
    EXPECT_CALL(mFlagManager, getBoolProperty)
            .Times((2))
            .WillOnce(Return(true))
            .WillOnce(Return(true));
    EXPECT_EQ(true, mFlagManager.late_boot_misc2());

    EXPECT_CALL(mFlagManager, getBoolProperty).WillOnce(Return(false));
    EXPECT_EQ(false, mFlagManager.late_boot_misc2());
}

TEST_F(FlagManagerTest, returnsValue) {
    mFlagManager.setUnitTestMode();

    EXPECT_CALL(mFlagManager, getBoolProperty).WillRepeatedly(Return(std::nullopt));

    {
        SET_FLAG_FOR_TEST(com::android::graphics::surfaceflinger::flags::late_boot_misc2, true);
        EXPECT_EQ(true, mFlagManager.late_boot_misc2());
    }

    {
        SET_FLAG_FOR_TEST(com::android::graphics::surfaceflinger::flags::late_boot_misc2, false);
        EXPECT_EQ(false, mFlagManager.late_boot_misc2());
    }
}

TEST_F(FlagManagerTest, readonlyReturnsOverride) {
    mFlagManager.setUnitTestMode();

    // Twice, since the first call is to initialize the static variable
    EXPECT_CALL(mFlagManager, getBoolProperty)
            .Times(2)
            .WillOnce(Return(true))
            .WillOnce(Return(true));
    EXPECT_EQ(true, mFlagManager.misc1());

    EXPECT_CALL(mFlagManager, getBoolProperty).WillOnce(Return(false));
    EXPECT_EQ(false, mFlagManager.misc1());
}

TEST_F(FlagManagerTest, readonlyReturnsValue) {
    mFlagManager.setUnitTestMode();

    EXPECT_CALL(mFlagManager, getBoolProperty).WillRepeatedly(Return(std::nullopt));

    {
        SET_FLAG_FOR_TEST(com::android::graphics::surfaceflinger::flags::misc1, true);
        EXPECT_EQ(true, mFlagManager.misc1());
    }

    {
        SET_FLAG_FOR_TEST(com::android::graphics::surfaceflinger::flags::misc1, false);
        EXPECT_EQ(false, mFlagManager.misc1());
    }
}

TEST_F(FlagManagerTest, dontSkipOnEarlyIsNotCached) {
    EXPECT_CALL(mFlagManager, getBoolProperty).WillRepeatedly(Return(std::nullopt));

    const auto initialValue = com::android::graphics::surfaceflinger::flags::dont_skip_on_early();

    com::android::graphics::surfaceflinger::flags::dont_skip_on_early(true);
    EXPECT_EQ(true, mFlagManager.dont_skip_on_early());

    com::android::graphics::surfaceflinger::flags::dont_skip_on_early(false);
    EXPECT_EQ(false, mFlagManager.dont_skip_on_early());

    com::android::graphics::surfaceflinger::flags::dont_skip_on_early(initialValue);
}

} // namespace android
