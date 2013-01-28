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

//#define LOG_NDEBUG 0
#define LOG_TAG "VPPWorker"

#include "VPPWorker.h"
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
#define COLOR_NUM 2
#define MAX_FRC_OUTPUT 4 /*for frcx4*/

#define QVGA_AREA (320 * 240)
#define VGA_AREA (640 * 480)
#define HD1080P_AREA (1920 * 1080)

namespace android {

VPPWorker::VPPWorker(const sp<ANativeWindow> &nativeWindow)
    :mNativeWindow(nativeWindow),
        mGraphicBufferNum(0),
        mWidth(0), mHeight(0), mInputFps(0),
        mVAStarted(false), mVAContext(VA_INVALID_ID),
        mDisplay(NULL), mVADisplay(NULL), mVAConfig(VA_INVALID_ID),
        mNumSurfaces(0), mSurfaces(NULL), mVASurfaceAttrib(NULL),
        mNumForwardReferences(3), mForwardReferences(NULL), mPrevInput(0),
        mNumFilterBuffers(0),
        mDeblockOn(false), mDenoiseOn(false), mSharpenOn(false), mColorOn(false),
        mFrcOn(false), mFrcRate(FRC_RATE_1X),
        mInputIndex(0), mOutputIndex(0) {
    memset(&mFilterBuffers, 0, VAProcFilterCount * sizeof(VABufferID));
    memset(&mGraphicBufferConfig, 0, sizeof(GraphicBufferConfig));
}

status_t VPPWorker::init() {
    status_t ret = STATUS_OK;

    if (!mVAStarted) {
        ret = setupVA();
        if (ret != STATUS_OK)
            return ret;
    }

    ret = configFilters();
    if (ret != STATUS_OK)
        return ret;

    if (mNumFilterBuffers == 0) {
        ret = setupFilters();
        if(ret != STATUS_OK)
            return ret;
    }
    return setupPipelineCaps();
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

void VPPWorker::setVideoInfo(uint32_t width, uint32_t height, uint32_t fps) {
    mWidth = width;
    mHeight = height;
    mInputFps = fps;
    configFilters();
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

VASurfaceID VPPWorker::mapBuffer(sp<GraphicBuffer> graphicBuffer) {
    if (graphicBuffer == NULL || mSurfaces == NULL || mVASurfaceAttrib == NULL)
        return VA_INVALID_SURFACE;
    ANativeWindowBuffer * nativeBuffer = graphicBuffer->getNativeBuffer();
    for (uint32_t i = 0; i < mNumSurfaces; i++) {
        if (mVASurfaceAttrib->buffers[i] == (uint32_t)nativeBuffer->handle)
            return mSurfaces[i];
    }
    return VA_INVALID_SURFACE;
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
    if (mInputIndex >= mNumForwardReferences)
        return getOutputBufCount(mOutputIndex);
    else
        return 0;
}

status_t VPPWorker::setupVA() {
    LOGV("setupVA");
    if (mVAStarted)
        return STATUS_OK;

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

    // Create VASurfaces
    mNumSurfaces = mGraphicBufferNum;
    mSurfaces = new VASurfaceID[mNumSurfaces];
    if (mSurfaces == NULL) {
        return STATUS_ALLOCATION_ERROR;
    }

    mVASurfaceAttrib = new VASurfaceAttributeTPI;
    if (mVASurfaceAttrib == NULL) {
        return STATUS_ALLOCATION_ERROR;
    }

    mVASurfaceAttrib->buffers= (unsigned int *)malloc(sizeof(unsigned int)*mNumSurfaces);
    if (mVASurfaceAttrib->buffers == NULL) {
        return STATUS_ALLOCATION_ERROR;
    }
    mVASurfaceAttrib->count = mNumSurfaces;
    mVASurfaceAttrib->luma_stride = mGraphicBufferConfig.stride;
    mVASurfaceAttrib->pixel_format = mGraphicBufferConfig.colorFormat;
    mVASurfaceAttrib->width = mGraphicBufferConfig.width;
    mVASurfaceAttrib->height = mGraphicBufferConfig.height;
    mVASurfaceAttrib->type = VAExternalMemoryAndroidGrallocBuffer;
    mVASurfaceAttrib->reserved[0] = (uint32_t)(mNativeWindow.get());
    LOGV("mVASurfaceAttrib->count = %d", mVASurfaceAttrib->count);
    LOGV("mVASurfaceAttrib->luma_stride=%d",mVASurfaceAttrib->luma_stride);
    LOGV("mVASurfaceAttrib->width=%d", mVASurfaceAttrib->width);
    LOGV("mVASurfaceAttrib->height=%d", mVASurfaceAttrib->height);

    for (uint32_t i = 0; i < mNumSurfaces; i++) {
        mVASurfaceAttrib->buffers[i] = (uint32_t)mGraphicBufferConfig.buffer[i];
    }
    vaStatus = vaCreateSurfacesWithAttribute(
        mVADisplay,
        mVASurfaceAttrib->width,
        mVASurfaceAttrib->height,
        VA_RT_FORMAT_YUV420,
        mNumSurfaces,
        mSurfaces,
        mVASurfaceAttrib);
    CHECK_VASTATUS("vaCreateSurfacesWithAttribute");

    // Create Context
    LOGV("ready to create context");
    vaStatus = vaCreateContext(mVADisplay, mVAConfig, mWidth, mHeight, 0, mSurfaces, mNumSurfaces, &mVAContext);
    CHECK_VASTATUS("vaCreateContext");

    mVAStarted = true;
    LOGV("VA has been successfully started");
    return STATUS_OK;
}

status_t VPPWorker::terminateVA() {
    if (mVASurfaceAttrib) {
        if (mVASurfaceAttrib->buffers) {
            free(mVASurfaceAttrib->buffers);
            mVASurfaceAttrib->buffers = NULL;
        }
        delete mVASurfaceAttrib;
        mVASurfaceAttrib = NULL;
    }

    if (mSurfaces) {
        vaDestroySurfaces(mVADisplay, mSurfaces, mNumSurfaces);
        delete [] mSurfaces;
        mSurfaces = NULL;
    }

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

    mVAStarted = false;
    return STATUS_OK;
}

status_t VPPWorker::configFilters() {
    // <QCIF or >1080P
    if (mHeight < 144 || mHeight > 1080)
        return STATUS_NOT_SUPPORT;
    uint32_t area = mWidth * mHeight;

    // QCIF to QVGA
    if (area <= QVGA_AREA) {
        mDeblockOn = true;
        mSharpenOn = true;
        mColorOn = true;
    }
    // QVGA to VGA
    else if (area <= VGA_AREA) {
        mDenoiseOn = true;
        mSharpenOn = true;
        mColorOn = true;
    }
    // VGA to 1080P
    else if (area <= HD1080P_AREA) {
        mSharpenOn = true;
    }

    if (mInputFps == 15) {
        mFrcOn = true;
        mFrcRate = FRC_RATE_4X;
    }
    else if (mInputFps == 24) {
        mFrcOn = true;
        mFrcRate = FRC_RATE_2_5X;
    }
    else if (mInputFps == 30) {
        mFrcOn = true;
        mFrcRate = FRC_RATE_2X;
    }
    LOGV("mDeblockOn=%d, mDenoiseOn=%d, mSharpenOn=%d, mColorOn=%d, mFrcOn=%d, mFrcRate=%d",
          mDeblockOn, mDenoiseOn, mSharpenOn, mColorOn, mFrcOn, mFrcRate);
    return STATUS_OK;
}

status_t VPPWorker::setupFilters() {
    VAProcFilterParameterBuffer deblock, denoise, sharpen;
    VAProcFilterParameterBufferColorBalance color[COLOR_NUM];
    VAProcFilterParameterBufferFrameRateConversion frc;
    VABufferID deblockId, denoiseId, sharpenId, colorId, frcId;
    uint32_t numCaps;
    VAProcFilterCap deblockCaps, denoiseCaps, sharpenCaps, frcCaps;
    VAProcFilterCapColorBalance colorCaps[COLOR_NUM];
    VAStatus vaStatus;
    uint32_t numSupportedFilters;
    VAProcFilterType supportedFilters[VAProcFilterCount];

    // query supported filters
    vaStatus = vaQueryVideoProcFilters(mVADisplay, mVAContext, supportedFilters, &numSupportedFilters);
    CHECK_VASTATUS("vaQueryVideoProcFilters");

    // create filter buffer for each filter
    for (uint32_t i = 0; i < numSupportedFilters; i++) {
        switch (supportedFilters[i]) {
            case VAProcFilterDeblocking:
                if (mDeblockOn) {
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
                if(mDenoiseOn) {
                    // check filter caps
                    numCaps = 1;
                    vaStatus = vaQueryVideoProcFilterCaps(mVADisplay, mVAContext,
                            VAProcFilterNoiseReduction,
                            &denoiseCaps,
                            &numCaps);
                    CHECK_VASTATUS("vaQueryVideoProcFilterCaps for denoising");
                    // create parameter buffer
                    denoise.type = VAProcFilterNoiseReduction;
                    denoise.value = denoiseCaps.range.min_value + DENOISE_DEBLOCK_STRENGTH * denoiseCaps.range.step;
                    vaStatus = vaCreateBuffer(mVADisplay, mVAContext,
                        VAProcFilterParameterBufferType, sizeof(denoise), 1,
                        &denoise, &denoiseId);
                    CHECK_VASTATUS("vaCreateBuffer for denoising");
                    mFilterBuffers[mNumFilterBuffers] = denoiseId;
                    mNumFilterBuffers++;
                }
                break;
            case VAProcFilterSharpening:
                if(mSharpenOn) {
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
                if(mColorOn) {
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
                            color[i].value = colorCaps[i].range.min_value + COLOR_STRENGTH * colorCaps[i].range.step;;
                            featureCount++;
                        }
                    }
                    vaStatus = vaCreateBuffer(mVADisplay, mVAContext,
                        VAProcFilterParameterBufferType, sizeof(*color), featureCount,
                        color, &colorId);
                    CHECK_VASTATUS("vaCreateBuffer for color balance");
                    mFilterBuffers[mNumFilterBuffers] = colorId;
                    mNumFilterBuffers++;
                }
                break;
            case VAProcFilterFrameRateConversion:
                if(mFrcOn) {
                    frc.type = VAProcFilterFrameRateConversion;
                    frc.input_fps = mInputFps;
                    switch (mFrcRate){
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
    return STATUS_OK;
}

status_t VPPWorker::setupPipelineCaps() {
    //TODO color standards
    VAProcPipelineCaps pipelineCaps;
    VAStatus vaStatus;
    pipelineCaps.input_color_standards = in_color_standards;
    pipelineCaps.num_input_color_standards = VAProcColorStandardCount;
    pipelineCaps.output_color_standards = out_color_standards;
    pipelineCaps.num_input_color_standards = VAProcColorStandardCount;

    vaStatus = vaQueryVideoProcPipelineCaps(mVADisplay, mVAContext,
        mFilterBuffers, mNumFilterBuffers,
        &pipelineCaps);
    CHECK_VASTATUS("vaQueryVideoProcPipelineCaps");

    mNumForwardReferences = pipelineCaps.num_forward_references;
    if (mNumForwardReferences > 0) {
        mForwardReferences = (VASurfaceID*)malloc(mNumForwardReferences * sizeof(VASurfaceID));
        if (mForwardReferences == NULL)
            return STATUS_ALLOCATION_ERROR;
        memset(mForwardReferences, 0, mNumForwardReferences * sizeof(VASurfaceID));
    }
    return STATUS_OK;
}

status_t VPPWorker::process(sp<GraphicBuffer> inputGraphicBuffer,
                             Vector< sp<GraphicBuffer> > outputGraphicBuffer,
                             uint32_t outputCount, bool isEOS) {
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
            sizeof(*pipeline),
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
    if (mFrcOn) {
        vaStatus = vaMapBuffer(mVADisplay, mFilterFrc, (void **)&frc);
        CHECK_VASTATUS("vaMapBuffer for frc parameter buffer");
        if (isEOS)
            frc->num_output_frames = 0;
        else
            frc->num_output_frames = outputCount - 1;
        frc->output_frames = output + 1;
    }

    // pipeline parameter setting
    /*VARectangle output_region;
    output_region.x = 0;
    output_region.y = 0;
    output_region.width = mWidth;
    output_region.height = mHeight;*/

    if (isEOS) {
        pipeline->surface = 0;
        pipeline->pipeline_flags = VA_PIPELINE_FLAG_END;
    }
    else {
        pipeline->surface = input;
        pipeline->pipeline_flags = 0;
    }
    pipeline->surface_region = NULL;
    pipeline->output_region = NULL;//&output_region;
    /* FIXME: set more meaningful background color */
    pipeline->output_background_color = 0;
    pipeline->filter_flags = 0;//VA_FILTER_SCALING_HQ;
    pipeline->filters = mFilterBuffers;
    pipeline->num_filters = mNumFilterBuffers;
    pipeline->forward_references = mForwardReferences;
    pipeline->num_forward_references = mNumForwardReferences;
    pipeline->backward_references = NULL;
    pipeline->num_backward_references = 0;
    pipeline->surface_color_standard = VAProcColorStandardNone;
    pipeline->output_color_standard = VAProcColorStandardNone;

    if (mFrcOn) {
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

    mInputIndex++;
    LOGV("process, exit");
    return STATUS_OK;
}

status_t VPPWorker::fill(Vector< sp<GraphicBuffer> > outputGraphicBuffer, uint32_t outputCount) {
    LOGV("fill, outputCount=%d, mOutputIndex=%d",outputCount, mOutputIndex);
    // get output surface
    VASurfaceID output[MAX_FRC_OUTPUT];
    VAStatus vaStatus;
    if (outputCount < 1)
        return STATUS_ERROR;
    // map GraphicBuffer to VASurface
    for (uint32_t i = 0; i < outputCount; i++) {
        output[i] = mapBuffer(outputGraphicBuffer[i]);
        if (output[i] == VA_INVALID_SURFACE) {
            LOGE("invalid output buffer");
            return STATUS_ERROR;
        }
        vaStatus = vaSyncSurface(mVADisplay, output[i]);
        CHECK_VASTATUS("vaSyncSurface");
        //dumpYUVFrameData(output[i]);
    }
    mOutputIndex++;
    LOGV("fill, exit");
    return STATUS_OK;
}

VPPWorker::~VPPWorker() {
    if (mForwardReferences != NULL) {
        free(mForwardReferences);
        mForwardReferences = NULL;
    }

    if (mVAStarted) {
        terminateVA();
    }
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

    ret = writeNV12(mWidth, mHeight, data_ptr, image.pitches[0], image.pitches[1]);
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
        return STATUS_ERROR;
    }
    if ((width % 2) || (height % 2))
    {
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

} //namespace Anroid
