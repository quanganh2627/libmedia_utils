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
#include "VPPWorker.h"
#ifndef TARGET_VPP_USE_GEN
#include <OMX_IntelColorFormatExt.h>
#endif

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "isv-omxil"

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

using namespace android;

VPPWorker::VPPWorker()
    :mNumForwardReferences(0),
    mVAStarted(false), mVAContext(VA_INVALID_ID),
    mWidth(0), mHeight(0),
    mDisplay(NULL), mVADisplay(NULL),
    mVAConfig(VA_INVALID_ID),
    mCanSupportAndroidGralloc(false),
    mForwardReferences(NULL), mPrevInput(0),
    mNumFilterBuffers(0), mFilters(0),
    mInputIndex(0), mOutputIndex(0) {
    memset(&mFilterBuffers, 0, VAProcFilterCount * sizeof(VABufferID));
    memset(&mFilterParam, 0, sizeof(mFilterParam));
    mBuffers.clear();
}

status_t VPPWorker::init(int32_t width, int32_t height) {
    status_t ret = STATUS_OK;

    if (!mVAStarted) {
        if (STATUS_OK != setupVA(width, height)) {
            LOGE("%s: failed to setupVA", __func__);
            return ret;
        }
        mVAStarted = true;
    }

    return ret;
}

bool VPPWorker::isSupport() const {
    bool support = false;

    int num_entrypoints = vaMaxNumEntrypoints(mVADisplay);
    VAEntrypoint * entrypoints = (VAEntrypoint *)malloc(num_entrypoints * sizeof(VAEntrypoint));
    if (entrypoints == NULL) {
        LOGE("failed to malloc entrypoints array\n");
        return false;
    }

    // check if it contains VPP entry point VAEntrypointVideoProc
    VAStatus vaStatus = vaQueryConfigEntrypoints(mVADisplay, VAProfileNone, entrypoints, &num_entrypoints);
    if (vaStatus != VA_STATUS_SUCCESS) {
        LOGE("vaQueryConfigEntrypoints failed");
        return false;
    }
    for (int i = 0; !support && i < num_entrypoints; i++) {
        support = entrypoints[i] == VAEntrypointVideoProc;
    }
    free(entrypoints);
    entrypoints = NULL;

    return support;
}

status_t VPPWorker::setBufferCount(int32_t size)
{
    Mutex::Autolock autoLock(mBufferLock);
#if 0
    if (!mBuffers.isEmpty()) {
        LOGE("%s: the buffer queue should be empty before we set its size", __func__);
        return STATUS_ERROR;
    }
#endif
    mBuffers.setCapacity(size);

    return STATUS_OK;
}

status_t VPPWorker::freeBuffer(buffer_handle_t handle)
{
    Mutex::Autolock autoLock(mBufferLock);
    for (uint32_t i = 0; i < mBuffers.size(); i++) {
        VPPBuffer vppBuffer = mBuffers.itemAt(i);
        if (vppBuffer.buffer == (uint32_t)handle) {
            if (VA_STATUS_SUCCESS != vaDestroySurfaces(mVADisplay, &vppBuffer.surface, 1))
                return STATUS_ERROR;
            mBuffers.removeAt(i);
            return STATUS_OK;
        }
    }

    LOGW("%s: can't find buffer %u", __func__, handle);
    return STATUS_ERROR;
}

status_t VPPWorker::allocSurface(VPPBuffer *pBuffer)
{
    // Create VASurfaces
    VASurfaceAttrib attribs[3];

    if (!mCanSupportAndroidGralloc) {
        int supportedMemType = 0;

        //check whether it support create surface from external buffer
        unsigned int num = 0;
        VASurfaceAttrib* outAttribs = NULL;
        //get attribs number
        VAStatus vaStatus = vaQuerySurfaceAttributes(mVADisplay, mVAConfig, NULL, &num);
        CHECK_VASTATUS("vaQuerySurfaceAttributes");
        if (num == 0)
            return STATUS_NOT_SUPPORT;

        //get attributes
        outAttribs = new VASurfaceAttrib[num];
        if (outAttribs == NULL) {
            return STATUS_ALLOCATION_ERROR;
        }
        vaStatus = vaQuerySurfaceAttributes(mVADisplay, mVAConfig, outAttribs, &num);
        if (vaStatus != VA_STATUS_SUCCESS) {
            LOGE("vaQuerySurfaceAttributs fail!");
            delete []outAttribs;
            return STATUS_ERROR;
        }

        for(uint32_t i = 0; i < num; i ++) {
            if (outAttribs[i].type == VASurfaceAttribMemoryType) {
                supportedMemType = outAttribs[i].value.value.i;
                break;
            }
        }
        delete []outAttribs;

        if ((supportedMemType & VA_SURFACE_ATTRIB_MEM_TYPE_ANDROID_GRALLOC) == 0)
            return VA_INVALID_SURFACE;

        mCanSupportAndroidGralloc = true;
    }

    VASurfaceAttribExternalBuffers mVAExtBuf;
    int32_t width, height, stride;

    memset(&mVAExtBuf, 0, sizeof(VASurfaceAttribExternalBuffers));
#ifdef TARGET_VPP_USE_GEN
    gralloc_module_t* pGralloc = NULL;
    ufo_buffer_details_t info;
    buffer_handle_t handle = (buffer_handle_t)pBuffer->buffer;

    memset(&info, 0, sizeof(ufo_buffer_details_t));
    int err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (hw_module_t const**)&pGralloc);
    if (!pGralloc) err = -1;
    if (0 == err) 
        err = pGralloc->perform(pGralloc, INTEL_UFO_GRALLOC_MODULE_PERFORM_GET_BO_INFO, handle, &info);

    if (0 != err)
    {
        LOGE("%s: can't get graphic buffer info", __func__);
        return STATUS_ERROR;
    }
    width = info.width;
    height = info.height;
    stride = info.pitch;
#else
    if (pBuffer->colorFormat == OMX_INTEL_COLOR_FormatYUV420PackedSemiPlanar_Tiled) {
        LOGV("set TILING flag");
        mVAExtBuf.flags |= VA_SURFACE_EXTBUF_DESC_ENABLE_TILING;
    }
    width = pBuffer->width;
    height = pBuffer->height;
    stride = pBuffer->stride;
#endif
    mVAExtBuf.pixel_format = VA_FOURCC_NV12;
    mVAExtBuf.width = width;
    mVAExtBuf.height = height;
    mVAExtBuf.data_size = stride * height * 1.5;
    mVAExtBuf.num_buffers = 1;
    mVAExtBuf.num_planes = 2;
    mVAExtBuf.pitches[0] = stride;
    mVAExtBuf.pitches[1] = stride;
    mVAExtBuf.pitches[2] = 0;
    mVAExtBuf.pitches[3] = 0;
    mVAExtBuf.offsets[0] = 0;
    mVAExtBuf.offsets[1] = stride * height;
    mVAExtBuf.offsets[2] = 0;
    mVAExtBuf.offsets[3] = 0;
    mVAExtBuf.flags |= VA_SURFACE_ATTRIB_MEM_TYPE_ANDROID_GRALLOC;
    mVAExtBuf.buffers = (long unsigned int*)&pBuffer->buffer;

    attribs[0].type = (VASurfaceAttribType)VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_ANDROID_GRALLOC;

    attribs[1].type = (VASurfaceAttribType)VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &mVAExtBuf;

    attribs[2].type = (VASurfaceAttribType)VASurfaceAttribUsageHint;
    attribs[2].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[2].value.type = VAGenericValueTypeInteger;
    attribs[2].value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_VPP_READ;

    LOGV("%s: Ext buffer: width %d, height %d, data_size %d, pitch %d", __func__,
            mVAExtBuf.width, mVAExtBuf.height, mVAExtBuf.data_size, mVAExtBuf.pitches[0]);
    VAStatus vaStatus = vaCreateSurfaces(mVADisplay, VA_RT_FORMAT_YUV420, mVAExtBuf.width,
                                 mVAExtBuf.height, &pBuffer->surface, 1, attribs, 3);
    CHECK_VASTATUS("vaCreateSurfaces");

    return STATUS_OK;
}

status_t VPPWorker::useBuffer(buffer_handle_t handle)
{
    Mutex::Autolock autoLock(mBufferLock);
    if (handle == NULL || mBuffers.size() >= mBuffers.capacity())
        return STATUS_ERROR;

    VPPBuffer vppBuffer;
    vppBuffer.buffer = (uint32_t)handle;

    if (STATUS_OK != allocSurface(&vppBuffer))
        return STATUS_ERROR;

    mBuffers.push_back(vppBuffer);
    return STATUS_OK;

}

status_t VPPWorker::useBuffer(const sp<ANativeWindowBuffer> nativeBuffer)
{
    Mutex::Autolock autoLock(mBufferLock);
    if (nativeBuffer == NULL || mBuffers.size() >= mBuffers.capacity())
        return STATUS_ERROR;

    VPPBuffer vppBuffer;
    vppBuffer.buffer = (uint32_t)nativeBuffer->handle;
    vppBuffer.colorFormat = nativeBuffer->format;
    vppBuffer.stride = nativeBuffer->stride;
    vppBuffer.width = nativeBuffer->width;
    vppBuffer.height = nativeBuffer->height;

    if (STATUS_OK != allocSurface(&vppBuffer))
        return STATUS_ERROR;

    mBuffers.push_back(vppBuffer);
    return STATUS_OK;
}

VASurfaceID VPPWorker::mapBuffer(buffer_handle_t handle) {
    if (handle == NULL)
        return VA_INVALID_SURFACE;
    for (uint32_t i = 0; i < mBuffers.size(); i++) {
        VPPBuffer vppBuffer = mBuffers.itemAt(i);
        if (vppBuffer.buffer == (uint32_t)handle)
            return vppBuffer.surface;
    }
    return VA_INVALID_SURFACE;
}

uint32_t VPPWorker::getProcBufCount() {
    return getOutputBufCount(mInputIndex);
}

uint32_t VPPWorker::getFillBufCount() {
        return getOutputBufCount(mOutputIndex);
}

uint32_t VPPWorker::getOutputBufCount(uint32_t index) {
    uint32_t bufCount = 1;
    if (((mFilters & FilterFrameRateConversion) != 0)
            && index > 0)
            bufCount = mFilterParam.frcRate - (((mFilterParam.frcRate == FRC_RATE_2_5X) ? (index & 1): 0));
    return bufCount;
}


status_t VPPWorker::setupVA(int32_t width, int32_t height) {
    LOGV("setupVA");
    if (mDisplay != NULL) {
        LOGE("VA is particially started");
        return STATUS_ERROR;
    }
    mDisplay = new Display;
    *mDisplay = ANDROID_DISPLAY_HANDLE;

    mVADisplay = vaGetDisplay(mDisplay);
    if (mVADisplay == NULL) {
        LOGE("vaGetDisplay failed");
        return STATUS_ERROR;
    }

    int majorVersion, minorVersion;
    VAStatus vaStatus = vaInitialize(mVADisplay, &majorVersion, &minorVersion);
    CHECK_VASTATUS("vaInitialize");

    // Check if VPP entry point is supported
    if (!isSupport()) {
        LOGE("VPP is not supported on current platform");
        return STATUS_NOT_SUPPORT;
    }

    // Find out the format for the target
    VAConfigAttrib attrib;
    attrib.type = VAConfigAttribRTFormat;
    vaStatus = vaGetConfigAttributes(mVADisplay, VAProfileNone, VAEntrypointVideoProc, &attrib, 1);
    CHECK_VASTATUS("vaGetConfigAttributes");

    if ((attrib.value & VA_RT_FORMAT_YUV420) == 0) {
        LOGE("attribute is %x vs wanted %x", attrib.value, VA_RT_FORMAT_YUV420);
        return STATUS_NOT_SUPPORT;
    }

    LOGV("ready to create config");
    // Create the configuration
    vaStatus = vaCreateConfig(mVADisplay, VAProfileNone, VAEntrypointVideoProc, &attrib, 1, &mVAConfig);
    CHECK_VASTATUS("vaCreateConfig");


    // Create Context
    LOGV("ready to create context");
    mWidth = width;
    mHeight = height;
    vaStatus = vaCreateContext(mVADisplay, mVAConfig, mWidth, mHeight, 0, NULL, 0, &mVAContext);
    CHECK_VASTATUS("vaCreateContext");

    LOGV("VA has been successfully started");
    return STATUS_OK;
}

status_t VPPWorker::terminateVA() {
    if (mVAContext != VA_INVALID_ID) {
         vaDestroyContext(mVADisplay, mVAContext);
         mVAContext = VA_INVALID_ID;
    }

    if (mVAConfig != VA_INVALID_ID) {
        vaDestroyConfig(mVADisplay, mVAConfig);
        mVAConfig = VA_INVALID_ID;
    }

    if (mVADisplay) {
        vaTerminate(mVADisplay);
        mVADisplay = NULL;
    }

    if (mDisplay) {
        delete mDisplay;
        mDisplay = NULL;
    }

    return STATUS_OK;
}

status_t VPPWorker::configFilters(uint32_t* filters,
                                  const FilterParam* filterParam,
                                  const uint32_t flags)
{
    status_t ret = STATUS_OK;

    if (!filterParam) {
        ALOGE("%s: invalid filterParam", __func__);
        return STATUS_ERROR;
    }

    uint32_t temp = *filters;
    mFilterParam.srcWidth = filterParam->srcWidth;
    mFilterParam.srcHeight = filterParam->srcHeight;
    mFilterParam.dstWidth = filterParam->dstWidth;
    mFilterParam.dstHeight = filterParam->dstHeight;
    mFilterParam.frameRate = filterParam->frameRate;

    if ((flags & OMX_BUFFERFLAG_TFF) != 0 ||
            (flags & OMX_BUFFERFLAG_BFF) != 0)
        *filters |= FilterDeinterlacing;
    else
        *filters &= ~FilterDeinterlacing;

    if (*filters != mFilters) {
        mFilters = *filters;
        ret = setupFilters();
    }

    return ret;
}

bool VPPWorker::isFpsSupport(int32_t fps, int32_t *fpsSet, int32_t fpsSetCnt) {
    bool ret = false;
    for (int32_t i = 0; i < fpsSetCnt; i++) {
        if (fps == fpsSet[i]) {
            ret = true;
            break;
        }
    }

    return ret;
}

status_t VPPWorker::setupFilters() {
    LOGV("setupFilters");
    VAProcFilterParameterBuffer deblock, denoise, sharpen;
    VAProcFilterParameterBufferDeinterlacing deint;
    VAProcFilterParameterBufferColorBalance color[COLOR_NUM];
    VAProcFilterParameterBufferFrameRateConversion frc;
    VABufferID deblockId, denoiseId, deintId, sharpenId, colorId, frcId;
    uint32_t numCaps;
    VAProcFilterCap deblockCaps, denoiseCaps, sharpenCaps, frcCaps;
    VAProcFilterCapDeinterlacing deinterlacingCaps[VAProcDeinterlacingCount];
    VAProcFilterCapColorBalance colorCaps[COLOR_NUM];
    VAStatus vaStatus;
    uint32_t numSupportedFilters = VAProcFilterCount;
    VAProcFilterType supportedFilters[VAProcFilterCount];

    if (mNumFilterBuffers != 0) {
        for (uint32_t i = 0; i < mNumFilterBuffers; i++) {
            if (VA_STATUS_SUCCESS != vaDestroyBuffer(mVADisplay, mFilterBuffers[i]))
                LOGW("%s: failed to destroy va buffer %d", __func__, mFilterBuffers[i]);
                //return STATUS_ERROR;
        }
        mNumFilterBuffers = 0;
    }

    // query supported filters
    vaStatus = vaQueryVideoProcFilters(mVADisplay, mVAContext, supportedFilters, &numSupportedFilters);
    CHECK_VASTATUS("vaQueryVideoProcFilters");

    // create filter buffer for each filter
    for (uint32_t i = 0; i < numSupportedFilters; i++) {
        switch (supportedFilters[i]) {
            case VAProcFilterDeblocking:
                if ((mFilters & FilterDeblocking) != 0) {
                    // check filter caps
                    numCaps = 1;
                    vaStatus = vaQueryVideoProcFilterCaps(mVADisplay, mVAContext,
                            VAProcFilterDeblocking,
                            &deblockCaps,
                            &numCaps);
                    CHECK_VASTATUS("vaQueryVideoProcFilterCaps for deblocking");
                    // create parameter buffer
                    deblock.type = VAProcFilterDeblocking;
                    deblock.value = deblockCaps.range.min_value + DENOISE_DEBLOCK_STRENGTH * deblockCaps.range.step;
                    vaStatus = vaCreateBuffer(mVADisplay, mVAContext,
                        VAProcFilterParameterBufferType, sizeof(deblock), 1,
                        &deblock, &deblockId);
                    CHECK_VASTATUS("vaCreateBuffer for deblocking");
                    mFilterBuffers[mNumFilterBuffers] = deblockId;
                    mNumFilterBuffers++;
                }
                break;
            case VAProcFilterNoiseReduction:
                if((mFilters & FilterNoiseReduction) != 0) {
                    // check filter caps
                    numCaps = 1;
                    vaStatus = vaQueryVideoProcFilterCaps(mVADisplay, mVAContext,
                            VAProcFilterNoiseReduction,
                            &denoiseCaps,
                            &numCaps);
                    CHECK_VASTATUS("vaQueryVideoProcFilterCaps for denoising");
                    // create parameter buffer
                    denoise.type = VAProcFilterNoiseReduction;
#ifdef TARGET_VPP_USE_GEN
                    char propValueString[PROPERTY_VALUE_MAX];

                    // placeholder for vpg driver: can't support denoise factor auto adjust, so leave config to user.
                    property_get("vpp.filter.denoise.factor", propValueString, "64.0");
                    denoise.value = atof(propValueString);
                    denoise.value = (denoise.value < 0.0f) ? 0.0f : denoise.value;
                    denoise.value = (denoise.value > 64.0f) ? 64.0f : denoise.value;
#else
                    denoise.value = denoiseCaps.range.min_value + DENOISE_DEBLOCK_STRENGTH * denoiseCaps.range.step;
#endif
                    vaStatus = vaCreateBuffer(mVADisplay, mVAContext,
                        VAProcFilterParameterBufferType, sizeof(denoise), 1,
                        &denoise, &denoiseId);
                    CHECK_VASTATUS("vaCreateBuffer for denoising");
                    mFilterBuffers[mNumFilterBuffers] = denoiseId;
                    mNumFilterBuffers++;
                }
                break;
            case VAProcFilterDeinterlacing:
                if ((mFilters & FilterDeinterlacing) != 0) {
                    numCaps = VAProcDeinterlacingCount;
                    vaStatus = vaQueryVideoProcFilterCaps(mVADisplay, mVAContext,
                            VAProcFilterDeinterlacing,
                            &deinterlacingCaps[0],
                            &numCaps);
                    CHECK_VASTATUS("vaQueryVideoProcFilterCaps for deinterlacing");
                    for (uint32_t i = 0; i < numCaps; i++)
                    {
                        VAProcFilterCapDeinterlacing * const cap = &deinterlacingCaps[i];
                        if (cap->type != VAProcDeinterlacingBob) // desired Deinterlacing Type
                            continue;

                        deint.type = VAProcFilterDeinterlacing;
                        deint.algorithm = VAProcDeinterlacingBob;
                        vaStatus = vaCreateBuffer(mVADisplay,
                                mVAContext,
                                VAProcFilterParameterBufferType,
                                sizeof(deint), 1,
                                &deint, &deintId);
                        CHECK_VASTATUS("vaCreateBuffer for deinterlacing");
                        mFilterBuffers[mNumFilterBuffers] = deintId;
                        mNumFilterBuffers++;
                    }
                }
                break;
            case VAProcFilterSharpening:
                if((mFilters & FilterSharpening) != 0) {
                    // check filter caps
                    numCaps = 1;
                    vaStatus = vaQueryVideoProcFilterCaps(mVADisplay, mVAContext,
                            VAProcFilterSharpening,
                            &sharpenCaps,
                            &numCaps);
                    CHECK_VASTATUS("vaQueryVideoProcFilterCaps for sharpening");
                    // create parameter buffer
                    sharpen.type = VAProcFilterSharpening;
                    sharpen.value = sharpenCaps.range.default_value;
                    vaStatus = vaCreateBuffer(mVADisplay, mVAContext,
                        VAProcFilterParameterBufferType, sizeof(sharpen), 1,
                        &sharpen, &sharpenId);
                    CHECK_VASTATUS("vaCreateBuffer for sharpening");
                    mFilterBuffers[mNumFilterBuffers] = sharpenId;
                    mNumFilterBuffers++;
                }
                break;
            case VAProcFilterColorBalance:
                if((mFilters & FilterColorBalance) != 0) {
                    uint32_t featureCount = 0;
                    // check filter caps
                    // FIXME: it's not used at all!
                    numCaps = COLOR_NUM;
                    vaStatus = vaQueryVideoProcFilterCaps(mVADisplay, mVAContext,
                            VAProcFilterColorBalance,
                            colorCaps,
                            &numCaps);
                    CHECK_VASTATUS("vaQueryVideoProcFilterCaps for color balance");
                    // create parameter buffer
                    for (uint32_t i = 0; i < numCaps; i++) {
                        if (colorCaps[i].type == VAProcColorBalanceAutoSaturation) {
                            color[i].type = VAProcFilterColorBalance;
                            color[i].attrib = VAProcColorBalanceAutoSaturation;
                            color[i].value = colorCaps[i].range.min_value + COLOR_STRENGTH * colorCaps[i].range.step;
                            featureCount++;
                        }
                        else if (colorCaps[i].type == VAProcColorBalanceAutoBrightness) {
                            color[i].type = VAProcFilterColorBalance;
                            color[i].attrib = VAProcColorBalanceAutoBrightness;
                            color[i].value = colorCaps[i].range.min_value + COLOR_STRENGTH * colorCaps[i].range.step;
                            featureCount++;
                        }
                    }
#ifdef TARGET_VPP_USE_GEN
                    //TODO: VPG need to support check input value by colorCaps.
                    enum {kHue = 0, kSaturation, kBrightness, kContrast};
                    char propValueString[PROPERTY_VALUE_MAX];
                    color[kHue].type = VAProcFilterColorBalance;
                    color[kHue].attrib = VAProcColorBalanceHue;

                    // placeholder for vpg driver: can't support auto color balance, so leave config to user.
                    property_get("vpp.filter.procamp.hue", propValueString, "179.0");
                    color[kHue].value = atof(propValueString);
                    color[kHue].value = (color[kHue].value < -180.0f) ? -180.0f : color[kHue].value;
                    color[kHue].value = (color[kHue].value > 180.0f) ? 180.0f : color[kHue].value;
                    featureCount++;

                    color[kSaturation].type   = VAProcFilterColorBalance;
                    color[kSaturation].attrib = VAProcColorBalanceSaturation;
                    property_get("vpp.filter.procamp.saturation", propValueString, "1.0");
                    color[kSaturation].value = atof(propValueString);
                    color[kSaturation].value = (color[kSaturation].value < 0.0f) ? 0.0f : color[kSaturation].value;
                    color[kSaturation].value = (color[kSaturation].value > 10.0f) ? 10.0f : color[kSaturation].value;
                    featureCount++;

                    color[kBrightness].type   = VAProcFilterColorBalance;
                    color[kBrightness].attrib = VAProcColorBalanceBrightness;
                    property_get("vpp.filter.procamp.brightness", propValueString, "0.0");
                    color[kBrightness].value = atof(propValueString);
                    color[kBrightness].value = (color[kBrightness].value < -100.0f) ? -100.0f : color[kBrightness].value;
                    color[kBrightness].value = (color[kBrightness].value > 100.0f) ? 100.0f : color[kBrightness].value;
                    featureCount++;

                    color[kContrast].type   = VAProcFilterColorBalance;
                    color[kContrast].attrib = VAProcColorBalanceContrast;
                    property_get("vpp.filter.procamp.contrast", propValueString, "1.0");
                    color[kContrast].value = atof(propValueString);
                    color[kContrast].value = (color[kContrast].value < 0.0f) ? 0.0f : color[kContrast].value;
                    color[kContrast].value = (color[kContrast].value > 10.0f) ? 10.0f : color[kContrast].value;
                    featureCount++;
#endif
                    vaStatus = vaCreateBuffer(mVADisplay, mVAContext,
                        VAProcFilterParameterBufferType, sizeof(*color), featureCount,
                        color, &colorId);
                    CHECK_VASTATUS("vaCreateBuffer for color balance");
                    mFilterBuffers[mNumFilterBuffers] = colorId;
                    mNumFilterBuffers++;
                }
                break;
            case VAProcFilterFrameRateConversion:
                if((mFilters & FilterFrameRateConversion) != 0) {
                    frc.type = VAProcFilterFrameRateConversion;
                    frc.input_fps = mFilterParam.frameRate;
                    switch (mFilterParam.frcRate){
                        case FRC_RATE_1X:
                            frc.output_fps = frc.input_fps;
                            break;
                        case FRC_RATE_2X:
                            frc.output_fps = frc.input_fps * 2;
                            break;
                        case FRC_RATE_2_5X:
                            frc.output_fps = frc.input_fps * 5/2;
                            break;
                        case FRC_RATE_4X:
                            frc.output_fps = frc.input_fps * 4;
                            break;
                    }
                    vaStatus = vaCreateBuffer(mVADisplay, mVAContext,
                        VAProcFilterParameterBufferType, sizeof(frc), 1,
                        &frc, &frcId);
                    CHECK_VASTATUS("vaCreateBuffer for frc");
                    mFilterBuffers[mNumFilterBuffers] = frcId;
                    mNumFilterBuffers++;
                    mFilterFrc = frcId;
                }
                break;
            default:
                LOGE("Not supported filter\n");
                break;
        }
    }

    return setupPipelineCaps();
}

status_t VPPWorker::setupPipelineCaps() {
    LOGV("setupPipelineCaps");
    //TODO color standards
    VAProcPipelineCaps pipelineCaps;
    VAStatus vaStatus;
    pipelineCaps.input_color_standards = in_color_standards;
    pipelineCaps.num_input_color_standards = VAProcColorStandardCount;
    pipelineCaps.output_color_standards = out_color_standards;
    pipelineCaps.num_output_color_standards = VAProcColorStandardCount;

    vaStatus = vaQueryVideoProcPipelineCaps(mVADisplay, mVAContext,
        mFilterBuffers, mNumFilterBuffers,
        &pipelineCaps);
    CHECK_VASTATUS("vaQueryVideoProcPipelineCaps");

    if (mForwardReferences != NULL) {
        free(mForwardReferences);
        mForwardReferences = NULL;
        mNumForwardReferences = 0;
    }

    mNumForwardReferences = pipelineCaps.num_forward_references;
    if (mNumForwardReferences > 0) {
        mForwardReferences = (VASurfaceID*)malloc(mNumForwardReferences * sizeof(VASurfaceID));
        if (mForwardReferences == NULL)
            return STATUS_ALLOCATION_ERROR;
        memset(mForwardReferences, 0, mNumForwardReferences * sizeof(VASurfaceID));
    }
    return STATUS_OK;
}

status_t VPPWorker::process(buffer_handle_t inputGraphicBuffer,
                             Vector<buffer_handle_t> outputGraphicBuffer,
                             uint32_t outputCount, bool isEOS, uint32_t flags) {
    LOGV("process: outputCount=%d, mInputIndex=%d", outputCount, mInputIndex);
    VASurfaceID input;
    VASurfaceID output[MAX_FRC_OUTPUT];
    VABufferID pipelineId;
    VAProcPipelineParameterBuffer *pipeline;
    VAProcFilterParameterBufferFrameRateConversion *frc;
    VAStatus vaStatus;
    uint32_t i;

    if (outputCount < 1) {
       LOGE("invalid outputCount");
       return STATUS_ERROR;
    }
    // map GraphicBuffer to VASurface
    input = mapBuffer(inputGraphicBuffer);

    if (input == VA_INVALID_SURFACE && !isEOS) {
        LOGE("invalid input buffer");
        return STATUS_ERROR;
    }
    for (i = 0; i < outputCount; i++) {
        output[i] = mapBuffer(outputGraphicBuffer[i]);
        if (output[i] == VA_INVALID_SURFACE) {
            LOGE("invalid output buffer");
            return STATUS_ERROR;
        }
    }

    // reference frames setting
    if (mNumForwardReferences > 0) {
        /* add previous frame into reference array*/
        for (i = 1; i < mNumForwardReferences; i++) {
            mForwardReferences[i - 1] = mForwardReferences[i];
        }

        //make last reference to input
        mForwardReferences[mNumForwardReferences - 1] = mPrevInput;
    }

    mPrevInput = input;

    // create pipeline parameter buffer
    vaStatus = vaCreateBuffer(mVADisplay,
            mVAContext,
            VAProcPipelineParameterBufferType,
            sizeof(VAProcPipelineParameterBuffer),
            1,
            NULL,
            &pipelineId);
    CHECK_VASTATUS("vaCreateBuffer for VAProcPipelineParameterBufferType");

    LOGV("before vaBeginPicture");
    vaStatus = vaBeginPicture(mVADisplay, mVAContext, output[0]);
    CHECK_VASTATUS("vaBeginPicture");

    // map pipeline paramter buffer
    vaStatus = vaMapBuffer(mVADisplay, pipelineId, (void**)&pipeline);
    CHECK_VASTATUS("vaMapBuffer for pipeline parameter buffer");

    // frc pamameter setting
    if ((mFilters & FilterFrameRateConversion) != 0) {
        vaStatus = vaMapBuffer(mVADisplay, mFilterFrc, (void **)&frc);
        CHECK_VASTATUS("vaMapBuffer for frc parameter buffer");
        if (isEOS)
            frc->num_output_frames = 0;
        else
            frc->num_output_frames = outputCount - 1;
        frc->output_frames = output + 1;
    }

    // pipeline parameter setting
    VARectangle dst_region;
    dst_region.x = 0;
    dst_region.y = 0;
    dst_region.width = mFilterParam.dstWidth;
    dst_region.height = mFilterParam.dstHeight;

    VARectangle src_region;
    src_region.x = 0;
    src_region.y = 0;
    src_region.width = mFilterParam.srcWidth;
    src_region.height = mFilterParam.srcHeight;

    if (isEOS) {
        pipeline->surface = 0;
        pipeline->pipeline_flags = VA_PIPELINE_FLAG_END;
    }
    else {
        pipeline->surface = input;
        pipeline->pipeline_flags = 0;
    }
#ifdef TARGET_VPP_USE_GEN
    pipeline->surface_region = &src_region;
    pipeline->output_region = &dst_region;
    pipeline->surface_color_standard = VAProcColorStandardBT601;
    pipeline->output_color_standard = VAProcColorStandardBT601;
#else
    pipeline->surface_region = NULL;
    pipeline->output_region = NULL;//&output_region;
    pipeline->surface_color_standard = VAProcColorStandardNone;
    pipeline->output_color_standard = VAProcColorStandardNone;
    /* real rotate state will be decided in psb video */
    pipeline->rotation_state = 0;
#endif
    /* FIXME: set more meaningful background color */
    pipeline->output_background_color = 0;
    pipeline->filters = mFilterBuffers;
    pipeline->num_filters = mNumFilterBuffers;
    pipeline->forward_references = mForwardReferences;
    pipeline->num_forward_references = mNumForwardReferences;
    pipeline->backward_references = NULL;
    pipeline->num_backward_references = 0;

    //currently, we only transfer TOP field to frame, no frame rate change.
    if (flags & (OMX_BUFFERFLAG_TFF | OMX_BUFFERFLAG_BFF)) {
        pipeline->filter_flags = VA_TOP_FIELD;
    } else {
        pipeline->filter_flags = VA_FRAME_PICTURE;
    }

    if ((mFilters & FilterFrameRateConversion) != 0) {
        vaStatus = vaUnmapBuffer(mVADisplay, mFilterFrc);
        CHECK_VASTATUS("vaUnmapBuffer for frc parameter buffer");
    }

    vaStatus = vaUnmapBuffer(mVADisplay, pipelineId);
    CHECK_VASTATUS("vaUnmapBuffer for pipeline parameter buffer");

    LOGV("before vaRenderPicture");
    // Send parameter to driver
    vaStatus = vaRenderPicture(mVADisplay, mVAContext, &pipelineId, 1);
    CHECK_VASTATUS("vaRenderPicture");

    LOGV("before vaEndPicture");
    vaStatus = vaEndPicture(mVADisplay, mVAContext);
    CHECK_VASTATUS("vaEndPicture");
    
    if (isEOS) {
        vaStatus = vaSyncSurface(mVADisplay, output[0]);
        CHECK_VASTATUS("vaSyncSurface");
        if (VA_STATUS_SUCCESS != vaDestroyBuffer(mVADisplay, pipelineId)) {
            LOGE("%s: failed to destroy va buffer %d", __func__, pipelineId);
            return STATUS_ERROR;
        }
        return STATUS_OK;
    }

    mInputIndex++;

    Mutex::Autolock autoLock(mPipelineBufferLock);
    mPipelineBuffers.push_back(pipelineId);

    LOGV("process, exit");
    return STATUS_OK;
}

status_t VPPWorker::fill(Vector<buffer_handle_t> outputGraphicBuffer, uint32_t outputCount) {
    LOGV("fill, outputCount=%d, mOutputIndex=%d",outputCount, mOutputIndex);
    // get output surface
    VASurfaceID output[MAX_FRC_OUTPUT];
    VAStatus vaStatus;
    VASurfaceStatus surStatus;

    if (outputCount < 1)
        return STATUS_ERROR;
    // map GraphicBuffer to VASurface
    for (uint32_t i = 0; i < outputCount; i++) {

        output[i] = mapBuffer(outputGraphicBuffer[i]);
        if (output[i] == VA_INVALID_SURFACE) {
            LOGE("invalid output buffer");
            return STATUS_ERROR;
        }
        //FIXME: only enable sync mode
#if 0
        vaStatus = vaQuerySurfaceStatus(mVADisplay, output[i],&surStatus);
        CHECK_VASTATUS("vaQuerySurfaceStatus");
        if (surStatus == VASurfaceRendering) {
            LOGV("Rendering %d", i);
            /* The behavior of driver is: all output of one process task are return in one interruption.
               The whole outputs of one FRC task are all ready or none of them is ready.
               If the behavior changed, it hurts the performance.
            */
            if (0 != i) {
                LOGW("*****Driver behavior changed. The performance is hurt.");
                LOGW("Please check driver behavior: all output of one task return in one interruption.");
            }
            vaStatus = STATUS_DATA_RENDERING;
            break;
        }

        if ((surStatus != VASurfaceRendering) && (surStatus != VASurfaceReady)) {
            LOGE("surface statu Error %d", surStatus);
            vaStatus = STATUS_ERROR;
        }
#endif

        vaStatus = vaSyncSurface(mVADisplay, output[i]);
        CHECK_VASTATUS("vaSyncSurface");
        vaStatus = STATUS_OK;
        //dumpYUVFrameData(output[i]);
    }

    {
        Mutex::Autolock autoLock(mPipelineBufferLock);
        if (vaStatus == STATUS_OK) {
            VABufferID pipelineBuffer = mPipelineBuffers.itemAt(0);
            if (VA_STATUS_SUCCESS != vaDestroyBuffer(mVADisplay, pipelineBuffer)) {
                LOGE("%s: failed to destroy va buffer %d", __func__, pipelineBuffer);
                return STATUS_ERROR;
            }
            mPipelineBuffers.removeAt(0);
            mOutputIndex++;
        }
    }

    LOGV("fill, exit");
    return vaStatus;
}

VPPWorker::~VPPWorker() {
    if (mNumFilterBuffers != 0) {
        for (uint32_t i = 0; i < mNumFilterBuffers; i++) {
            if(VA_STATUS_SUCCESS != vaDestroyBuffer(mVADisplay, mFilterBuffers[i]))
                LOGW("%s: failed to destroy va buffer id %d", __func__, mFilterBuffers[i]);
        }
        mNumFilterBuffers = 0;
    }

    {
        Mutex::Autolock autoLock(mPipelineBufferLock);
        while (!mPipelineBuffers.isEmpty()) {
            VABufferID pipelineBuffer = mPipelineBuffers.itemAt(0);
            if (VA_STATUS_SUCCESS != vaDestroyBuffer(mVADisplay, pipelineBuffer))
                LOGW("%s: failed to destroy va buffer id %d", __func__, pipelineBuffer);
            mPipelineBuffers.removeAt(0);
        }
    }

    if (mForwardReferences != NULL) {
        free(mForwardReferences);
        mForwardReferences = NULL;
        mNumForwardReferences = 0;
    }

    terminateVA();
    mFilters = 0;
    memset(&mFilterParam, 0, sizeof(mFilterParam));
}

// Debug only
#define FRAME_OUTPUT_FILE_NV12 "/storage/sdcard0/vpp_output.nv12"
status_t VPPWorker::dumpYUVFrameData(VASurfaceID surfaceID) {
    status_t ret;
    if (surfaceID == VA_INVALID_SURFACE)
        return STATUS_ERROR;

    VAStatus vaStatus;
    VAImage image;
    unsigned char *data_ptr;

    vaStatus = vaDeriveImage(mVADisplay,
            surfaceID,
            &image);
    CHECK_VASTATUS("vaDeriveImage");

    vaStatus = vaMapBuffer(mVADisplay, image.buf, (void **)&data_ptr);
    CHECK_VASTATUS("vaMapBuffer");

    ret = writeNV12(mFilterParam.srcWidth, mFilterParam.srcHeight, data_ptr, image.pitches[0], image.pitches[1]);
    if (ret != STATUS_OK) {
        ALOGV("writeNV12 error");
        return STATUS_ERROR;
    }

    vaStatus = vaUnmapBuffer(mVADisplay, image.buf);
    CHECK_VASTATUS("vaUnMapBuffer");

    vaStatus = vaDestroyImage(mVADisplay,image.image_id);
    CHECK_VASTATUS("vaDestroyImage");

    return STATUS_OK;
}

status_t VPPWorker::reset() {
    status_t ret;
    LOGI("reset");
    LOGI("======mVPPInputCount=%d, mVPPRenderCount=%d======",
            mInputIndex, mOutputIndex);
    mInputIndex = 0;
    mOutputIndex = 0;

    {
        Mutex::Autolock autoLock(mPipelineBufferLock);
        while (!mPipelineBuffers.isEmpty()) {
            VABufferID pipelineBuffer = mPipelineBuffers.itemAt(0);
            if (VA_STATUS_SUCCESS != vaDestroyBuffer(mVADisplay, pipelineBuffer)) {
                LOGE("%s: failed to destroy va buffer %d", __func__, pipelineBuffer);
                return STATUS_ERROR;
            }
            mPipelineBuffers.removeAt(0);
        }
    }

    if (mNumFilterBuffers != 0) {
        for (uint32_t i = 0; i < mNumFilterBuffers; i++) {
            if (VA_STATUS_SUCCESS != vaDestroyBuffer(mVADisplay, mFilterBuffers[i]))
                LOGW("%s: failed to destroy va buffer %d", __func__, mFilterBuffers[i]);
                //return STATUS_ERROR;
        }
        mNumFilterBuffers = 0;
    }
    
    // we need to clear the cache for reference surfaces
    if (mForwardReferences != NULL) {
        free(mForwardReferences);
        mForwardReferences = NULL;
        mNumForwardReferences = 0;
    }

    if (mVAContext != VA_INVALID_ID) {
         vaDestroyContext(mVADisplay, mVAContext);
         mVAContext = VA_INVALID_ID;
    }
    VAStatus vaStatus = vaCreateContext(mVADisplay, mVAConfig, mWidth, mHeight, 0, NULL, 0, &mVAContext);
    CHECK_VASTATUS("vaCreateContext");

    return setupFilters();
}

uint32_t VPPWorker::getVppOutputFps() {
    uint32_t outputFps;
    //mFilterParam.frcRate is 1 if FRC is disabled or input FPS is not changed by VPP.
    if (FRC_RATE_2_5X == mFilterParam.frcRate) {
        outputFps = mFilterParam.frameRate * 5 / 2;
    } else {
        outputFps = mFilterParam.frameRate * mFilterParam.frcRate;
    }

    LOGV("vpp is on in settings %d %d %d", outputFps,  mFilterParam.frameRate, mFilterParam.frcRate);
    return outputFps;
}


status_t VPPWorker::writeNV12(int width, int height, unsigned char *out_buf, int y_pitch, int uv_pitch) {
    size_t result;
    int frame_size;
    unsigned char *y_start, *uv_start;
    int h;

    FILE *ofile = fopen(FRAME_OUTPUT_FILE_NV12, "ab");
    if(ofile == NULL) {
        ALOGE("Open %s failed!", FRAME_OUTPUT_FILE_NV12);
        return STATUS_ERROR;
    }

    if (out_buf == NULL)
    {
        fclose(ofile);
        return STATUS_ERROR;
    }
    if ((width % 2) || (height % 2))
    {
        fclose(ofile);
        return STATUS_ERROR;
    }
    // Set frame size
    frame_size = height * width * 3/2;

    /* write y */
    y_start = out_buf;
    for (h = 0; h < height; ++h) {
        result = fwrite(y_start, sizeof(unsigned char), width, ofile);
        y_start += y_pitch;
    }

    /* write uv */
    uv_start = out_buf + uv_pitch * height;
    for (h = 0; h < height / 2; ++h) {
        result = fwrite(uv_start, sizeof(unsigned char), width, ofile);
        uv_start += uv_pitch;
    }
    // Close file
    fclose(ofile);
    return STATUS_OK;
}
