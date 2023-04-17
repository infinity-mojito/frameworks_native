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

#ifndef ANDROID_ULTRAHDR_JPEGR_H
#define ANDROID_ULTRAHDR_JPEGR_H

#include "jpegrerrorcode.h"
#include "ultrahdr.h"

#ifndef FLT_MAX
#define FLT_MAX 0x1.fffffep127f
#endif

namespace android::ultrahdr {

struct jpegr_info_struct {
    size_t width;
    size_t height;
    std::vector<uint8_t>* iccData;
    std::vector<uint8_t>* exifData;
};

/*
 * Holds information for uncompressed image or gain map.
 */
struct jpegr_uncompressed_struct {
    // Pointer to the data location.
    void* data;
    // Width of the gain map or the luma plane of the image in pixels.
    int width;
    // Height of the gain map or the luma plane of the image in pixels.
    int height;
    // Color gamut.
    ultrahdr_color_gamut colorGamut;

    // Values below are optional
    // Pointer to chroma data, if it's NULL, chroma plane is considered to be immediately
    // following after the luma plane.
    // Note: currently this feature is only supported for P010 image (HDR input).
    void* chroma_data = nullptr;
    // Strides of Y plane in number of pixels, using 0 to present uninitialized, must be
    // larger than or equal to luma width.
    // Note: currently this feature is only supported for P010 image (HDR input).
    int luma_stride = 0;
    // Strides of UV plane in number of pixels, using 0 to present uninitialized, must be
    // larger than or equal to chroma width.
    // Note: currently this feature is only supported for P010 image (HDR input).
    int chroma_stride = 0;
};

/*
 * Holds information for compressed image or gain map.
 */
struct jpegr_compressed_struct {
    // Pointer to the data location.
    void* data;
    // Used data length in bytes.
    int length;
    // Maximum available data length in bytes.
    int maxLength;
    // Color gamut.
    ultrahdr_color_gamut colorGamut;
};

/*
 * Holds information for EXIF metadata.
 */
struct jpegr_exif_struct {
    // Pointer to the data location.
    void* data;
    // Data length;
    int length;
};

typedef struct jpegr_uncompressed_struct* jr_uncompressed_ptr;
typedef struct jpegr_compressed_struct* jr_compressed_ptr;
typedef struct jpegr_exif_struct* jr_exif_ptr;
typedef struct jpegr_info_struct* jr_info_ptr;

class JpegR {
public:
    /*
     * Experimental only
     *
     * Encode API-0
     * Compress JPEGR image from 10-bit HDR YUV.
     *
     * Tonemap the HDR input to a SDR image, generate gain map from the HDR and SDR images,
     * compress SDR YUV to 8-bit JPEG and append the gain map to the end of the compressed
     * JPEG.
     * @param uncompressed_p010_image uncompressed HDR image in P010 color format
     * @param hdr_tf transfer function of the HDR image
     * @param dest destination of the compressed JPEGR image
     * @param quality target quality of the JPEG encoding, must be in range of 0-100 where 100 is
     *                the highest quality
     * @param exif pointer to the exif metadata.
     * @return NO_ERROR if encoding succeeds, error code if error occurs.
     */
    status_t encodeJPEGR(jr_uncompressed_ptr uncompressed_p010_image,
                         ultrahdr_transfer_function hdr_tf,
                         jr_compressed_ptr dest,
                         int quality,
                         jr_exif_ptr exif);

    /*
     * Encode API-1
     * Compress JPEGR image from 10-bit HDR YUV and 8-bit SDR YUV.
     *
     * Generate gain map from the HDR and SDR inputs, compress SDR YUV to 8-bit JPEG and append
     * the gain map to the end of the compressed JPEG. HDR and SDR inputs must be the same
     * resolution.
     * @param uncompressed_p010_image uncompressed HDR image in P010 color format
     * @param uncompressed_yuv_420_image uncompressed SDR image in YUV_420 color format
     * @param hdr_tf transfer function of the HDR image
     * @param dest destination of the compressed JPEGR image
     * @param quality target quality of the JPEG encoding, must be in range of 0-100 where 100 is
     *                the highest quality
     * @param exif pointer to the exif metadata.
     * @return NO_ERROR if encoding succeeds, error code if error occurs.
     */
    status_t encodeJPEGR(jr_uncompressed_ptr uncompressed_p010_image,
                         jr_uncompressed_ptr uncompressed_yuv_420_image,
                         ultrahdr_transfer_function hdr_tf,
                         jr_compressed_ptr dest,
                         int quality,
                         jr_exif_ptr exif);

    /*
     * Encode API-2
     * Compress JPEGR image from 10-bit HDR YUV, 8-bit SDR YUV and compressed 8-bit JPEG.
     *
     * This method requires HAL Hardware JPEG encoder.
     *
     * Generate gain map from the HDR and SDR inputs, append the gain map to the end of the
     * compressed JPEG. HDR and SDR inputs must be the same resolution and color space.
     * @param uncompressed_p010_image uncompressed HDR image in P010 color format
     * @param uncompressed_yuv_420_image uncompressed SDR image in YUV_420 color format
     *                                   Note: the SDR image must be the decoded version of the JPEG
     *                                         input
     * @param compressed_jpeg_image compressed 8-bit JPEG image
     * @param hdr_tf transfer function of the HDR image
     * @param dest destination of the compressed JPEGR image
     * @return NO_ERROR if encoding succeeds, error code if error occurs.
     */
    status_t encodeJPEGR(jr_uncompressed_ptr uncompressed_p010_image,
                         jr_uncompressed_ptr uncompressed_yuv_420_image,
                         jr_compressed_ptr compressed_jpeg_image,
                         ultrahdr_transfer_function hdr_tf,
                         jr_compressed_ptr dest);

    /*
     * Encode API-3
     * Compress JPEGR image from 10-bit HDR YUV and 8-bit SDR YUV.
     *
     * This method requires HAL Hardware JPEG encoder.
     *
     * Decode the compressed 8-bit JPEG image to YUV SDR, generate gain map from the HDR input
     * and the decoded SDR result, append the gain map to the end of the compressed JPEG. HDR
     * and SDR inputs must be the same resolution.
     * @param uncompressed_p010_image uncompressed HDR image in P010 color format
     * @param compressed_jpeg_image compressed 8-bit JPEG image
     * @param hdr_tf transfer function of the HDR image
     * @param dest destination of the compressed JPEGR image
     * @return NO_ERROR if encoding succeeds, error code if error occurs.
     */
    status_t encodeJPEGR(jr_uncompressed_ptr uncompressed_p010_image,
                         jr_compressed_ptr compressed_jpeg_image,
                         ultrahdr_transfer_function hdr_tf,
                         jr_compressed_ptr dest);

    /*
     * Decode API
     * Decompress JPEGR image.
     *
     * @param compressed_jpegr_image compressed JPEGR image.
     * @param dest destination of the uncompressed JPEGR image.
     * @param max_display_boost (optional) the maximum available boost supported by a display,
     *                          the value must be greater than or equal to 1.0.
     * @param exif destination of the decoded EXIF metadata. The default value is NULL where the
                   decoder will do nothing about it. If configured not NULL the decoder will write
                   EXIF data into this structure. The format is defined in {@code jpegr_exif_struct}
     * @param output_format flag for setting output color format. Its value configures the output
                            color format. The default value is {@code JPEGR_OUTPUT_HDR_LINEAR}.
                            ----------------------------------------------------------------------
                            |      output_format       |    decoded color format to be written   |
                            ----------------------------------------------------------------------
                            |     JPEGR_OUTPUT_SDR     |                RGBA_8888                |
                            ----------------------------------------------------------------------
                            | JPEGR_OUTPUT_HDR_LINEAR  |        (default)RGBA_F16 linear         |
                            ----------------------------------------------------------------------
                            |   JPEGR_OUTPUT_HDR_PQ    |             RGBA_1010102 PQ             |
                            ----------------------------------------------------------------------
                            |   JPEGR_OUTPUT_HDR_HLG   |            RGBA_1010102 HLG             |
                            ----------------------------------------------------------------------
     * @param gain_map destination of the decoded gain map. The default value is NULL where
                           the decoder will do nothing about it. If configured not NULL the decoder
                           will write the decoded gain_map data into this structure. The format
                           is defined in {@code jpegr_uncompressed_struct}.
     * @param metadata destination of the decoded metadata. The default value is NULL where the
                       decoder will do nothing about it. If configured not NULL the decoder will
                       write metadata into this structure. the format of metadata is defined in
                       {@code ultrahdr_metadata_struct}.
     * @return NO_ERROR if decoding succeeds, error code if error occurs.
     */
    status_t decodeJPEGR(jr_compressed_ptr compressed_jpegr_image,
                         jr_uncompressed_ptr dest,
                         float max_display_boost = FLT_MAX,
                         jr_exif_ptr exif = nullptr,
                         ultrahdr_output_format output_format = ULTRAHDR_OUTPUT_HDR_LINEAR,
                         jr_uncompressed_ptr gain_map = nullptr,
                         ultrahdr_metadata_ptr metadata = nullptr);

    /*
    * Gets Info from JPEGR file without decoding it.
    *
    * The output is filled jpegr_info structure
    * @param compressed_jpegr_image compressed JPEGR image
    * @param jpegr_info pointer to output JPEGR info. Members of jpegr_info
    *         are owned by the caller
    * @return NO_ERROR if JPEGR parsing succeeds, error code otherwise
    */
    status_t getJPEGRInfo(jr_compressed_ptr compressed_jpegr_image,
                          jr_info_ptr jpegr_info);
protected:
    /*
     * This method is called in the encoding pipeline. It will take the uncompressed 8-bit and
     * 10-bit yuv images as input, and calculate the uncompressed gain map. The input images
     * must be the same resolution.
     *
     * @param uncompressed_yuv_420_image uncompressed SDR image in YUV_420 color format
     * @param uncompressed_p010_image uncompressed HDR image in P010 color format
     * @param hdr_tf transfer function of the HDR image
     * @param dest gain map; caller responsible for memory of data
     * @param metadata max_content_boost is filled in
     * @return NO_ERROR if calculation succeeds, error code if error occurs.
     */
    status_t generateGainMap(jr_uncompressed_ptr uncompressed_yuv_420_image,
                             jr_uncompressed_ptr uncompressed_p010_image,
                             ultrahdr_transfer_function hdr_tf,
                             ultrahdr_metadata_ptr metadata,
                             jr_uncompressed_ptr dest);

    /*
     * This method is called in the decoding pipeline. It will take the uncompressed (decoded)
     * 8-bit yuv image, the uncompressed (decoded) gain map, and extracted JPEG/R metadata as
     * input, and calculate the 10-bit recovered image. The recovered output image is the same
     * color gamut as the SDR image, with HLG transfer function, and is in RGBA1010102 data format.
     *
     * @param uncompressed_yuv_420_image uncompressed SDR image in YUV_420 color format
     * @param uncompressed_gain_map uncompressed gain map
     * @param metadata JPEG/R metadata extracted from XMP.
     * @param output_format flag for setting output color format. if set to
     *                      {@code JPEGR_OUTPUT_SDR}, decoder will only decode the primary image
     *                      which is SDR. Default value is JPEGR_OUTPUT_HDR_LINEAR.
     * @param max_display_boost the maximum available boost supported by a display
     * @param dest reconstructed HDR image
     * @return NO_ERROR if calculation succeeds, error code if error occurs.
     */
    status_t applyGainMap(jr_uncompressed_ptr uncompressed_yuv_420_image,
                          jr_uncompressed_ptr uncompressed_gain_map,
                          ultrahdr_metadata_ptr metadata,
                          ultrahdr_output_format output_format,
                          float max_display_boost,
                          jr_uncompressed_ptr dest);

private:
    /*
     * This method is called in the encoding pipeline. It will encode the gain map.
     *
     * @param uncompressed_gain_map uncompressed gain map
     * @param dest encoded recover map
     * @return NO_ERROR if encoding succeeds, error code if error occurs.
     */
    status_t compressGainMap(jr_uncompressed_ptr uncompressed_gain_map,
                             jr_compressed_ptr dest);

    /*
     * This methoud is called to separate primary image and gain map image from JPEGR
     *
     * @param compressed_jpegr_image compressed JPEGR image
     * @param primary_image destination of primary image
     * @param gain_map destination of compressed gain map
     * @return NO_ERROR if calculation succeeds, error code if error occurs.
    */
    status_t extractPrimaryImageAndGainMap(jr_compressed_ptr compressed_jpegr_image,
                                           jr_compressed_ptr primary_image,
                                           jr_compressed_ptr gain_map);
    /*
     * This method is called in the decoding pipeline. It will read XMP metadata to find the start
     * position of the compressed gain map, and will extract the compressed gain map.
     *
     * @param compressed_jpegr_image compressed JPEGR image
     * @param dest destination of compressed gain map
     * @return NO_ERROR if calculation succeeds, error code if error occurs.
     */
    status_t extractGainMap(jr_compressed_ptr compressed_jpegr_image,
                            jr_compressed_ptr dest);

    /*
     * This method is called in the encoding pipeline. It will take the standard 8-bit JPEG image,
     * the compressed gain map and optionally the exif package as inputs, and generate the XMP
     * metadata, and finally append everything in the order of:
     *     SOI, APP2(EXIF) (if EXIF is from outside), APP2(XMP), primary image, gain map
     * Note that EXIF package is only available for encoding API-0 and API-1. For encoding API-2 and
     * API-3 this parameter is null, but the primary image in JPEG/R may still have EXIF as long as
     * the input JPEG has EXIF.
     *
     * @param compressed_jpeg_image compressed 8-bit JPEG image
     * @param compress_gain_map compressed recover map
     * @param (nullable) exif EXIF package
     * @param metadata JPEG/R metadata to encode in XMP of the jpeg
     * @param dest compressed JPEGR image
     * @return NO_ERROR if calculation succeeds, error code if error occurs.
     */
    status_t appendGainMap(jr_compressed_ptr compressed_jpeg_image,
                           jr_compressed_ptr compressed_gain_map,
                           jr_exif_ptr exif,
                           ultrahdr_metadata_ptr metadata,
                           jr_compressed_ptr dest);

    /*
     * This method will tone map a HDR image to an SDR image.
     *
     * @param src (input) uncompressed P010 image
     * @param dest (output) tone mapping result as a YUV_420 image
     * @return NO_ERROR if calculation succeeds, error code if error occurs.
     */
    status_t toneMap(jr_uncompressed_ptr src,
                     jr_uncompressed_ptr dest);

    /*
     * This method will check the validity of the input images.
     *
     * @param uncompressed_p010_image uncompressed HDR image in P010 color format
     * @param uncompressed_yuv_420_image uncompressed SDR image in YUV_420 color format
     * @return NO_ERROR if the input images are valid, error code is not valid.
     */
    status_t areInputImagesValid(jr_uncompressed_ptr uncompressed_p010_image,
                                 jr_uncompressed_ptr uncompressed_yuv_420_image);
};

} // namespace android::ultrahdr

#endif // ANDROID_ULTRAHDR_JPEGR_H
