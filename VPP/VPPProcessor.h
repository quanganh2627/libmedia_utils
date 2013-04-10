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

#ifndef __VPP_PROCESSOR_H
#define __VPP_PROCESSOR_H
#include "VPPProcThread.h"
#include "VPPFillThread.h"
#include "VPPWorker.h"

#include <stdint.h>

#include <android/native_window.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/OMXCodec.h>

namespace android {

struct MediaBuffer;
struct MediaBufferObserver;
struct OMXCodec;
class VPPProcThread;
class VPPFillThread;

// Input buffer transition:
// FREE->LOADED->PROCESSING->READY->FREE
// Output buffer transition:
// FREE->PROCESSING->READY->RENDERING->FREE
//                       |_>FREE
enum VPPBufferStatus {
    VPP_BUFFER_FREE = 0,        //free, not being used
    VPP_BUFFER_PROCESSING,      //sent to VSP driver for process
    VPP_BUFFER_READY,           //VSP process done, ready to use
    VPP_BUFFER_LOADED,          //input only, decoded buffer loaded
    VPP_BUFFER_RENDERING        //output only, vpp buffer in RenderList
};

enum {
    VPP_OK = 0,
    VPP_BUFFER_NOT_READY,
    VPP_FAIL = -1
};

struct VPPBufferInfo {
    MediaBuffer* buffer;
    VPPBufferStatus status;
    uint32_t flags;
};

struct VPPVideoInfo {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
};

#define MAX_VPP_BUFFER_NUMBER 32

class VPPProcessor : public MediaBufferObserver {
public:
    VPPProcessor(const sp<ANativeWindow> &native, OMXCodec* codec, VPPVideoInfo* pInfo);
    virtual ~VPPProcessor();

    /*
     * Get VPP on/off status from VppSettings
     * @return:
     *      true: vpp on
     *      false: vpp off
     */
    static bool isVppOn();

    /*
     * Set VPPWorker::mSeek flag to true, send run signal to VPPThread to
     * make sure VPPThread is activated, and then wait for VPPThread's ready
     * signal to reset all input and output buffers.
     */
    void seek();

    /*
     * Check whether there is empty input buffer to put decoder buffer in,
     * or RenderList is empty. Input buffer, output buffer and RenderList
     * will also be updated in it.
     * @return:
     *     true: need to set data into VPP
     *     false: NO need to set data into VPP
     */
    bool canSetDecoderBufferToVPP();

    /*
     * Set video decoder buffer to VPPProcesor, this buffer will be inserted
     * into RenderList, as well as input buffer if there is a room.
     * @param:
     *      buff: video decoder buffer
     * @return:
     *      VPP_OK: success
     *      VPP_FAIL: fail
     */
    status_t setDecoderBufferToVPP(MediaBuffer *buff);

    /*
     * Read buffer out from RenderList for rendering
     * @param:
     *      buffer: the buffers from Renderlist
     * @return:
     *      VPP_OK: success
     *      VPP_FAIL: fail
     *      VPP_BUFFER_NOT_READY: no buffer available to render
     *      ERROR_END_OF_STREAM: got end of stream
     */
    virtual status_t read(MediaBuffer **buffer);

    /*
     * Callback function for release MediaBuffer
     * (This is the virtual function of MediaBufferObserver)
     * @param:
     *      buffer: the buffer is releasing
     */
    virtual void signalBufferReturned(MediaBuffer *buffer);

     /*
      * indicate video stream has reached to end
      */
     void setEOS();
public:
    // number of extra input buffer needed by VPP
    uint32_t mInputBufferNum;
    // number of output buffer needed by VPP
    uint32_t mOutputBufferNum;

private:
    // In this init() function, firstly, bufferInfo will be set as OMXCodec's,
    // and then VPPWorker will be initialized. After both steps succeed,
    // VPPThread starts to run.
    status_t init();
    // init inputBuffer and outBuffer
    status_t initBuffers();
    // completely release all buffers
    void releaseBuffers();
    //Set video clip info to VppProcessor
    status_t updateVideoInfo(VPPVideoInfo* info);
    // flush buffers and renderlist for seek
    void flush();
    // stop thread if needed
    void quitThread();
    // return the BufferInfo accordingly to MediaBuffer
    OMXCodec::BufferInfo *findBufferInfo(MediaBuffer *buff);
    // cancel MediaBuffer to native window
    status_t cancelBufferToNativeWindow(MediaBuffer *buff);
    // dequeue MediaBuffer from native window
    MediaBuffer * dequeueBufferFromNativeWindow();
    // get MediaBuffer's time stamp from meta data field
    int64_t getBufferTimestamp(MediaBuffer * buff);
    // release useless input buffers as well
    status_t clearInput();
    // add output buffer into Renderlist
    status_t updateRenderList();
    // debug only
    void printBuffers();
    void printRenderList();

private:
    // buffer info for VPP input
    VPPBufferInfo mInput[MAX_VPP_BUFFER_NUMBER];
    // buffer info for VPP output
    VPPBufferInfo mOutput[MAX_VPP_BUFFER_NUMBER];
    // mRenderList is used to render
    List<MediaBuffer *> mRenderList;
    // input load point
    uint32_t mInputLoadPoint;
    // output load to RenderList point
    uint32_t mOutputLoadPoint;

    MediaBuffer* mLastRenderBuffer;

    sp<VPPProcThread> mProcThread;
    sp<VPPFillThread> mFillThread;
    friend class VPPProcThread;
    friend class VPPFillThread;
    VPPWorker* mWorker;

    sp<ANativeWindow> mNativeWindow;
    OMXCodec* mCodec;
    // mBufferInfos is all buffer Infos allocated by OMXCodec
    Vector<OMXCodec::BufferInfo> * mBufferInfos;
    bool mThreadRunning;
    bool mEOS;
    uint32_t mTotalDecodedCount, mInputCount, mVPPProcCount, mVPPRenderCount;
};

} /* namespace android */

#endif /* __VPP_PROCESSOR_H */
