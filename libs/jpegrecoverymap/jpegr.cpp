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

#include <jpegrecoverymap/jpegr.h>
#include <jpegrecoverymap/jpegencoderhelper.h>
#include <jpegrecoverymap/jpegdecoderhelper.h>
#include <jpegrecoverymap/recoverymapmath.h>
#include <jpegrecoverymap/jpegrutils.h>

#include <image_io/jpeg/jpeg_marker.h>
#include <image_io/jpeg/jpeg_info.h>
#include <image_io/jpeg/jpeg_scanner.h>
#include <image_io/jpeg/jpeg_info_builder.h>
#include <image_io/base/data_segment_data_source.h>
#include <utils/Log.h>
#include "SkColorSpace.h"
#include "SkICC.h"

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace std;
using namespace photos_editing_formats::image_io;

namespace android::jpegrecoverymap {

#define USE_SRGB_INVOETF_LUT 1
#define USE_HLG_OETF_LUT 1
#define USE_PQ_OETF_LUT 1
#define USE_HLG_INVOETF_LUT 1
#define USE_PQ_INVOETF_LUT 1
#define USE_APPLY_RECOVERY_LUT 1

#define JPEGR_CHECK(x)          \
  {                             \
    status_t status = (x);      \
    if ((status) != NO_ERROR) { \
      return status;            \
    }                           \
  }

// The current JPEGR version that we encode to
static const uint32_t kJpegrVersion = 1;

// Map is quarter res / sixteenth size
static const size_t kMapDimensionScaleFactor = 4;
// JPEG block size.
// JPEG encoding / decoding will require 8 x 8 DCT transform.
// Width must be 8 dividable, and height must be 2 dividable.
static const size_t kJpegBlock = 8;
// JPEG compress quality (0 ~ 100) for recovery map
static const int kMapCompressQuality = 85;

#define CONFIG_MULTITHREAD 1
int GetCPUCoreCount() {
  int cpuCoreCount = 1;
#if CONFIG_MULTITHREAD
#if defined(_SC_NPROCESSORS_ONLN)
  cpuCoreCount = sysconf(_SC_NPROCESSORS_ONLN);
#else
  // _SC_NPROC_ONLN must be defined...
  cpuCoreCount = sysconf(_SC_NPROC_ONLN);
#endif
#endif
  return cpuCoreCount;
}

static const map<jpegrecoverymap::jpegr_color_gamut, skcms_Matrix3x3> jrGamut_to_skGamut {
    {JPEGR_COLORGAMUT_BT709,     SkNamedGamut::kSRGB},
    {JPEGR_COLORGAMUT_P3,        SkNamedGamut::kDisplayP3},
    {JPEGR_COLORGAMUT_BT2100,    SkNamedGamut::kRec2020},
};

static const map<
        jpegrecoverymap::jpegr_transfer_function,
        skcms_TransferFunction> jrTransFunc_to_skTransFunc {
    {JPEGR_TF_SRGB,        SkNamedTransferFn::kSRGB},
    {JPEGR_TF_LINEAR,      SkNamedTransferFn::kLinear},
    {JPEGR_TF_HLG,         SkNamedTransferFn::kHLG},
    {JPEGR_TF_PQ,          SkNamedTransferFn::kPQ},
};

/* Encode API-0 */
status_t JpegR::encodeJPEGR(jr_uncompressed_ptr uncompressed_p010_image,
                                  jpegr_transfer_function hdr_tf,
                                  jr_compressed_ptr dest,
                                  int quality,
                                  jr_exif_ptr exif) {
  if (uncompressed_p010_image == nullptr || dest == nullptr) {
    return ERROR_JPEGR_INVALID_NULL_PTR;
  }

  if (quality < 0 || quality > 100) {
    return ERROR_JPEGR_INVALID_INPUT_TYPE;
  }

  if (uncompressed_p010_image->width % kJpegBlock != 0
          || uncompressed_p010_image->height % 2 != 0) {
    ALOGE("Image size can not be handled: %dx%d",
            uncompressed_p010_image->width, uncompressed_p010_image->height);
    return ERROR_JPEGR_INVALID_INPUT_TYPE;
  }

  jpegr_metadata metadata;
  metadata.version = kJpegrVersion;

  jpegr_uncompressed_struct uncompressed_yuv_420_image;
  unique_ptr<uint8_t[]> uncompressed_yuv_420_image_data = make_unique<uint8_t[]>(
      uncompressed_p010_image->width * uncompressed_p010_image->height * 3 / 2);
  uncompressed_yuv_420_image.data = uncompressed_yuv_420_image_data.get();
  JPEGR_CHECK(toneMap(uncompressed_p010_image, &uncompressed_yuv_420_image));

  jpegr_uncompressed_struct map;
  JPEGR_CHECK(generateRecoveryMap(
      &uncompressed_yuv_420_image, uncompressed_p010_image, hdr_tf, &metadata, &map));
  std::unique_ptr<uint8_t[]> map_data;
  map_data.reset(reinterpret_cast<uint8_t*>(map.data));

  jpegr_compressed_struct compressed_map;
  compressed_map.maxLength = map.width * map.height;
  unique_ptr<uint8_t[]> compressed_map_data = make_unique<uint8_t[]>(compressed_map.maxLength);
  compressed_map.data = compressed_map_data.get();
  JPEGR_CHECK(compressRecoveryMap(&map, &compressed_map));

  sk_sp<SkData> icc = SkWriteICCProfile(
          jrTransFunc_to_skTransFunc.at(JPEGR_TF_SRGB),
          jrGamut_to_skGamut.at(uncompressed_yuv_420_image.colorGamut));

  JpegEncoderHelper jpeg_encoder;
  if (!jpeg_encoder.compressImage(uncompressed_yuv_420_image.data,
                                  uncompressed_yuv_420_image.width,
                                  uncompressed_yuv_420_image.height, quality,
                                  icc.get()->data(), icc.get()->size())) {
    return ERROR_JPEGR_ENCODE_ERROR;
  }
  jpegr_compressed_struct jpeg;
  jpeg.data = jpeg_encoder.getCompressedImagePtr();
  jpeg.length = jpeg_encoder.getCompressedImageSize();

  JPEGR_CHECK(appendRecoveryMap(&jpeg, &compressed_map, exif, &metadata, dest));

  return NO_ERROR;
}

/* Encode API-1 */
status_t JpegR::encodeJPEGR(jr_uncompressed_ptr uncompressed_p010_image,
                                  jr_uncompressed_ptr uncompressed_yuv_420_image,
                                  jpegr_transfer_function hdr_tf,
                                  jr_compressed_ptr dest,
                                  int quality,
                                  jr_exif_ptr exif) {
  if (uncompressed_p010_image == nullptr
   || uncompressed_yuv_420_image == nullptr
   || dest == nullptr) {
    return ERROR_JPEGR_INVALID_NULL_PTR;
  }

  if (quality < 0 || quality > 100) {
    return ERROR_JPEGR_INVALID_INPUT_TYPE;
  }

  if (uncompressed_p010_image->width != uncompressed_yuv_420_image->width
   || uncompressed_p010_image->height != uncompressed_yuv_420_image->height) {
    return ERROR_JPEGR_RESOLUTION_MISMATCH;
  }

  if (uncompressed_p010_image->width % kJpegBlock != 0
          || uncompressed_p010_image->height % 2 != 0) {
    ALOGE("Image size can not be handled: %dx%d",
            uncompressed_p010_image->width, uncompressed_p010_image->height);
    return ERROR_JPEGR_INVALID_INPUT_TYPE;
  }

  jpegr_metadata metadata;
  metadata.version = kJpegrVersion;

  jpegr_uncompressed_struct map;
  JPEGR_CHECK(generateRecoveryMap(
      uncompressed_yuv_420_image, uncompressed_p010_image, hdr_tf, &metadata, &map));
  std::unique_ptr<uint8_t[]> map_data;
  map_data.reset(reinterpret_cast<uint8_t*>(map.data));

  jpegr_compressed_struct compressed_map;
  compressed_map.maxLength = map.width * map.height;
  unique_ptr<uint8_t[]> compressed_map_data = make_unique<uint8_t[]>(compressed_map.maxLength);
  compressed_map.data = compressed_map_data.get();
  JPEGR_CHECK(compressRecoveryMap(&map, &compressed_map));

  sk_sp<SkData> icc = SkWriteICCProfile(
          jrTransFunc_to_skTransFunc.at(JPEGR_TF_SRGB),
          jrGamut_to_skGamut.at(uncompressed_yuv_420_image->colorGamut));

  JpegEncoderHelper jpeg_encoder;
  if (!jpeg_encoder.compressImage(uncompressed_yuv_420_image->data,
                                  uncompressed_yuv_420_image->width,
                                  uncompressed_yuv_420_image->height, quality,
                                  icc.get()->data(), icc.get()->size())) {
    return ERROR_JPEGR_ENCODE_ERROR;
  }
  jpegr_compressed_struct jpeg;
  jpeg.data = jpeg_encoder.getCompressedImagePtr();
  jpeg.length = jpeg_encoder.getCompressedImageSize();

  JPEGR_CHECK(appendRecoveryMap(&jpeg, &compressed_map, exif, &metadata, dest));

  return NO_ERROR;
}

/* Encode API-2 */
status_t JpegR::encodeJPEGR(jr_uncompressed_ptr uncompressed_p010_image,
                                  jr_uncompressed_ptr uncompressed_yuv_420_image,
                                  jr_compressed_ptr compressed_jpeg_image,
                                  jpegr_transfer_function hdr_tf,
                                  jr_compressed_ptr dest) {
  if (uncompressed_p010_image == nullptr
   || uncompressed_yuv_420_image == nullptr
   || compressed_jpeg_image == nullptr
   || dest == nullptr) {
    return ERROR_JPEGR_INVALID_NULL_PTR;
  }

  if (uncompressed_p010_image->width != uncompressed_yuv_420_image->width
   || uncompressed_p010_image->height != uncompressed_yuv_420_image->height) {
    return ERROR_JPEGR_RESOLUTION_MISMATCH;
  }

  if (uncompressed_p010_image->width % kJpegBlock != 0
          || uncompressed_p010_image->height % 2 != 0) {
    ALOGE("Image size can not be handled: %dx%d",
            uncompressed_p010_image->width, uncompressed_p010_image->height);
    return ERROR_JPEGR_INVALID_INPUT_TYPE;
  }

  jpegr_metadata metadata;
  metadata.version = kJpegrVersion;

  jpegr_uncompressed_struct map;
  JPEGR_CHECK(generateRecoveryMap(
      uncompressed_yuv_420_image, uncompressed_p010_image, hdr_tf, &metadata, &map));
  std::unique_ptr<uint8_t[]> map_data;
  map_data.reset(reinterpret_cast<uint8_t*>(map.data));

  jpegr_compressed_struct compressed_map;
  compressed_map.maxLength = map.width * map.height;
  unique_ptr<uint8_t[]> compressed_map_data = make_unique<uint8_t[]>(compressed_map.maxLength);
  compressed_map.data = compressed_map_data.get();
  JPEGR_CHECK(compressRecoveryMap(&map, &compressed_map));

  JPEGR_CHECK(appendRecoveryMap(compressed_jpeg_image, &compressed_map, nullptr, &metadata, dest));

  return NO_ERROR;
}

/* Encode API-3 */
status_t JpegR::encodeJPEGR(jr_uncompressed_ptr uncompressed_p010_image,
                                  jr_compressed_ptr compressed_jpeg_image,
                                  jpegr_transfer_function hdr_tf,
                                  jr_compressed_ptr dest) {
  if (uncompressed_p010_image == nullptr
   || compressed_jpeg_image == nullptr
   || dest == nullptr) {
    return ERROR_JPEGR_INVALID_NULL_PTR;
  }

  if (uncompressed_p010_image->width % kJpegBlock != 0
          || uncompressed_p010_image->height % 2 != 0) {
    ALOGE("Image size can not be handled: %dx%d",
            uncompressed_p010_image->width, uncompressed_p010_image->height);
    return ERROR_JPEGR_INVALID_INPUT_TYPE;
  }

  JpegDecoderHelper jpeg_decoder;
  if (!jpeg_decoder.decompressImage(compressed_jpeg_image->data, compressed_jpeg_image->length)) {
    return ERROR_JPEGR_DECODE_ERROR;
  }
  jpegr_uncompressed_struct uncompressed_yuv_420_image;
  uncompressed_yuv_420_image.data = jpeg_decoder.getDecompressedImagePtr();
  uncompressed_yuv_420_image.width = jpeg_decoder.getDecompressedImageWidth();
  uncompressed_yuv_420_image.height = jpeg_decoder.getDecompressedImageHeight();
  uncompressed_yuv_420_image.colorGamut = compressed_jpeg_image->colorGamut;

  if (uncompressed_p010_image->width != uncompressed_yuv_420_image.width
   || uncompressed_p010_image->height != uncompressed_yuv_420_image.height) {
    return ERROR_JPEGR_RESOLUTION_MISMATCH;
  }

  jpegr_metadata metadata;
  metadata.version = kJpegrVersion;

  jpegr_uncompressed_struct map;
  JPEGR_CHECK(generateRecoveryMap(
      &uncompressed_yuv_420_image, uncompressed_p010_image, hdr_tf, &metadata, &map));
  std::unique_ptr<uint8_t[]> map_data;
  map_data.reset(reinterpret_cast<uint8_t*>(map.data));

  jpegr_compressed_struct compressed_map;
  compressed_map.maxLength = map.width * map.height;
  unique_ptr<uint8_t[]> compressed_map_data = make_unique<uint8_t[]>(compressed_map.maxLength);
  compressed_map.data = compressed_map_data.get();
  JPEGR_CHECK(compressRecoveryMap(&map, &compressed_map));

  JPEGR_CHECK(appendRecoveryMap(compressed_jpeg_image, &compressed_map, nullptr, &metadata, dest));

  return NO_ERROR;
}

status_t JpegR::getJPEGRInfo(jr_compressed_ptr compressed_jpegr_image,
                                   jr_info_ptr jpegr_info) {
  if (compressed_jpegr_image == nullptr || jpegr_info == nullptr) {
    return ERROR_JPEGR_INVALID_NULL_PTR;
  }

  jpegr_compressed_struct primary_image, recovery_map;
  JPEGR_CHECK(extractPrimaryImageAndRecoveryMap(compressed_jpegr_image,
                                                &primary_image, &recovery_map));

  JpegDecoderHelper jpeg_decoder;
  if (!jpeg_decoder.getCompressedImageParameters(primary_image.data, primary_image.length,
                                                 &jpegr_info->width, &jpegr_info->height,
                                                 jpegr_info->iccData, jpegr_info->exifData)) {
    return ERROR_JPEGR_DECODE_ERROR;
  }

  return NO_ERROR;
}

/* Decode API */
status_t JpegR::decodeJPEGR(jr_compressed_ptr compressed_jpegr_image,
                                  jr_uncompressed_ptr dest,
                                  jr_exif_ptr exif,
                                  bool request_sdr) {
  if (compressed_jpegr_image == nullptr || dest == nullptr) {
    return ERROR_JPEGR_INVALID_NULL_PTR;
  }
  // TODO: fill EXIF data
  (void) exif;

  if (request_sdr) {
    JpegDecoderHelper jpeg_decoder;
    if (!jpeg_decoder.decompressImage(compressed_jpegr_image->data, compressed_jpegr_image->length,
                                      true)) {
        return ERROR_JPEGR_DECODE_ERROR;
    }
    jpegr_uncompressed_struct uncompressed_rgba_image;
    uncompressed_rgba_image.data = jpeg_decoder.getDecompressedImagePtr();
    uncompressed_rgba_image.width = jpeg_decoder.getDecompressedImageWidth();
    uncompressed_rgba_image.height = jpeg_decoder.getDecompressedImageHeight();
    memcpy(dest->data, uncompressed_rgba_image.data,
           uncompressed_rgba_image.width * uncompressed_rgba_image.height * 4);
    dest->width = uncompressed_rgba_image.width;
    dest->height = uncompressed_rgba_image.height;
    return NO_ERROR;
  }

  jpegr_compressed_struct compressed_map;
  jpegr_metadata metadata;
  JPEGR_CHECK(extractRecoveryMap(compressed_jpegr_image, &compressed_map));

  JpegDecoderHelper jpeg_decoder;
  if (!jpeg_decoder.decompressImage(compressed_jpegr_image->data, compressed_jpegr_image->length)) {
    return ERROR_JPEGR_DECODE_ERROR;
  }

  JpegDecoderHelper recovery_map_decoder;
  if (!recovery_map_decoder.decompressImage(compressed_map.data, compressed_map.length)) {
    return ERROR_JPEGR_DECODE_ERROR;
  }

  jpegr_uncompressed_struct map;
  map.data = recovery_map_decoder.getDecompressedImagePtr();
  map.width = recovery_map_decoder.getDecompressedImageWidth();
  map.height = recovery_map_decoder.getDecompressedImageHeight();

  jpegr_uncompressed_struct uncompressed_yuv_420_image;
  uncompressed_yuv_420_image.data = jpeg_decoder.getDecompressedImagePtr();
  uncompressed_yuv_420_image.width = jpeg_decoder.getDecompressedImageWidth();
  uncompressed_yuv_420_image.height = jpeg_decoder.getDecompressedImageHeight();

  if (!getMetadataFromXMP(static_cast<uint8_t*>(jpeg_decoder.getXMPPtr()),
                          jpeg_decoder.getXMPSize(), &metadata)) {
    return ERROR_JPEGR_DECODE_ERROR;
  }

  JPEGR_CHECK(applyRecoveryMap(&uncompressed_yuv_420_image, &map, &metadata, dest));
  return NO_ERROR;
}

status_t JpegR::compressRecoveryMap(jr_uncompressed_ptr uncompressed_recovery_map,
                                          jr_compressed_ptr dest) {
  if (uncompressed_recovery_map == nullptr || dest == nullptr) {
    return ERROR_JPEGR_INVALID_NULL_PTR;
  }

  JpegEncoderHelper jpeg_encoder;
  if (!jpeg_encoder.compressImage(uncompressed_recovery_map->data,
                                  uncompressed_recovery_map->width,
                                  uncompressed_recovery_map->height,
                                  kMapCompressQuality,
                                  nullptr,
                                  0,
                                  true /* isSingleChannel */)) {
    return ERROR_JPEGR_ENCODE_ERROR;
  }

  if (dest->maxLength < jpeg_encoder.getCompressedImageSize()) {
    return ERROR_JPEGR_BUFFER_TOO_SMALL;
  }

  memcpy(dest->data, jpeg_encoder.getCompressedImagePtr(), jpeg_encoder.getCompressedImageSize());
  dest->length = jpeg_encoder.getCompressedImageSize();
  dest->colorGamut = JPEGR_COLORGAMUT_UNSPECIFIED;

  return NO_ERROR;
}

const int kJobSzInRows = 16;
static_assert(kJobSzInRows > 0 && kJobSzInRows % kMapDimensionScaleFactor == 0,
              "align job size to kMapDimensionScaleFactor");

class JobQueue {
 public:
  bool dequeueJob(size_t& rowStart, size_t& rowEnd);
  void enqueueJob(size_t rowStart, size_t rowEnd);
  void markQueueForEnd();
  void reset();

 private:
  bool mQueuedAllJobs = false;
  std::deque<std::tuple<size_t, size_t>> mJobs;
  std::mutex mMutex;
  std::condition_variable mCv;
};

bool JobQueue::dequeueJob(size_t& rowStart, size_t& rowEnd) {
  std::unique_lock<std::mutex> lock{mMutex};
  while (true) {
    if (mJobs.empty()) {
      if (mQueuedAllJobs) {
        return false;
      } else {
        mCv.wait(lock);
      }
    } else {
      auto it = mJobs.begin();
      rowStart = std::get<0>(*it);
      rowEnd = std::get<1>(*it);
      mJobs.erase(it);
      return true;
    }
  }
  return false;
}

void JobQueue::enqueueJob(size_t rowStart, size_t rowEnd) {
  std::unique_lock<std::mutex> lock{mMutex};
  mJobs.push_back(std::make_tuple(rowStart, rowEnd));
  lock.unlock();
  mCv.notify_one();
}

void JobQueue::markQueueForEnd() {
  std::unique_lock<std::mutex> lock{mMutex};
  mQueuedAllJobs = true;
}

void JobQueue::reset() {
  std::unique_lock<std::mutex> lock{mMutex};
  mJobs.clear();
  mQueuedAllJobs = false;
}

status_t JpegR::generateRecoveryMap(jr_uncompressed_ptr uncompressed_yuv_420_image,
                                          jr_uncompressed_ptr uncompressed_p010_image,
                                          jpegr_transfer_function hdr_tf,
                                          jr_metadata_ptr metadata,
                                          jr_uncompressed_ptr dest) {
  if (uncompressed_yuv_420_image == nullptr
   || uncompressed_p010_image == nullptr
   || metadata == nullptr
   || dest == nullptr) {
    return ERROR_JPEGR_INVALID_NULL_PTR;
  }

  if (uncompressed_yuv_420_image->width != uncompressed_p010_image->width
   || uncompressed_yuv_420_image->height != uncompressed_p010_image->height) {
    return ERROR_JPEGR_RESOLUTION_MISMATCH;
  }

  if (uncompressed_yuv_420_image->colorGamut == JPEGR_COLORGAMUT_UNSPECIFIED
   || uncompressed_p010_image->colorGamut == JPEGR_COLORGAMUT_UNSPECIFIED) {
    return ERROR_JPEGR_INVALID_COLORGAMUT;
  }

  size_t image_width = uncompressed_yuv_420_image->width;
  size_t image_height = uncompressed_yuv_420_image->height;
  size_t map_width = image_width / kMapDimensionScaleFactor;
  size_t map_height = image_height / kMapDimensionScaleFactor;
  size_t map_stride = static_cast<size_t>(
          floor((map_width + kJpegBlock - 1) / kJpegBlock)) * kJpegBlock;
  size_t map_height_aligned = ((map_height + 1) >> 1) << 1;

  dest->width = map_stride;
  dest->height = map_height_aligned;
  dest->colorGamut = JPEGR_COLORGAMUT_UNSPECIFIED;
  dest->data = new uint8_t[map_stride * map_height_aligned];
  std::unique_ptr<uint8_t[]> map_data;
  map_data.reset(reinterpret_cast<uint8_t*>(dest->data));

  ColorTransformFn hdrInvOetf = nullptr;
  float hdr_white_nits = 0.0f;
  switch (hdr_tf) {
    case JPEGR_TF_LINEAR:
      hdrInvOetf = identityConversion;
      break;
    case JPEGR_TF_HLG:
#if USE_HLG_INVOETF_LUT
      hdrInvOetf = hlgInvOetfLUT;
#else
      hdrInvOetf = hlgInvOetf;
#endif
      hdr_white_nits = kHlgMaxNits;
      break;
    case JPEGR_TF_PQ:
#if USE_PQ_INVOETF_LUT
      hdrInvOetf = pqInvOetfLUT;
#else
      hdrInvOetf = pqInvOetf;
#endif
      hdr_white_nits = kPqMaxNits;
      break;
    default:
      // Should be impossible to hit after input validation.
      return ERROR_JPEGR_INVALID_TRANS_FUNC;
  }

  ColorTransformFn hdrGamutConversionFn = getHdrConversionFn(
      uncompressed_yuv_420_image->colorGamut, uncompressed_p010_image->colorGamut);

  ColorCalculationFn luminanceFn = nullptr;
  switch (uncompressed_yuv_420_image->colorGamut) {
    case JPEGR_COLORGAMUT_BT709:
      luminanceFn = srgbLuminance;
      break;
    case JPEGR_COLORGAMUT_P3:
      luminanceFn = p3Luminance;
      break;
    case JPEGR_COLORGAMUT_BT2100:
      luminanceFn = bt2100Luminance;
      break;
    case JPEGR_COLORGAMUT_UNSPECIFIED:
      // Should be impossible to hit after input validation.
      return ERROR_JPEGR_INVALID_COLORGAMUT;
  }

  std::mutex mutex;
  float max_gain = 0.0f;
  float min_gain = 1.0f;
  const int threads = std::clamp(GetCPUCoreCount(), 1, 4);
  size_t rowStep = threads == 1 ? image_height : kJobSzInRows;
  JobQueue jobQueue;

  std::function<void()> computeMetadata = [uncompressed_p010_image, uncompressed_yuv_420_image,
                                           hdrInvOetf, hdrGamutConversionFn, luminanceFn,
                                           hdr_white_nits, threads, &mutex, &max_gain, &min_gain,
                                           &jobQueue]() -> void {
    size_t rowStart, rowEnd;
    float max_gain_th = 0.0f;
    float min_gain_th = 1.0f;

    while (jobQueue.dequeueJob(rowStart, rowEnd)) {
      for (size_t y = rowStart; y < rowEnd; ++y) {
        for (size_t x = 0; x < uncompressed_p010_image->width; ++x) {
          Color hdr_yuv_gamma = getP010Pixel(uncompressed_p010_image, x, y);
          Color hdr_rgb_gamma = bt2100YuvToRgb(hdr_yuv_gamma);
          Color hdr_rgb = hdrInvOetf(hdr_rgb_gamma);
          hdr_rgb = hdrGamutConversionFn(hdr_rgb);
          float hdr_y_nits = luminanceFn(hdr_rgb) * hdr_white_nits;

          Color sdr_yuv_gamma =
              getYuv420Pixel(uncompressed_yuv_420_image, x, y);
          Color sdr_rgb_gamma = srgbYuvToRgb(sdr_yuv_gamma);
#if USE_SRGB_INVOETF_LUT
          Color sdr_rgb = srgbInvOetfLUT(sdr_rgb_gamma);
#else
          Color sdr_rgb = srgbInvOetf(sdr_rgb_gamma);
#endif
          float sdr_y_nits = luminanceFn(sdr_rgb) * kSdrWhiteNits;

          float gain = hdr_y_nits / sdr_y_nits;
          max_gain_th = std::max(max_gain_th, gain);
          min_gain_th = std::min(min_gain_th, gain);
        }
      }
    }
    std::unique_lock<std::mutex> lock{mutex};
    max_gain = std::max(max_gain, max_gain_th);
    min_gain = std::min(min_gain, min_gain_th);
  };

  std::function<void()> generateMap = [uncompressed_yuv_420_image, uncompressed_p010_image,
                                       metadata, dest, hdrInvOetf, hdrGamutConversionFn,
                                       luminanceFn, hdr_white_nits, &jobQueue]() -> void {
    size_t rowStart, rowEnd;
    size_t dest_map_width = uncompressed_yuv_420_image->width / kMapDimensionScaleFactor;
    size_t dest_map_stride = dest->width;
    while (jobQueue.dequeueJob(rowStart, rowEnd)) {
      for (size_t y = rowStart; y < rowEnd; ++y) {
        for (size_t x = 0; x < dest_map_width; ++x) {
          Color sdr_yuv_gamma =
              sampleYuv420(uncompressed_yuv_420_image, kMapDimensionScaleFactor, x, y);
          Color sdr_rgb_gamma = srgbYuvToRgb(sdr_yuv_gamma);
#if USE_SRGB_INVOETF_LUT
          Color sdr_rgb = srgbInvOetfLUT(sdr_rgb_gamma);
#else
          Color sdr_rgb = srgbInvOetf(sdr_rgb_gamma);
#endif
          float sdr_y_nits = luminanceFn(sdr_rgb) * kSdrWhiteNits;

          Color hdr_yuv_gamma = sampleP010(uncompressed_p010_image, kMapDimensionScaleFactor, x, y);
          Color hdr_rgb_gamma = bt2100YuvToRgb(hdr_yuv_gamma);
          Color hdr_rgb = hdrInvOetf(hdr_rgb_gamma);
          hdr_rgb = hdrGamutConversionFn(hdr_rgb);
          float hdr_y_nits = luminanceFn(hdr_rgb) * hdr_white_nits;

          size_t pixel_idx = x + y * dest_map_stride;
          reinterpret_cast<uint8_t*>(dest->data)[pixel_idx] =
              encodeRecovery(sdr_y_nits, hdr_y_nits, metadata);
        }
      }
    }
  };

  std::vector<std::thread> workers;
  for (int th = 0; th < threads - 1; th++) {
    workers.push_back(std::thread(computeMetadata));
  }

  // compute metadata
  for (size_t rowStart = 0; rowStart < image_height;) {
    size_t rowEnd = std::min(rowStart + rowStep, image_height);
    jobQueue.enqueueJob(rowStart, rowEnd);
    rowStart = rowEnd;
  }
  jobQueue.markQueueForEnd();
  computeMetadata();
  std::for_each(workers.begin(), workers.end(), [](std::thread& t) { t.join(); });
  workers.clear();

  metadata->maxContentBoost = max_gain;
  metadata->minContentBoost = min_gain;

  // generate map
  jobQueue.reset();
  for (int th = 0; th < threads - 1; th++) {
    workers.push_back(std::thread(generateMap));
  }

  rowStep = (threads == 1 ? image_height : kJobSzInRows) / kMapDimensionScaleFactor;
  for (size_t rowStart = 0; rowStart < map_height;) {
    size_t rowEnd = std::min(rowStart + rowStep, map_height);
    jobQueue.enqueueJob(rowStart, rowEnd);
    rowStart = rowEnd;
  }
  jobQueue.markQueueForEnd();
  generateMap();
  std::for_each(workers.begin(), workers.end(), [](std::thread& t) { t.join(); });

  map_data.release();
  return NO_ERROR;
}

status_t JpegR::applyRecoveryMap(jr_uncompressed_ptr uncompressed_yuv_420_image,
                                       jr_uncompressed_ptr uncompressed_recovery_map,
                                       jr_metadata_ptr metadata,
                                       jr_uncompressed_ptr dest) {
  if (uncompressed_yuv_420_image == nullptr
   || uncompressed_recovery_map == nullptr
   || metadata == nullptr
   || dest == nullptr) {
    return ERROR_JPEGR_INVALID_NULL_PTR;
  }

  dest->width = uncompressed_yuv_420_image->width;
  dest->height = uncompressed_yuv_420_image->height;
  ShepardsIDW idwTable(kMapDimensionScaleFactor);
  RecoveryLUT recoveryLUT(metadata);

  JobQueue jobQueue;
  std::function<void()> applyRecMap = [uncompressed_yuv_420_image, uncompressed_recovery_map,
                                       metadata, dest, &jobQueue, &idwTable,
                                       &recoveryLUT]() -> void {
    const float hdr_ratio = metadata->maxContentBoost;
    size_t width = uncompressed_yuv_420_image->width;
    size_t height = uncompressed_yuv_420_image->height;

#if USE_HLG_OETF_LUT
    ColorTransformFn hdrOetf = hlgOetfLUT;
#else
    ColorTransformFn hdrOetf = hlgOetf;
#endif

    size_t rowStart, rowEnd;
    while (jobQueue.dequeueJob(rowStart, rowEnd)) {
      for (size_t y = rowStart; y < rowEnd; ++y) {
        for (size_t x = 0; x < width; ++x) {
          Color yuv_gamma_sdr = getYuv420Pixel(uncompressed_yuv_420_image, x, y);
          Color rgb_gamma_sdr = srgbYuvToRgb(yuv_gamma_sdr);
#if USE_SRGB_INVOETF_LUT
          Color rgb_sdr = srgbInvOetfLUT(rgb_gamma_sdr);
#else
          Color rgb_sdr = srgbInvOetf(rgb_gamma_sdr);
#endif
          float recovery;
          // TODO: determine map scaling factor based on actual map dims
          size_t map_scale_factor = kMapDimensionScaleFactor;
          // TODO: If map_scale_factor is guaranteed to be an integer, then remove the following.
          // Currently map_scale_factor is of type size_t, but it could be changed to a float
          // later.
          if (map_scale_factor != floorf(map_scale_factor)) {
            recovery = sampleMap(uncompressed_recovery_map, map_scale_factor, x, y);
          } else {
            recovery = sampleMap(uncompressed_recovery_map, map_scale_factor, x, y, idwTable);
          }
#if USE_APPLY_RECOVERY_LUT
          Color rgb_hdr = applyRecoveryLUT(rgb_sdr, recovery, recoveryLUT);
#else
          Color rgb_hdr = applyRecovery(rgb_sdr, recovery, metadata);
#endif
          Color rgb_gamma_hdr = hdrOetf(rgb_hdr / metadata->maxContentBoost);
          uint32_t rgba1010102 = colorToRgba1010102(rgb_gamma_hdr);

          size_t pixel_idx = x + y * width;
          reinterpret_cast<uint32_t*>(dest->data)[pixel_idx] = rgba1010102;
        }
      }
    }
  };

  const int threads = std::clamp(GetCPUCoreCount(), 1, 4);
  std::vector<std::thread> workers;
  for (int th = 0; th < threads - 1; th++) {
    workers.push_back(std::thread(applyRecMap));
  }
  const int rowStep = threads == 1 ? uncompressed_yuv_420_image->height : kJobSzInRows;
  for (int rowStart = 0; rowStart < uncompressed_yuv_420_image->height;) {
    int rowEnd = std::min(rowStart + rowStep, uncompressed_yuv_420_image->height);
    jobQueue.enqueueJob(rowStart, rowEnd);
    rowStart = rowEnd;
  }
  jobQueue.markQueueForEnd();
  applyRecMap();
  std::for_each(workers.begin(), workers.end(), [](std::thread& t) { t.join(); });
  return NO_ERROR;
}

status_t JpegR::extractPrimaryImageAndRecoveryMap(jr_compressed_ptr compressed_jpegr_image,
                                                        jr_compressed_ptr primary_image,
                                                        jr_compressed_ptr recovery_map) {
  if (compressed_jpegr_image == nullptr) {
    return ERROR_JPEGR_INVALID_NULL_PTR;
  }

  MessageHandler msg_handler;
  std::shared_ptr<DataSegment> seg =
                  DataSegment::Create(DataRange(0, compressed_jpegr_image->length),
                                      static_cast<const uint8_t*>(compressed_jpegr_image->data),
                                      DataSegment::BufferDispositionPolicy::kDontDelete);
  DataSegmentDataSource data_source(seg);
  JpegInfoBuilder jpeg_info_builder;
  jpeg_info_builder.SetImageLimit(2);
  JpegScanner jpeg_scanner(&msg_handler);
  jpeg_scanner.Run(&data_source, &jpeg_info_builder);
  data_source.Reset();

  if (jpeg_scanner.HasError()) {
    return ERROR_JPEGR_INVALID_INPUT_TYPE;
  }

  const auto& jpeg_info = jpeg_info_builder.GetInfo();
  const auto& image_ranges = jpeg_info.GetImageRanges();
  if (image_ranges.empty()) {
    return ERROR_JPEGR_INVALID_INPUT_TYPE;
  }

  if (image_ranges.size() != 2) {
    // Must be 2 JPEG Images
    return ERROR_JPEGR_INVALID_INPUT_TYPE;
  }

  if (primary_image != nullptr) {
    primary_image->data = static_cast<uint8_t*>(compressed_jpegr_image->data) +
                                               image_ranges[0].GetBegin();
    primary_image->length = image_ranges[0].GetLength();
  }

  if (recovery_map != nullptr) {
    recovery_map->data = static_cast<uint8_t*>(compressed_jpegr_image->data) +
                                              image_ranges[1].GetBegin();
    recovery_map->length = image_ranges[1].GetLength();
  }

  return NO_ERROR;
}


status_t JpegR::extractRecoveryMap(jr_compressed_ptr compressed_jpegr_image,
                                         jr_compressed_ptr dest) {
  if (compressed_jpegr_image == nullptr || dest == nullptr) {
    return ERROR_JPEGR_INVALID_NULL_PTR;
  }

  return extractPrimaryImageAndRecoveryMap(compressed_jpegr_image, nullptr, dest);
}

// JPEG/R structure:
// SOI (ff d8)
//
// (Optional, only if EXIF package is from outside)
// APP1 (ff e1)
// 2 bytes of length (2 + length of exif package)
// EXIF package (this includes the first two bytes representing the package length)
//
// (Required, XMP package) APP1 (ff e1)
// 2 bytes of length (2 + 29 + length of xmp package)
// name space ("http://ns.adobe.com/xap/1.0/\0")
// xmp
//
// (Required) primary image (without the first two bytes (SOI), may have other packages)
//
// (Required) secondary image (the recovery map)
//
// Metadata versions we are using:
// ECMA TR-98 for JFIF marker
// Exif 2.2 spec for EXIF marker
// Adobe XMP spec part 3 for XMP marker
// ICC v4.3 spec for ICC
status_t JpegR::appendRecoveryMap(jr_compressed_ptr compressed_jpeg_image,
                                        jr_compressed_ptr compressed_recovery_map,
                                        jr_exif_ptr exif,
                                        jr_metadata_ptr metadata,
                                        jr_compressed_ptr dest) {
  if (compressed_jpeg_image == nullptr
   || compressed_recovery_map == nullptr
   || metadata == nullptr
   || dest == nullptr) {
    return ERROR_JPEGR_INVALID_NULL_PTR;
  }

  int pos = 0;

  // Write SOI
  JPEGR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kStart, 1, pos));
  JPEGR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kSOI, 1, pos));

  // Write EXIF
  if (exif != nullptr) {
    const int length = 2 + exif->length;
    const uint8_t lengthH = ((length >> 8) & 0xff);
    const uint8_t lengthL = (length & 0xff);
    JPEGR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kStart, 1, pos));
    JPEGR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kAPP1, 1, pos));
    JPEGR_CHECK(Write(dest, &lengthH, 1, pos));
    JPEGR_CHECK(Write(dest, &lengthL, 1, pos));
    JPEGR_CHECK(Write(dest, exif->data, exif->length, pos));
  }

  // Prepare and write XMP
  {
    const string xmp = generateXmp(compressed_recovery_map->length, *metadata);
    const string nameSpace = "http://ns.adobe.com/xap/1.0/\0";
    const int nameSpaceLength = nameSpace.size() + 1;  // need to count the null terminator
    // 2 bytes: representing the length of the package
    // 29 bytes: length of name space "http://ns.adobe.com/xap/1.0/\0",
    // x bytes: length of xmp packet
    const int length = 2 + nameSpaceLength + xmp.size();
    const uint8_t lengthH = ((length >> 8) & 0xff);
    const uint8_t lengthL = (length & 0xff);
    JPEGR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kStart, 1, pos));
    JPEGR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kAPP1, 1, pos));
    JPEGR_CHECK(Write(dest, &lengthH, 1, pos));
    JPEGR_CHECK(Write(dest, &lengthL, 1, pos));
    JPEGR_CHECK(Write(dest, (void*)nameSpace.c_str(), nameSpaceLength, pos));
    JPEGR_CHECK(Write(dest, (void*)xmp.c_str(), xmp.size(), pos));
  }

  // Write primary image
  JPEGR_CHECK(Write(dest,
      (uint8_t*)compressed_jpeg_image->data + 2, compressed_jpeg_image->length - 2, pos));

  // Write secondary image
  JPEGR_CHECK(Write(dest, compressed_recovery_map->data, compressed_recovery_map->length, pos));

  // Set back length
  dest->length = pos;

  // Done!
  return NO_ERROR;
}

status_t JpegR::toneMap(jr_uncompressed_ptr src,
                              jr_uncompressed_ptr dest) {
  if (src == nullptr || dest == nullptr) {
    return ERROR_JPEGR_INVALID_NULL_PTR;
  }

  dest->width = src->width;
  dest->height = src->height;

  size_t pixel_count = src->width * src->height;
  for (size_t y = 0; y < src->height; ++y) {
    for (size_t x = 0; x < src->width; ++x) {
      size_t pixel_y_idx = x + y * src->width;
      size_t pixel_uv_idx = x / 2 + (y / 2) * (src->width / 2);

      uint16_t y_uint = reinterpret_cast<uint16_t*>(src->data)[pixel_y_idx]
                        >> 6;
      uint16_t u_uint = reinterpret_cast<uint16_t*>(src->data)[pixel_count + pixel_uv_idx * 2]
                        >> 6;
      uint16_t v_uint = reinterpret_cast<uint16_t*>(src->data)[pixel_count + pixel_uv_idx * 2 + 1]
                        >> 6;

      uint8_t* y = &reinterpret_cast<uint8_t*>(dest->data)[pixel_y_idx];
      uint8_t* u = &reinterpret_cast<uint8_t*>(dest->data)[pixel_count + pixel_uv_idx];
      uint8_t* v = &reinterpret_cast<uint8_t*>(dest->data)[pixel_count * 5 / 4 + pixel_uv_idx];

      *y = static_cast<uint8_t>((y_uint >> 2) & 0xff);
      *u = static_cast<uint8_t>((u_uint >> 2) & 0xff);
      *v = static_cast<uint8_t>((v_uint >> 2) & 0xff);
    }
  }

  dest->colorGamut = src->colorGamut;

  return NO_ERROR;
}

} // namespace android::jpegrecoverymap
