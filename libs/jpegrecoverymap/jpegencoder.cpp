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

#include <jpegrecoverymap/jpegencoder.h>

#include <cutils/log.h>

#include <errno.h>

namespace android::recoverymap {

// The destination manager that can access |mResultBuffer| in JpegEncoder.
struct destination_mgr {
public:
    struct jpeg_destination_mgr mgr;
    JpegEncoder* encoder;
};

JpegEncoder::JpegEncoder() {
}

JpegEncoder::~JpegEncoder() {
}

bool JpegEncoder::compressImage(const void* image, int width, int height, int quality,
                                   const void* iccBuffer, unsigned int iccSize) {
    if (width % 8 != 0 || height % 2 != 0) {
        ALOGE("Image size can not be handled: %dx%d", width, height);
        return false;
    }

    mResultBuffer.clear();
    if (!encode(image, width, height, quality, iccBuffer, iccSize)) {
        return false;
    }
    ALOGI("Compressed JPEG: %d[%dx%d] -> %zu bytes",
        (width * height * 12) / 8, width, height, mResultBuffer.size());
    return true;
}

const void* JpegEncoder::getCompressedImagePtr() {
    return mResultBuffer.data();
}

size_t JpegEncoder::getCompressedImageSize() {
    return mResultBuffer.size();
}

void JpegEncoder::initDestination(j_compress_ptr cinfo) {
    destination_mgr* dest = reinterpret_cast<destination_mgr*>(cinfo->dest);
    std::vector<JOCTET>& buffer = dest->encoder->mResultBuffer;
    buffer.resize(kBlockSize);
    dest->mgr.next_output_byte = &buffer[0];
    dest->mgr.free_in_buffer = buffer.size();
}

boolean JpegEncoder::emptyOutputBuffer(j_compress_ptr cinfo) {
    destination_mgr* dest = reinterpret_cast<destination_mgr*>(cinfo->dest);
    std::vector<JOCTET>& buffer = dest->encoder->mResultBuffer;
    size_t oldsize = buffer.size();
    buffer.resize(oldsize + kBlockSize);
    dest->mgr.next_output_byte = &buffer[oldsize];
    dest->mgr.free_in_buffer = kBlockSize;
    return true;
}

void JpegEncoder::terminateDestination(j_compress_ptr cinfo) {
    destination_mgr* dest = reinterpret_cast<destination_mgr*>(cinfo->dest);
    std::vector<JOCTET>& buffer = dest->encoder->mResultBuffer;
    buffer.resize(buffer.size() - dest->mgr.free_in_buffer);
}

void JpegEncoder::outputErrorMessage(j_common_ptr cinfo) {
    char buffer[JMSG_LENGTH_MAX];

    /* Create the message */
    (*cinfo->err->format_message) (cinfo, buffer);
    ALOGE("%s\n", buffer);
}

bool JpegEncoder::encode(const void* inYuv, int width, int height, int jpegQuality,
                           const void* iccBuffer, unsigned int iccSize) {
    jpeg_compress_struct cinfo;
    jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    // Override output_message() to print error log with ALOGE().
    cinfo.err->output_message = &outputErrorMessage;
    jpeg_create_compress(&cinfo);
    setJpegDestination(&cinfo);

    setJpegCompressStruct(width, height, jpegQuality, &cinfo);
    jpeg_start_compress(&cinfo, TRUE);

    if (iccBuffer != nullptr && iccSize > 0) {
        jpeg_write_marker(&cinfo, JPEG_APP0 + 2, static_cast<const JOCTET*>(iccBuffer), iccSize);
    }

    if (!compress(&cinfo, static_cast<const uint8_t*>(inYuv))) {
        return false;
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return true;
}

void JpegEncoder::setJpegDestination(jpeg_compress_struct* cinfo) {
    destination_mgr* dest = static_cast<struct destination_mgr *>((*cinfo->mem->alloc_small) (
            (j_common_ptr) cinfo, JPOOL_PERMANENT, sizeof(destination_mgr)));
    dest->encoder = this;
    dest->mgr.init_destination = &initDestination;
    dest->mgr.empty_output_buffer = &emptyOutputBuffer;
    dest->mgr.term_destination = &terminateDestination;
    cinfo->dest = reinterpret_cast<struct jpeg_destination_mgr*>(dest);
}

void JpegEncoder::setJpegCompressStruct(int width, int height, int quality,
                                               jpeg_compress_struct* cinfo) {
    cinfo->image_width = width;
    cinfo->image_height = height;
    cinfo->input_components = 3;
    cinfo->in_color_space = JCS_YCbCr;
    jpeg_set_defaults(cinfo);

    jpeg_set_quality(cinfo, quality, TRUE);
    jpeg_set_colorspace(cinfo, JCS_YCbCr);
    cinfo->raw_data_in = TRUE;
    cinfo->dct_method = JDCT_IFAST;

    // Configure sampling factors. The sampling factor is JPEG subsampling 420 because the
    // source format is YUV420.
    cinfo->comp_info[0].h_samp_factor = 2;
    cinfo->comp_info[0].v_samp_factor = 2;
    cinfo->comp_info[1].h_samp_factor = 1;
    cinfo->comp_info[1].v_samp_factor = 1;
    cinfo->comp_info[2].h_samp_factor = 1;
    cinfo->comp_info[2].v_samp_factor = 1;
}

bool JpegEncoder::compress(jpeg_compress_struct* cinfo, const uint8_t* yuv) {
    JSAMPROW y[kCompressBatchSize];
    JSAMPROW cb[kCompressBatchSize / 2];
    JSAMPROW cr[kCompressBatchSize / 2];
    JSAMPARRAY planes[3] {y, cb, cr};

    size_t y_plane_size = cinfo->image_width * cinfo->image_height;
    size_t uv_plane_size = y_plane_size / 4;
    uint8_t* y_plane = const_cast<uint8_t*>(yuv);
    uint8_t* u_plane = const_cast<uint8_t*>(yuv + y_plane_size);
    uint8_t* v_plane = const_cast<uint8_t*>(yuv + y_plane_size + uv_plane_size);
    std::unique_ptr<uint8_t[]> empty(new uint8_t[cinfo->image_width]);
    memset(empty.get(), 0, cinfo->image_width);

    while (cinfo->next_scanline < cinfo->image_height) {
        for (int i = 0; i < kCompressBatchSize; ++i) {
            size_t scanline = cinfo->next_scanline + i;
            if (scanline < cinfo->image_height) {
                y[i] = y_plane + scanline * cinfo->image_width;
            } else {
                y[i] = empty.get();
            }
        }
        // cb, cr only have half scanlines
        for (int i = 0; i < kCompressBatchSize / 2; ++i) {
            size_t scanline = cinfo->next_scanline / 2 + i;
            if (scanline < cinfo->image_height / 2) {
                int offset = scanline * (cinfo->image_width / 2);
                cb[i] = u_plane + offset;
                cr[i] = v_plane + offset;
            } else {
                cb[i] = cr[i] = empty.get();
            }
        }

        int processed = jpeg_write_raw_data(cinfo, planes, kCompressBatchSize);
        if (processed != kCompressBatchSize) {
            ALOGE("Number of processed lines does not equal input lines.");
            return false;
        }
    }
    return true;
}

} // namespace android