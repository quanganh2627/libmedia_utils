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

#include <va/va.h>
#include <va/va_vpp.h>
#include <va/va_android.h>
#include <OMX_Component.h>
#include "isv_profile.h"

#define ANDROID_DISPLAY_HANDLE 0x18C34078
#define Display unsigned int

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

using namespace android;

typedef enum
{
    STATUS_OK = 0,
    STATUS_NOT_SUPPORT,
    STATUS_ALLOCATION_ERROR,
    STATUS_ERROR,
    STATUS_DATA_RENDERING
} vpp_status;

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
    FRC_RATE frcRate;
} FilterParam;

//FIX ME: copy from ufo gralloc.h
typedef struct _ufo_buffer_details_t
{
    int width;       // \see alloc_device_t::alloc
    int height;      // \see alloc_device_t::alloc
    int format;      // \see alloc_device_t::alloc
    int usage;       // \see alloc_device_t::alloc
    int name;        // flink
    uint32_t fb;     // framebuffer id
    int drmformat;   // drm format
    int pitch;       // buffer pitch (in bytes)
    int size;        // buffer size (in bytes)
    int allocWidth;  // allocated buffer width in pixels.
    int allocHeight; // allocated buffer height in lines.
    int allocOffsetX;// horizontal pixel offset to content origin within allocated buffer.
    int allocOffsetY;// vertical line offset to content origin within allocated buffer.
} ufo_buffer_details_t;

enum
{
    INTEL_UFO_GRALLOC_MODULE_PERFORM_GET_BO_INFO = 6 // (buffer_handle_t, buffer_info_t*)
};

class VPPWorker {

    public:
        // config filters on or off based on video info
        status_t configFilters(uint32_t* filters, const FilterParam* filterParam, const uint32_t flags);

        // Initialize: setupVA()->setupFilters()->setupPipelineCaps()
        status_t init(int32_t width, int32_t height);

        // Get output buffer number needed for processing
        uint32_t getProcBufCount();

        // Get output buffer number needed for filling
        uint32_t getFillBufCount();

        // Send input and output buffers to VSP to begin processing
        status_t process(buffer_handle_t input, Vector<buffer_handle_t> output, uint32_t outputCount, bool isEOS, uint32_t flags);

        // Fill output buffers given, it's a blocking call
        status_t fill(Vector<buffer_handle_t> outputGraphicBuffer, uint32_t outputCount);

        // Initialize& deinit graphic configuration buffer
        status_t setBufferCount(int32_t size);
        status_t useBuffer(const sp<ANativeWindowBuffer> nativeBuffer);
        status_t useBuffer(buffer_handle_t handle);
        status_t freeBuffer(buffer_handle_t handle);

        // reset index
        status_t reset();

        // set video display mode
        void setDisplayMode(int32_t mode);

        // get video display mode
        int32_t getDisplayMode();

        // check HDMI connection status
        bool isHdmiConnected();

        uint32_t getVppOutputFps();

        VPPWorker();
        ~VPPWorker();

    private:
        // Check if VPP is supported
        bool isSupport() const;

        // Create VA context
        status_t setupVA(int32_t width, int32_t height);

        // Destroy VA context
        status_t terminateVA();

        // Get output buffer number needed for processing
        uint32_t getOutputBufCount(uint32_t index);

        // Check filter caps and create filter buffers
        status_t setupFilters();

        // Setup pipeline caps
        status_t setupPipelineCaps();

        // Map GraphicBuffer to VASurface
        VASurfaceID mapBuffer(buffer_handle_t graphicBuffer);

        struct VPPBuffer {
            uint32_t buffer;
            VASurfaceID surface;
            uint32_t colorFormat;
            uint32_t stride;
            uint32_t width;
            uint32_t height;
        };

        // alloc va surface for graphic buffer
        status_t allocSurface(VPPBuffer* pBuffer);

        //check if the input fps is suportted in array fpsSet.
        bool isFpsSupport(int32_t fps, int32_t *fpsSet, int32_t fpsSetCnt);

        // Debug only
        // Dump YUV frame
        status_t dumpYUVFrameData(VASurfaceID surfaceID);
        status_t writeNV12(int width, int height, unsigned char *out_buf, int y_pitch, int uv_pitch);

        VPPWorker(const VPPWorker &);
        VPPWorker &operator=(const VPPWorker &);

    public:
        uint32_t mNumForwardReferences;

    private:
        // VPP buffer queue
        Vector<VPPBuffer> mBuffers;
        Mutex mBufferLock; // to protect access to mBuffers

        // VA common variables
        bool mVAStarted;
        VAContextID mVAContext;
        int32_t mWidth;
        int32_t mHeight;
        Display * mDisplay;
        VADisplay mVADisplay;
        VAConfigID mVAConfig;
        bool mCanSupportAndroidGralloc;

        // Forward References Surfaces
        Vector<VABufferID> mPipelineBuffers;
        Mutex mPipelineBufferLock; // to protect access to mPipelineBuffers
        VASurfaceID *mForwardReferences;
        VASurfaceID mPrevInput;
        VASurfaceID mPrevOutput;

        // VPP Filters Buffers
        uint32_t mNumFilterBuffers;
        VABufferID mFilterBuffers[VAProcFilterCount];

        // VPP filter configuration
        VABufferID mFilterFrc;

        // VPP filter configuration
        uint32_t mFilters;
        FilterParam mFilterParam;

        // status
        uint32_t mInputIndex;
        uint32_t mOutputIndex;

        // FIXME: not very sure how to check color standard
        VAProcColorStandardType in_color_standards[VAProcColorStandardCount];
        VAProcColorStandardType out_color_standards[VAProcColorStandardCount];
};

#endif //VPPWorker_H_
