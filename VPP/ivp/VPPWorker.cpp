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

//#define LOG_NDEBUG 0
#define LOG_TAG "VPPWorker"

#include "VPPSetting.h"
#include "ivp/VPPWorker.h"
#if defined (TARGET_HAS_MULTIPLE_DISPLAY)
#include <display/MultiDisplayService.h>
using namespace android::intel;
#endif

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

namespace android {

VPPWorker::VPPWorker(const sp<ANativeWindow> &nativeWindow)
    :mGraphicBufferNum(0),
        mWidth(0), mHeight(0), mInputFps(0),
        mVPStarted(false), mNumForwardReferences(3),
        mDeblockOn(false), mDenoiseOn(false), mDeinterlacingOn(false),
        mSharpenOn(false), mColorOn(false), mSkintoneOn(false),
        mFrcOn(false), mFrcRate(FRC_RATE_1X),
        mInputIndex(0), mOutputIndex(0),
        mPreDisplayMode(0), mDisplayMode(0),
        mVPPSettingUpdated(false),
#ifdef TARGET_HAS_3P
        m3POn(false), m3PReconfig(false),
#endif
        mVPPOn(false), mDebug(0) {
    memset(&mGraphicBufferConfig, 0, sizeof(GraphicBufferConfig));
}

//static
VPPWorker* VPPWorker::mVPPWorker = NULL;
//static
sp<ANativeWindow> VPPWorker::mNativeWindow = NULL;

//static
VPPWorker* VPPWorker::getInstance(const sp<ANativeWindow> &nativeWindow) {
    if (mVPPWorker == NULL) {
        mVPPWorker = new VPPWorker(nativeWindow);
        mNativeWindow = nativeWindow;
    } else if (mNativeWindow != nativeWindow)
        return NULL;
    return mVPPWorker;
}

//static
bool VPPWorker::validateNativeWindow(const sp<ANativeWindow> &nativeWindow) {
    if (mNativeWindow == nativeWindow)
        return true;
    else
        return false;
}

status_t VPPWorker::init() {
    if (!mVPStarted) {
#ifdef TARGET_HAS_3P
        iVP_status status = iVP_create_context(&mVPContext, IVP_DEFAULT_WIDTH, IVP_DEFAULT_HEIGHT, IVP_3P_CAPABILITY);
#else
        iVP_status status = iVP_create_context(&mVPContext, IVP_DEFAULT_WIDTH, IVP_DEFAULT_HEIGHT, IVP_DEFAULT_CAPABLILITY);
#endif
        if (status == IVP_STATUS_SUCCESS)
            mVPStarted = true;
        else
            return STATUS_ERROR;
    }

    char propValueString[PROPERTY_VALUE_MAX];
#ifdef TARGET_HAS_3P
    property_get("media.3p.debug", propValueString, "0");
    mDebug = atoi(propValueString);
#endif
    //FIX ME: update VPP status here
    isVppOn();

    return STATUS_OK;
}

status_t VPPWorker::setGraphicBufferConfig(sp<GraphicBuffer> graphicBuffer) {
    if (graphicBuffer == NULL || mGraphicBufferNum >= MAX_GRAPHIC_BUFFER_NUMBER)
        return STATUS_ERROR;

    ANativeWindowBuffer * nativeBuffer = graphicBuffer->getNativeBuffer();
    if (nativeBuffer == NULL)
        return STATUS_ERROR;
    // assign below config data for the 1st time
    if (!mGraphicBufferNum) {
        mGraphicBufferConfig.colorFormat = nativeBuffer->format;
        mGraphicBufferConfig.stride = nativeBuffer->stride;
        mGraphicBufferConfig.width = nativeBuffer->width;
        mGraphicBufferConfig.height = nativeBuffer->height;
    }
    mGraphicBufferConfig.buffer[mGraphicBufferNum++] = (uint32_t)nativeBuffer->handle;
    return STATUS_OK;
}

uint32_t VPPWorker::getOutputBufCount(uint32_t index) {
    uint32_t bufCount = 1;
    if (mFrcOn && index > 0)
            bufCount = mFrcRate - (((mFrcRate == FRC_RATE_2_5X) ? (index & 1): 0));
    return bufCount;
}

uint32_t VPPWorker::getProcBufCount() {
    return getOutputBufCount(mInputIndex);
}

uint32_t VPPWorker::getFillBufCount() {
        return getOutputBufCount(mOutputIndex);
}

status_t VPPWorker::configFilters(const uint32_t width, const uint32_t height, const uint32_t fps, const uint32_t slowMotionFactor, const uint32_t flags) {
    mWidth = width;
    mHeight = height;
    mInputFps = fps;

    uint32_t area = mWidth * mHeight;

    if (mVPPOn) {
        if (area <= VGA_AREA)
            mDenoiseOn = true;

        if (area <= HD1080P_AREA)
            mSharpenOn = true;

        //mColorOn = true;
        mSkintoneOn = true;
#ifdef TARGET_HAS_3P
        if ((mDisplayMode & MDS_HDMI_CONNECTED) != 0 ||
                (mDisplayMode & MDS_WIDI_ON) != 0)
            m3POn = false;
        else
            m3POn = true;
#endif

    } else {
        mDenoiseOn = false;
        mSharpenOn = false;
        mColorOn = false;
        mSkintoneOn = false;
#ifdef TARGET_HAS_3P
        m3POn = false;
#endif
    }

    if ((flags & OMX_BUFFERFLAG_TFF) != 0 ||
            (flags & OMX_BUFFERFLAG_BFF) != 0)
        mDeinterlacingOn = true;
    else
        mDeinterlacingOn = false;

    LOGV("%s: skintone %d, denoise %d, sharpenon %d, deinterlacingon %d, mColorOn %d", __func__, mSkintoneOn, mDenoiseOn, mSharpenOn, mDeinterlacingOn, mColorOn);
#ifndef TARGET_HAS_3P
    if (!mDenoiseOn && !mSharpenOn && !mColorOn && !mSkintoneOn && !mDeinterlacingOn) {
        LOGW("all the filters are off, do not do VPP");
        return STATUS_NOT_SUPPORT;
    }
#endif
    return STATUS_OK;
}

status_t VPPWorker::process(sp<GraphicBuffer> inputGraphicBuffer,
                             Vector< sp<GraphicBuffer> > outputGraphicBuffer,
                             uint32_t outputCount, bool isEOS, uint32_t flags) {
    LOGV("process: outputCount=%d, mInputIndex=%d", outputCount, mInputIndex);
    iVP_layer_t   primarySurf;
    iVP_layer_t   outSurf[DEINTERLACE_NUM];
    ANativeWindowBuffer* input;
    ANativeWindowBuffer* output[DEINTERLACE_NUM];
    int angle = 0, i = 0;
    uint32_t vppStatus = 0;
    INTEL_PRIVATE_VIDEOINFO videoInfo;

    videoInfo.value = flags;

    if (isEOS) {
        LOGI("EOS flag is detected");
        return STATUS_OK;
    }

    if (outputCount < 1 || outputCount > DEINTERLACE_NUM) {
       LOGE("invalid outputCount");
       return STATUS_ERROR;
    }

    //need to update vpp status if VPP settings has been modified.
    if (mVPPSettingUpdated) {
        isVppOn();
#ifdef TARGET_HAS_3P
        if (mVPPOn)
            m3PReconfig = true;
#endif
        mVPPSettingUpdated = false;
        LOGI("%s: VPPSetting is modified, read vpp status %d.", __func__, vppStatus);
    }
    configFilters(mWidth, mHeight, mInputFps, 0, flags);

    input = inputGraphicBuffer->getNativeBuffer();

    memset(&primarySurf,0,sizeof(iVP_layer_t));
    memset(&outSurf,0,sizeof(iVP_layer_t));

    iVP_rect_t priSrect;
    priSrect.left  = 0;
    priSrect.top   = 0;
    priSrect.width = mWidth;
    priSrect.height  = mHeight;

    iVP_rect_t priDrect;
    priDrect.left  = 0;
    priDrect.top   = 0;
    priDrect.width = mWidth;
    priDrect.height  = mHeight;

    primarySurf.srcRect = &priSrect;
    primarySurf.destRect = &priDrect;
    primarySurf.bufferType    = IVP_GRALLOC_HANDLE; //TODO: it only support buffer_handle_t now
    primarySurf.rotation = (iVP_rotation_t)(angle/90);
    primarySurf.gralloc_handle = input->handle;

    // add VP filter to primarySurf : DI
    if (mDeinterlacingOn) {
        primarySurf.VPfilters |= FILTER_DEINTERLACE;
        primarySurf.iDeinterlaceMode = IVP_DEINTERLACE_BOB;
    }
    // add VP filter to primarySurf : DN
    if (mDenoiseOn) {
        primarySurf.VPfilters |= FILTER_DENOISE;
        primarySurf.fDenoiseFactor = 64;
    }
    // add VP filter to primarySurf : Sharpness
    if (mSharpenOn) {
        primarySurf.VPfilters |= FILTER_SHARPNESS;
        primarySurf.fSharpnessFactor = 64;
    }
    // add VP filter to primarySurf : Color balance
    if (mColorOn) {
        primarySurf.VPfilters |= FILTER_COLORBALANCE;
        primarySurf.fColorBalanceBrightness = 0;
        primarySurf.fColorBalanceContrast = 1;
        primarySurf.fColorBalanceHue = 0;
        primarySurf.fColorBalanceSaturation = 1;
    }
    // add VP filter to primarySurf : skintone enhancement
    if (mSkintoneOn) {
        primarySurf.VPfilters |= FILTER_SKINTONEENHANCEMENT;
        primarySurf.fSkinToneEnchancementFactor = 8.0;
    }
#ifdef TARGET_HAS_3P
    if (m3POn) {
        primarySurf.VPfilters |= FILTER_3P;
        primarySurf.st3pInfo.bEnable3P = true;
        switch (videoInfo.videoinfo.eVideoSource) {
            case INTEL_VideoSourceCamera:
                primarySurf.st3pInfo.stStreamType = IVP_STREAM_TYPE_CAMERA;
                break;
            //FIX ME: to add more stream type for video editor, video conf, e.g.
            case INTEL_VideoSourceVideoEditor:
            case INTEL_VideoSourceTranscode:
            case INTEL_VideoSourceVideoConf:
            default:
                primarySurf.st3pInfo.stStreamType = IVP_STREAM_TYPE_NORMAL;
                break;
        }

        primarySurf.st3pInfo.fFrameRate = mInputFps * 1.0;
        primarySurf.st3pInfo.bReconfig = false;
        primarySurf.st3pInfo.eKernelDumpBitmap.value = 0;
        if (mDebug == 1) {
            char propValueString[PROPERTY_VALUE_MAX];

            property_get("media.3p.reconfigure", propValueString, "0");
            int reconfigure = atoi(propValueString);
            if (reconfigure == 1) {
                m3PReconfig = true;
                property_set("media.3p.reconfigure", "0");
            }
            property_get("media.3p.kernelruntimedump", propValueString, "0");
            int krtDump = atoi(propValueString);
            primarySurf.st3pInfo.eKernelDumpBitmap.value = krtDump;
        }
        if (m3PReconfig) {
            LOGI("%s: prepare reconfigure 3P", __func__);
            primarySurf.st3pInfo.bReconfig = true;
            m3PReconfig = false;
        }
    }
#endif
    iVP_rect_t outSrect;
    outSrect.left  = 0;
    outSrect.top   = 0;
    outSrect.width = mWidth;
    outSrect.height  = mHeight;

    iVP_rect_t outDrect;
    outDrect.left  = 0;
    outDrect.top   = 0;
    outDrect.width = mWidth;
    outDrect.height  = mHeight;

    for (i = 0; i < outputCount; i++) {
        output[i] = outputGraphicBuffer[i]->getNativeBuffer();
        outSurf[i].srcRect			= &outSrect;
        outSurf[i].destRect		= &outDrect;
        outSurf[i].bufferType		= IVP_GRALLOC_HANDLE; //TODO: it only support buffer_handle_t now
        outSurf[i].gralloc_handle   = output[i]->handle;

        if (flags & (OMX_BUFFERFLAG_TFF))
            primarySurf.sample_type = (i == 0) ? IVP_SAMPLETYPE_TOPFIELD : IVP_SAMPLETYPE_BOTTOMFIELD;
        else if (flags & (OMX_BUFFERFLAG_BFF))
            primarySurf.sample_type = (i == 0) ? IVP_SAMPLETYPE_BOTTOMFIELD : IVP_SAMPLETYPE_TOPFIELD;
        else
            primarySurf.sample_type = IVP_SAMPLETYPE_PROGRESSIVE;

        iVP_exec(&mVPContext, &primarySurf, NULL, 0, &outSurf[i], true);
    }

    mInputIndex++;
    LOGV("process, exit");
    return STATUS_OK;
}

status_t VPPWorker::fill(Vector< sp<GraphicBuffer> > outputGraphicBuffer, uint32_t outputCount) {
    LOGV("fill, outputCount=%d, mOutputIndex=%d",outputCount, mOutputIndex);
    VAStatus vaStatus = STATUS_OK;

    //do nothing, only adjust pointer here.
    if (outputCount < 1)
        return STATUS_ERROR;

    mOutputIndex++;

    LOGV("fill, exit");
    return vaStatus;
}

VPPWorker::~VPPWorker() {
    if (mVPStarted) {
        iVP_destroy_context(&mVPContext);
        mVPStarted = false;
    }
    mVPPWorker = NULL;
    mNativeWindow.clear();
}

status_t VPPWorker::reset() {
    LOGD("reset");
    mInputIndex = 0;
    mOutputIndex = 0;

    if (mVPStarted)
        iVP_destroy_context(&mVPContext);

#ifdef TARGET_HAS_3P
        iVP_status status = iVP_create_context(&mVPContext, IVP_DEFAULT_WIDTH, IVP_DEFAULT_HEIGHT, IVP_3P_CAPABILITY);
#else
        iVP_status status = iVP_create_context(&mVPContext, IVP_DEFAULT_WIDTH, IVP_DEFAULT_HEIGHT, IVP_DEFAULT_CAPABLILITY);
#endif
    if (status != IVP_STATUS_SUCCESS)
        return STATUS_ERROR;

    mVPStarted = true;
    return STATUS_OK;
}

void VPPWorker::setDisplayMode(int mode) {
    if (mode & MDS_VPP_CHANGED)
        mVPPSettingUpdated = true;

    //only care HDMI/WIDI status
    mDisplayMode = mode & (MDS_HDMI_CONNECTED | MDS_WIDI_ON);
    if (mPreDisplayMode == mDisplayMode) {
        ALOGV("%s: HDMI/WIDI status no change", __func__);
        return;
    }

#ifdef TARGET_HAS_3P
    //HDMI disconnected
    if ((mDisplayMode & MDS_HDMI_CONNECTED) == 0 &&
            (mPreDisplayMode & MDS_HDMI_CONNECTED) != 0)
        m3PReconfig = true;

    //WIDI disconnected
    if ((mDisplayMode & MDS_WIDI_ON) == 0 &&
            (mPreDisplayMode & MDS_WIDI_ON) != 0)
        m3PReconfig = true;
#endif
    mPreDisplayMode = mDisplayMode;

    return;
}

int32_t VPPWorker::getDisplayMode () {
    return mDisplayMode;
}

bool VPPWorker::isHdmiConnected() {
    return (mDisplayMode & MDS_HDMI_CONNECTED);
}

uint32_t VPPWorker::isVppOn() {
    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == NULL) {
        ALOGE("%s: Failed to get service manager", __func__);
        return false;
    }
    sp<IMDService> mds = interface_cast<IMDService>(
            sm->getService(String16(INTEL_MDS_SERVICE_NAME)));
    if (mds == NULL) {
        ALOGE("%s: Failed to get MDS service", __func__);
        return false;
    }
    sp<IMultiDisplayInfoProvider> mdsInfoProvider = mds->getInfoProvider();
    if (mdsInfoProvider == NULL) {
        ALOGE("%s: Failed to get info provider", __func__);
        return false;
    }

    uint32_t vppStatus = mdsInfoProvider->getVppState();
    mVPPOn = (vppStatus & VPP_COMMON_ON) != 0;
    LOGI("%s: isVPPOn return %d", __func__, mVPPOn);

    return vppStatus;
}

uint32_t VPPWorker::getVppOutputFps() {
    uint32_t outputFps;
    //mFrcRate is 1 if FRC is disabled or input FPS is not changed by VPP.
    if (FRC_RATE_2_5X == mFrcRate) {
        outputFps = mInputFps * 5 / 2;
    } else {
        outputFps = mInputFps * mFrcRate;
    }

    LOGI("vpp is on in settings %d %d %d", outputFps,  mInputFps, mFrcRate);
    return outputFps;
}
} //namespace Anroid
