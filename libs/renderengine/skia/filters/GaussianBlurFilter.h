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

#pragma once

#include "BlurFilter.h"
#include <SkCanvas.h>
#include <SkImage.h>
#include <SkRuntimeEffect.h>
#include <SkSurface.h>

using namespace std;

namespace android {
namespace renderengine {
namespace skia {

/**
 * This is an implementation of a Gaussian blur using Skia's built-in GaussianBlur filter.
 */
class GaussianBlurFilter: public BlurFilter {
public:
    explicit GaussianBlurFilter();
    virtual ~GaussianBlurFilter(){}

    // Execute blur, saving it to a texture
    sk_sp<SkImage> generate(GrRecordingContext* context, const uint32_t radius,
                            const sk_sp<SkImage> blurInput, const SkRect& blurRect) const override;

};

} // namespace skia
} // namespace renderengine
} // namespace android
