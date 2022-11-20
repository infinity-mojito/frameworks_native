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

#ifndef ANDROID_JPEGRECOVERYMAP_RECOVERYMAP_H
#define ANDROID_JPEGRECOVERYMAP_RECOVERYMAP_H

#include "jpegrerrorcode.h"

namespace android::recoverymap {

typedef enum {
  JPEGR_COLORGAMUT_UNSPECIFIED,
  JPEGR_COLORGAMUT_BT709,
  JPEGR_COLORGAMUT_P3,
  JPEGR_COLORGAMUT_BT2100,
} jpegr_color_gamut;

// Transfer functions as defined for XMP metadata
typedef enum {
  JPEGR_TF_HLG = 0,
  JPEGR_TF_PQ = 1,
} jpegr_transfer_function;

/*
 * Holds information for uncompressed image or recovery map.
 */
struct jpegr_uncompressed_struct {
    // Pointer to the data location.
    void* data;
    // Width of the recovery map or image in pixels.
    int width;
    // Height of the recovery map or image in pixels.
    int height;
    // Color gamut.
    jpegr_color_gamut colorGamut;
};

/*
 * Holds information for compressed image or recovery map.
 */
struct jpegr_compressed_struct {
    // Pointer to the data location.
    void* data;
    // Data length.
    int length;
    // Color gamut.
    jpegr_color_gamut colorGamut;
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

struct chromaticity_coord {
  float x;
  float y;
};


struct st2086_metadata {
  // xy chromaticity coordinate of the red primary of the mastering display
  chromaticity_coord redPrimary;
  // xy chromaticity coordinate of the green primary of the mastering display
  chromaticity_coord greenPrimary;
  // xy chromaticity coordinate of the blue primary of the mastering display
  chromaticity_coord bluePrimary;
  // xy chromaticity coordinate of the white point of the mastering display
  chromaticity_coord whitePoint;
  // Maximum luminance in nits of the mastering display
  uint32_t maxLuminance;
  // Minimum luminance in nits of the mastering display
  float minLuminance;
};

struct hdr10_metadata {
  // Mastering display color volume
  st2086_metadata st2086Metadata;
  // Max frame average light level in nits
  float maxFALL;
  // Max content light level in nits
  float maxCLL;
};

struct jpegr_metadata {
  // JPEG/R version
  uint32_t version;
  // Range scaling factor for the map
  float rangeScalingFactor;
  // The transfer function for decoding the HDR representation of the image
  jpegr_transfer_function transferFunction;
  // HDR10 metadata, only applicable for transferFunction of JPEGR_TF_PQ
  hdr10_metadata hdr10Metadata;
};

typedef struct jpegr_uncompressed_struct* jr_uncompressed_ptr;
typedef struct jpegr_compressed_struct* jr_compressed_ptr;
typedef struct jpegr_exif_struct* jr_exif_ptr;
typedef struct jpegr_metadata* jr_metadata_ptr;

class RecoveryMap {
public:
    /*
     * Compress JPEGR image from 10-bit HDR YUV and 8-bit SDR YUV.
     *
     * Generate recovery map from the HDR and SDR inputs, compress SDR YUV to 8-bit JPEG and append
     * the recovery map to the end of the compressed JPEG. HDR and SDR inputs must be the same
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
                         jpegr_transfer_function hdr_tf,
                         jr_compressed_ptr dest,
                         int quality,
                         jr_exif_ptr exif);

    /*
     * Compress JPEGR image from 10-bit HDR YUV, 8-bit SDR YUV and compressed 8-bit JPEG.
     *
     * This method requires HAL Hardware JPEG encoder.
     *
     * Generate recovery map from the HDR and SDR inputs, append the recovery map to the end of the
     * compressed JPEG. HDR and SDR inputs must be the same resolution and color space.
     * @param uncompressed_p010_image uncompressed HDR image in P010 color format
     * @param uncompressed_yuv_420_image uncompressed SDR image in YUV_420 color format
     * @param compressed_jpeg_image compressed 8-bit JPEG image
     * @param hdr_tf transfer function of the HDR image
     * @param dest destination of the compressed JPEGR image
     * @return NO_ERROR if encoding succeeds, error code if error occurs.
     */
    status_t encodeJPEGR(jr_uncompressed_ptr uncompressed_p010_image,
                         jr_uncompressed_ptr uncompressed_yuv_420_image,
                         jr_compressed_ptr compressed_jpeg_image,
                         jpegr_transfer_function hdr_tf,
                         jr_compressed_ptr dest);

    /*
     * Compress JPEGR image from 10-bit HDR YUV and 8-bit SDR YUV.
     *
     * This method requires HAL Hardware JPEG encoder.
     *
     * Decode the compressed 8-bit JPEG image to YUV SDR, generate recovery map from the HDR input
     * and the decoded SDR result, append the recovery map to the end of the compressed JPEG. HDR
     * and SDR inputs must be the same resolution.
     * @param uncompressed_p010_image uncompressed HDR image in P010 color format
     * @param compressed_jpeg_image compressed 8-bit JPEG image
     * @param hdr_tf transfer function of the HDR image
     * @param dest destination of the compressed JPEGR image
     * @return NO_ERROR if encoding succeeds, error code if error occurs.
     */
    status_t encodeJPEGR(jr_uncompressed_ptr uncompressed_p010_image,
                         jr_compressed_ptr compressed_jpeg_image,
                         jpegr_transfer_function hdr_tf,
                         jr_compressed_ptr dest);

    /*
     * Decompress JPEGR image.
     *
     * The output JPEGR image is in RGBA_1010102 data format if decoding to HDR.
     * @param compressed_jpegr_image compressed JPEGR image
     * @param dest destination of the uncompressed JPEGR image
     * @param exif destination of the decoded EXIF metadata.
     * @param request_sdr flag that request SDR output. If set to true, decoder will only decode
     *                    the primary image which is SDR. Setting of request_sdr and input source
     *                    (HDR or SDR) can be found in the table below:
     *                    |  input source  |  request_sdr  |  output of decoding  |
     *                    |       HDR      |     true      |          SDR         |
     *                    |       HDR      |     false     |          HDR         |
     *                    |       SDR      |     true      |          SDR         |
     *                    |       SDR      |     false     |          SDR         |
     * @return NO_ERROR if decoding succeeds, error code if error occurs.
     */
    status_t decodeJPEGR(jr_compressed_ptr compressed_jpegr_image,
                         jr_uncompressed_ptr dest,
                         jr_exif_ptr exif,
                         bool request_sdr);
private:
    /*
     * This method is called in the decoding pipeline. It will decode the recovery map.
     *
     * @param compressed_recovery_map compressed recovery map
     * @param dest decoded recover map
     * @return NO_ERROR if decoding succeeds, error code if error occurs.
     */
    status_t decompressRecoveryMap(jr_compressed_ptr compressed_recovery_map,
                               jr_uncompressed_ptr dest);

    /*
     * This method is called in the encoding pipeline. It will encode the recovery map.
     *
     * @param uncompressed_recovery_map uncompressed recovery map
     * @param dest encoded recover map
     * @return NO_ERROR if encoding succeeds, error code if error occurs.
     */
    status_t compressRecoveryMap(jr_uncompressed_ptr uncompressed_recovery_map,
                               jr_compressed_ptr dest);

    /*
     * This method is called in the encoding pipeline. It will take the uncompressed 8-bit and
     * 10-bit yuv images as input, and calculate the uncompressed recovery map. The input images
     * must be the same resolution.
     *
     * @param uncompressed_yuv_420_image uncompressed SDR image in YUV_420 color format
     * @param uncompressed_p010_image uncompressed HDR image in P010 color format
     * @param dest recovery map; caller responsible for memory of data
     * @param metadata metadata provides the transfer function for the HDR
     *                 image; range_scaling_factor and hdr10 FALL and CLL will
     *                 be updated.
     * @return NO_ERROR if calculation succeeds, error code if error occurs.
     */
    status_t generateRecoveryMap(jr_uncompressed_ptr uncompressed_yuv_420_image,
                                 jr_uncompressed_ptr uncompressed_p010_image,
                                 jr_metadata_ptr metadata,
                                 jr_uncompressed_ptr dest);

    /*
     * This method is called in the decoding pipeline. It will take the uncompressed (decoded)
     * 8-bit yuv image, the uncompressed (decoded) recovery map, and extracted JPEG/R metadata as
     * input, and calculate the 10-bit recovered image. The recovered output image is the same
     * color gamut as the SDR image, with the transfer function specified in the JPEG/R metadata,
     * and is in RGBA1010102 data format.
     *
     * @param uncompressed_yuv_420_image uncompressed SDR image in YUV_420 color format
     * @param uncompressed_recovery_map uncompressed recovery map
     * @param metadata JPEG/R metadata extracted from XMP.
     * @param dest reconstructed HDR image
     * @return NO_ERROR if calculation succeeds, error code if error occurs.
     */
    status_t applyRecoveryMap(jr_uncompressed_ptr uncompressed_yuv_420_image,
                              jr_uncompressed_ptr uncompressed_recovery_map,
                              jr_metadata_ptr metadata,
                              jr_uncompressed_ptr dest);

    /*
     * This method is called in the decoding pipeline. It will read XMP metadata to find the start
     * position of the compressed recovery map, and will extract the compressed recovery map.
     *
     * @param compressed_jpegr_image compressed JPEGR image
     * @param dest destination of compressed recovery map
     * @param metadata destination of the recovery map metadata
     * @return NO_ERROR if calculation succeeds, error code if error occurs.
     */
    status_t extractRecoveryMap(jr_compressed_ptr compressed_jpegr_image,
                                jr_compressed_ptr dest,
                                jr_metadata_ptr metadata);

    /*
     * This method is called in the encoding pipeline. It will take the standard 8-bit JPEG image
     * and the compressed recovery map as input, and update the XMP metadata with the end of JPEG
     * marker, and append the compressed gian map after the JPEG.
     *
     * @param compressed_jpeg_image compressed 8-bit JPEG image
     * @param compress_recovery_map compressed recover map
     * @param metadata JPEG/R metadata to encode in XMP of the jpeg
     * @param dest compressed JPEGR image
     * @return NO_ERROR if calculation succeeds, error code if error occurs.
     */
    status_t appendRecoveryMap(jr_compressed_ptr compressed_jpeg_image,
                               jr_compressed_ptr compressed_recovery_map,
                               jr_metadata_ptr metadata,
                               jr_compressed_ptr dest);

    /*
     * This method generates XMP metadata.
     *
     * below is an example of the XMP metadata that this function generates where
     * secondary_image_length = 1000
     * range_scaling_factor = 1.25
     *
     * <x:xmpmeta
     *   xmlns:x="adobe:ns:meta/"
     *   x:xmptk="Adobe XMP Core 5.1.2">
     *   <rdf:RDF
     *     xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
     *     <rdf:Description
     *       xmlns:GContainer="http://ns.google.com/photos/1.0/container/">
     *       <GContainer:Version>1</GContainer:Version>
     *       <GContainer:RangeScalingFactor>1.25</GContainer:RangeScalingFactor>
     *       <GContainer:Directory>
     *         <rdf:Seq>
     *           <rdf:li>
     *             <GContainer:Item
     *               Item:Semantic="Primary"
     *               Item:Mime="image/jpeg"/>
     *           </rdf:li>
     *           <rdf:li>
     *             <GContainer:Item
     *               Item:Semantic="RecoveryMap"
     *               Item:Mime="image/jpeg"
     *               Item:Length="1000"/>
     *           </rdf:li>
     *         </rdf:Seq>
     *       </GContainer:Directory>
     *     </rdf:Description>
     *   </rdf:RDF>
     * </x:xmpmeta>
     *
     * @param secondary_image_length length of secondary image
     * @param metadata JPEG/R metadata to encode as XMP
     * @return XMP metadata in type of string
     */
    std::string generateXmp(int secondary_image_length, jpegr_metadata& metadata);
};

} // namespace android::recoverymap

#endif // ANDROID_JPEGRECOVERYMAP_RECOVERYMAP_H
