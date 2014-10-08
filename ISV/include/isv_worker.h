/*
 * Copyright (C) 2012 Intel Corporation.  All rights reserved.
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
 *
 */

#ifndef VPPWorker_H_
#define VPPWorker_H_

#define ANDROID_DISPLAY_HANDLE 0x18C34078
#define MAX_GRAPHIC_BUFFER_NUMBER 64 // TODO: use GFX limitation first
#define Display unsigned int
#include <stdint.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
//#include "iVP_api.h"

using namespace android;

//FIXME: copy from OMX_VPP.h
/**
 * Enumeration of possible image filter types
 */
typedef enum OMX_INTEL_IMAGEFILTERTYPE {
    OMX_INTEL_ImageFilterDenoise            = 0x00000001, /**< extension for Intel Denoise */
    OMX_INTEL_ImageFilterDeinterlace        = 0x00000002, /**< extension for Intel de-interlace */
    OMX_INTEL_ImageFilterSharpness          = 0x00000004, /**< extension for Intel sharpness */
    OMX_INTEL_ImageFilterScale              = 0x00000008, /**< extension for Intel Scaling*/
    OMX_INTEL_ImageFilterColorBalance       = 0x00000010, /**< extension for Intel Colorbalance*/
    OMX_INTEL_ImageFilter3P                 = 0x00000020, /**< extension for 3P */
} OMX_INTEL_IMAGEFILTERTYPE;

/**
 * Enumeration of possible configure index for video processing
 */
typedef enum  _OMX_INTEL_VPP_INDEXTYPE {
    /* Vendor specific area for storing indices */
    OMX_INTEL_IndexConfigVPPStart
        = ((OMX_INDEXTYPE)OMX_IndexVendorStartUnused + 0xA0000), /**< reference: OMX_CONFIG_VPPStart */

    OMX_INTEL_IndexConfigFilterType,              /**< reference: OMX_INTEL_CONFIG_FILTERTYPE */
    OMX_INTEL_IndexConfigDenoiseLevel,            /**< reference: OMX_INTEL_CONFIG_DENOISETYPE */
    OMX_INTEL_IndexConfigDeinterlaceLevel,        /**< reference: OMX_INTEL_CONFIG_DEINTERLACETYPE */
    OMX_INTEL_IndexConfigColorBalanceLevel,       /**< reference: OMX_INTEL_CONFIG_COLORBALANCETYPE */
    OMX_INTEL_IndexConfigSharpnessLevel,          /**< reference: OMX_INTEL_CONFIG_SHARPNESSTYPE */
    OMX_INTEL_IndexConfigScaleLevel,              /**< reference: OMX_INTEL_CONFIG_SCALARTYPE */
    OMX_INTEL_IndexConfigIntel3PLevel,            /**< reference: OMX_INTEL_CONFIG_INTEL3PTYPE */
} OMX_INTEL_VPP_INDEX;

//FIXME: copy from OMX_Core.h

/* interlaced frame flag: This flag is set to indicate the buffer contains a 
 * top and bottom field and display ordering is top field first.
 * @ingroup buf
 */
#define OMX_BUFFERFLAG_TFF 0x00010000

/* interlaced frame flag: This flag is set to indicate the buffer contains a 
 * top and bottom field and display ordering is bottom field first.
 * @ingroup buf
 */
#define OMX_BUFFERFLAG_BFF 0x00020000

enum VPPWorkerStatus {
    STATUS_OK = 0,
    STATUS_NOT_SUPPORT,
    STATUS_ALLOCATION_ERROR,
    STATUS_ERROR,
    STATUS_DATA_RENDERING
};

typedef enum
{
    DEINTERLACE_BOB = 0,                   // BOB DI
    DEINTERLACE_ADI = 1,                   // ADI
} deinterlace_t;

//Avaiable filter types
typedef enum
{
    FILTER_HQ   = 0,                      // high-quality filter (AVS scaling)
    FILTER_FAST = 1,                      // fast filter (bilinear scaling)
} filter_t;

typedef struct {
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t dstWidth;
    uint32_t dstHeight;
    uint32_t denoiseLevel;
    uint32_t sharpnessLevel;
    uint32_t colorBalanceLevel;
    deinterlace_t deinterlaceType;
    filter_t scalarType;
    uint32_t frameRate;
    uint32_t hasEncoder;
} FilterParam;

typedef enum _INTEL_SCAN_TYPE
{
    INTEL_SCAN_PROGRESSIV      = 0,  /* Progressive*/
    INTEL_SCAN_TOPFIELD        = 1,  /* Top field */
    INTEL_SCAN_BOTTOMFIELD     = 2   /* Bottom field */
} INTEL_SCAN_TYPE;

typedef enum _INTEL_VIDEOSOURCE_TYPE
{
    INTEL_VideoSourceUnKnown      = 0,/* unKnown stream */
    INTEL_VideoSourceCamera       = 1,/* Camera stream */
    INTEL_VideoSourceVideoEditor  = 2,/* Video Editor stream */
    INTEL_VideoSourceTranscode    = 3,/* Transcode stream */
    INTEL_VideoSourceVideoConf    = 4,/* Video Conference stream*/
    INTEL_VideoSourceMax          = 5 /* Reserve for future use*/
} INTEL_VIDEOSOURCE_TYPE;

typedef enum _INTEL_FRAME_TYPE
{
    INTEL_I_FRAME      = 0,  /* I frame */
    INTEL_P_FRAME      = 1,  /* P frame */
    INTEL_B_FRAME      = 2   /* B Frame */
} INTEL_FRAME_TYPE;

typedef union _INTEL_PRIVATE_VIDEOINFO{
    struct {
        unsigned int legacy       : 16;   /*reserved for legacy OMX usage*/
        INTEL_SCAN_TYPE eScanType : 2;    /*Progressive or interlace*/
        INTEL_VIDEOSOURCE_TYPE eVideoSource: 3 ; /*Camera,VideoEdtor, etc. */
        INTEL_FRAME_TYPE  ePictureType : 2; /*I/P/B*/
        unsigned int      nFrameRate   : 7; /*frame rate*/
        unsigned int      reserved     : 1; /*reserved for extension*/
    }videoinfo;
    unsigned int value;
}INTEL_PRIVATE_VIDEOINFO;

class VPPWorker {

    public:
        static VPPWorker* getInstance();

        // config filters on or off based on video info
        status_t configFilters(uint32_t* filters, const FilterParam* filterParam, const uint32_t flags);

        // Initialize: setupVA()->setupFilters()->setupPipelineCaps()
        status_t init();

        // Send input and output buffers to VSP to begin processing
        status_t process(buffer_handle_t input, buffer_handle_t output, uint32_t outputCount, bool isEOS, uint32_t flags);

        // reset index
        status_t reset();
        // update VPP status
        bool isVppOn();

        ~VPPWorker();

    private:
        VPPWorker();

        VPPWorker(const VPPWorker &);
        VPPWorker &operator=(const VPPWorker &);

    private:
        static VPPWorker* mVPPWorker;

        iVPCtxID mVPContext;
        bool mVPStarted;

        // VPP filter configuration
        uint32_t mFilters;
        FilterParam mFilterParam;
};

#endif //VPPWorker_H_
