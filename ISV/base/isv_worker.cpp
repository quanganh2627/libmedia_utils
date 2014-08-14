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
#include <cutils/properties.h>
#include <OMX_Core.h>
#include <OMX_IVCommon.h>
#include <system/graphics.h>
#include <system/window.h>
#include <cutils/log.h>
using namespace android::intel;
//#define LOG_NDEBUG 0
#define LOG_TAG "PPWorker"

#include "isv_worker.h"

#define CHECK_VASTATUS(str) \
    do { \
        if (vaStatus != VA_STATUS_SUCCESS) { \
                LOGE("%s failed\n", str); \
                return STATUS_ERROR;}   \
        }while(0);

enum STRENGTH {
    STRENGTH_LOW = 0,
    STRENGTH_MEDIUM,
    STRENGTH_HIGH
};

#define DENOISE_DEBLOCK_STRENGTH STRENGTH_MEDIUM
#define COLOR_STRENGTH STRENGTH_MEDIUM
#ifdef TARGET_VPP_USE_GEN
#define COLOR_NUM 4
#else
#define COLOR_NUM 2
#endif

#define MAX_FRC_OUTPUT 4 /*for frcx4*/
#define DEINTERLACE_NUM 1

#define QVGA_AREA (320 * 240)
#define VGA_AREA (640 * 480)
#define HD1080P_AREA (1920 * 1080)

#define DEFAULT_FRAME_RATE 30
#define DEFAULT_DENOISE_LEVEL 64
#define MIN_DENOISE_LEVEL 0
#define MAX_DENOISE_LEVEL 64
#define DEFAULT_SHARPNESS_LEVEL 32
#define MIN_SHARPNESS_LEVEL 0
#define MAX_SHARPNESS_LEVEL 64
#define DEFAULT_COLORBALANCE_LEVEL 32
#define MIN_COLORBALANCE_LEVEL 0
#define MAX_COLORBALANCE_LEVEL 64


VPPWorker::VPPWorker()
    :   mVPStarted(false),
        mFilters(0) {
    LOGI("%s", __func__);
    memset(&mFilterParam, 0, sizeof(mFilterParam));
}

//static
VPPWorker* VPPWorker::mVPPWorker = NULL;

//static
VPPWorker* VPPWorker::getInstance() {
    if (mVPPWorker == NULL)
        mVPPWorker = new VPPWorker();
    return mVPPWorker;
}

status_t VPPWorker::init() {
    if (!mVPStarted) {
        iVP_status status = iVP_create_context(&mVPContext, 1200, 800);
        if (status != IVP_STATUS_SUCCESS)
            return STATUS_ERROR;

        //update vpp status here
        isVppOn();

        //set to default value
        mFilterParam.frameRate = DEFAULT_FRAME_RATE;
        mFilterParam.denoiseLevel = DEFAULT_DENOISE_LEVEL;
        mFilterParam.sharpnessLevel = DEFAULT_SHARPNESS_LEVEL;
        mFilterParam.colorBalanceLevel = DEFAULT_COLORBALANCE_LEVEL;
        mVPStarted = true;
    }

    return STATUS_OK;
}

status_t VPPWorker::configFilters(uint32_t* filters,
                                  const FilterParam* filterParam,
                                  const uint32_t flags) {
    if (!filterParam) {
        ALOGE("%s: invalid filterParam", __func__);
        return STATUS_ERROR;
    }

    // reset filter type to 0
    mFilters = 0;
    mFilterParam.srcWidth = filterParam->srcWidth;
    mFilterParam.srcHeight = filterParam->srcHeight;
    mFilterParam.dstWidth = filterParam->dstWidth;
    mFilterParam.dstHeight = filterParam->dstHeight;
    mFilterParam.scalarType = filterParam->scalarType;
    mFilterParam.deinterlaceType = filterParam->deinterlaceType;

        mFilterParam.frameRate = filterParam->frameRate;
        mFilterParam.hasEncoder = filterParam->hasEncoder;

        if (filterParam->denoiseLevel >= MIN_DENOISE_LEVEL
                && filterParam->denoiseLevel <= MAX_DENOISE_LEVEL)
            mFilterParam.denoiseLevel = filterParam->denoiseLevel;
        if (filterParam->sharpnessLevel >= MIN_SHARPNESS_LEVEL
                && filterParam->sharpnessLevel <= MAX_SHARPNESS_LEVEL)
            mFilterParam.sharpnessLevel = filterParam->sharpnessLevel;
        if (filterParam->colorBalanceLevel >= MIN_COLORBALANCE_LEVEL
                && filterParam->colorBalanceLevel <= MAX_COLORBALANCE_LEVEL)
            mFilterParam.colorBalanceLevel = filterParam->colorBalanceLevel;

        uint32_t area = filterParam->srcWidth * filterParam->srcHeight;

        if (area <= VGA_AREA)
            mFilters |= OMX_INTEL_ImageFilterDenoise;

        if (area <= HD1080P_AREA)
            mFilters |= OMX_INTEL_ImageFilterSharpness;
        mFilters |= OMX_INTEL_ImageFilterColorBalance;

    if ((flags & OMX_BUFFERFLAG_TFF) != 0 ||
            (flags & OMX_BUFFERFLAG_BFF) != 0)
        mFilters |= OMX_INTEL_ImageFilterDeinterlace;

    *filters = mFilters;

    return STATUS_OK;
}

status_t VPPWorker::process(buffer_handle_t inputBuffer,
                            buffer_handle_t outputBuffer,
                            uint32_t outputCount, bool isEOS, uint32_t flags) {
    iVP_layer_t   primarySurf;
    iVP_layer_t   outSurf[DEINTERLACE_NUM];
    int angle = 0;
    uint32_t i = 0;
    INTEL_PRIVATE_VIDEOINFO videoInfo;

    videoInfo.value = flags;

    if (isEOS) {
        ALOGI("%s: EOS flag is detected", __func__);
        return STATUS_OK;
    }

    if (outputCount < 1 || outputCount > DEINTERLACE_NUM) {
       ALOGE("%s: invalid outputCount", __func__);
       return STATUS_ERROR;
    }

    memset(&primarySurf,0,sizeof(iVP_layer_t));
    memset(&outSurf,0,sizeof(iVP_layer_t));

    iVP_rect_t priSrect;
    priSrect.left  = 0;
    priSrect.top   = 0;
    priSrect.width = mFilterParam.srcWidth;
    priSrect.height  = mFilterParam.srcHeight;

    iVP_rect_t priDrect;
    priDrect.left  = 0;
    priDrect.top   = 0;
    priDrect.width = mFilterParam.dstWidth;
    priDrect.height  = mFilterParam.dstHeight;

    primarySurf.srcRect = &priSrect;
    primarySurf.destRect = &priDrect;
    primarySurf.bufferType    = IVP_GRALLOC_HANDLE; //TODO: it only support buffer_handle_t now
    primarySurf.rotation = (iVP_rotation_t)(angle/90);
    primarySurf.gralloc_handle = inputBuffer;
    
    iVP_filter_t filterType;
    switch (mFilterParam.scalarType) {
        case FILTER_HQ:
            filterType = IVP_FILTER_HQ;
            break;
        case FILTER_FAST:
        default:
            filterType = IVP_FILTER_FAST;
            break;
    }
    primarySurf.filter = filterType;

    // add VP filter to primarySurf : DI
    if (mFilters & OMX_INTEL_ImageFilterDeinterlace) {
        primarySurf.VPfilters |= FILTER_DEINTERLACE;
        iVP_deinterlace_t deinterlaceType;
        switch (mFilterParam.deinterlaceType) {
            case DEINTERLACE_BOB:
                deinterlaceType = IVP_DEINTERLACE_BOB;
                break;
            case DEINTERLACE_ADI:
            default:
                deinterlaceType = IVP_DEINTERLACE_IVP;
                break;
        }
        primarySurf.iDeinterlaceMode = deinterlaceType;
    }
    // add VP filter to primarySurf : DN
    if (mFilters & OMX_INTEL_ImageFilterDenoise) {
        primarySurf.VPfilters |= FILTER_DENOISE;
        primarySurf.fDenoiseFactor = mFilterParam.denoiseLevel;
    }
    // add VP filter to primarySurf : Sharpness
    if (mFilters & OMX_INTEL_ImageFilterSharpness) {
        primarySurf.VPfilters |= FILTER_SHARPNESS;
        primarySurf.fSharpnessFactor = mFilterParam.sharpnessLevel;
    }
    // add VP filter to primarySurf : Color balance
    if (mFilters & OMX_INTEL_ImageFilterColorBalance) {
        primarySurf.VPfilters |= FILTER_COLORBALANCE;
        primarySurf.fColorBalanceBrightness = 99.0;
        primarySurf.fColorBalanceContrast = 2.0;
        primarySurf.fColorBalanceHue = 179.0;
        primarySurf.fColorBalanceSaturation = 2.0;
        LOGD("%s: bright %f, contrast %f, hue %f, saturation %f", __func__, 
                primarySurf.fColorBalanceBrightness,
                primarySurf.fColorBalanceContrast,
                primarySurf.fColorBalanceHue,
                primarySurf.fColorBalanceSaturation);
    }

    iVP_rect_t outSrect;
    outSrect.left  = 0;
    outSrect.top   = 0;
    outSrect.width = mFilterParam.srcWidth;
    outSrect.height  = mFilterParam.srcHeight;

    iVP_rect_t outDrect;
    outDrect.left  = 0;
    outDrect.top   = 0;
    outDrect.width = mFilterParam.srcWidth;
    outDrect.height  = mFilterParam.srcHeight;

    for (i = 0; i < outputCount; i++) {
        outSurf[i].srcRect			= &outSrect;
        outSurf[i].destRect		= &outDrect;
        outSurf[i].bufferType		= IVP_GRALLOC_HANDLE; //TODO: it only support buffer_handle_t now
        outSurf[i].gralloc_handle   = outputBuffer;
        LOGD("%s: src width %d, height %d; dst width %d, dst height %d", __func__,
                mFilterParam.srcWidth, mFilterParam.srcHeight,
                mFilterParam.dstWidth, mFilterParam.dstHeight); 

        if (flags & (OMX_BUFFERFLAG_TFF))
            primarySurf.sample_type = (i == 0) ? IVP_SAMPLETYPE_TOPFIELD : IVP_SAMPLETYPE_BOTTOMFIELD;
        else if (flags & (OMX_BUFFERFLAG_BFF))
            primarySurf.sample_type = (i == 0) ? IVP_SAMPLETYPE_BOTTOMFIELD : IVP_SAMPLETYPE_TOPFIELD;
        else
            primarySurf.sample_type = IVP_SAMPLETYPE_PROGRESSIVE;

        if(IVP_STATUS_SUCCESS != iVP_exec(&mVPContext, &primarySurf, NULL, 0, &outSurf[i], true)) {
            LOGE("%s: iVP_exec failed!", __func__);
            return STATUS_ERROR;
        }
    }

    ALOGV("process, exit");
    return STATUS_OK;
}

VPPWorker::~VPPWorker() {
    LOGI("%s", __func__);
    if (mVPStarted) {
        iVP_destroy_context(&mVPContext);
        mVPStarted = false;
    }
    mFilters = 0;
    memset(&mFilterParam, 0, sizeof(mFilterParam));
    mVPPWorker = NULL;
}

status_t VPPWorker::reset() {
    ALOGV("reset");

    if (mVPStarted)
        iVP_destroy_context(&mVPContext);

    iVP_status status = iVP_create_context(&mVPContext, 1200, 800);
    if (status != IVP_STATUS_SUCCESS)
        return STATUS_ERROR;

    memset(&mFilterParam, 0, sizeof(mFilterParam));
    mVPStarted = true;
    return STATUS_OK;
}

