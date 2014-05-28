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

#include <android/native_window.h>
#include <va/va.h>
#include <va/va_vpp.h>
#include <va/va_tpi.h>
#include "va/va_android.h"
#include "iVP_api.h"
#include "VPPMds.h"

namespace android {

typedef enum _FRC_RATE {
    FRC_RATE_1X = 1,
    FRC_RATE_2X,
    FRC_RATE_2_5X,
    FRC_RATE_4X
}FRC_RATE;

enum VPPWorkerStatus {
    STATUS_OK = 0,
    STATUS_NOT_SUPPORT,
    STATUS_ALLOCATION_ERROR,
    STATUS_ERROR,
    STATUS_DATA_RENDERING
};

struct GraphicBufferConfig {
    uint32_t colorFormat;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t buffer[MAX_GRAPHIC_BUFFER_NUMBER];
};

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
        static VPPWorker* getInstance(const sp<ANativeWindow> &nativeWindow);

        // config filters on or off based on video info
        status_t configFilters(const uint32_t width, const uint32_t height, const uint32_t fps, const uint32_t slowMotionFactor = 1, const uint32_t flags = 0);

        // Initialize: setupVA()->setupFilters()->setupPipelineCaps()
        status_t init();

        // Get output buffer number needed for processing
        uint32_t getProcBufCount();

        // Get output buffer number needed for filling
        uint32_t getFillBufCount();

        // Send input and output buffers to VSP to begin processing
        status_t process(sp<GraphicBuffer> input, Vector< sp<GraphicBuffer> > output, uint32_t outputCount, bool isEOS, uint32_t flags);

        // Fill output buffers given, it's a blocking call
        status_t fill(Vector< sp<GraphicBuffer> > outputGraphicBuffer, uint32_t outputCount);

        // Initialize graphic configuration buffer
        status_t setGraphicBufferConfig(sp<GraphicBuffer> graphicBuffer);

        // Check if the input NativeWindow handle is the same as the one when construction
        bool validateNativeWindow(const sp<ANativeWindow> &nativeWindow);
        // reset index
        status_t reset();
        // set display mode
        void setDisplayMode(int mode);

        // get video display mode
        int32_t getDisplayMode();

        // check HDMI connection status
        bool isHdmiConnected();

        /* config enable or disable VPP frame rate conversion for HDMI feature.
         * To enable this feature, MDS listener is MUST
         */
        status_t configFrc4Hdmi(bool enableFrc4Hdmi, sp<VPPMDSListener>* pmds){return STATUS_OK;};

        uint32_t getVppOutputFps();
        status_t calculateFrc(bool *frcOn, FRC_RATE *rate){return STATUS_OK;};
        ~VPPWorker();

    private:
        VPPWorker(const sp<ANativeWindow> &nativeWindow);

        // Get output buffer needed based on input index
        uint32_t getOutputBufCount(uint32_t index);


        VPPWorker(const VPPWorker &);
        VPPWorker &operator=(const VPPWorker &);
        uint32_t isVppOn();
    public:
        uint32_t mNumForwardReferences;
        FRC_RATE mFrcRate;
        //updated FrrRate used in VPP frc for HDMI only
        FRC_RATE mUpdatedFrcRate;
        bool mUpdatedFrcOn;
        bool bNeedCheckFrc;
        bool mFrcOn;
    private:
        // Graphic buffer
        static VPPWorker* mVPPWorker;
        static sp<ANativeWindow> mNativeWindow;
        uint32_t mGraphicBufferNum;
        struct GraphicBufferConfig mGraphicBufferConfig;

        // video info
        uint32_t mWidth;
        uint32_t mHeight;
        uint32_t mInputFps;

        iVPCtxID mVPContext;
        bool mVPStarted;

        // VPP filter configuration
        bool mDeblockOn;
        bool mDenoiseOn;
        bool mDeinterlacingOn;
        bool mSharpenOn;
        bool mColorOn;
        bool mSkintoneOn;
#ifdef TARGET_HAS_3P
        bool m3POn;
        bool m3PReconfig;
#endif

        // status
        uint32_t mInputIndex;
        uint32_t mOutputIndex;

        //display mode
        int mDisplayMode;
        int mPreDisplayMode;
        bool mVPPSettingUpdated;
        bool mVPPOn;

        //debug flag
        int mDebug;

};

}
#endif //VPPWorker_H_
