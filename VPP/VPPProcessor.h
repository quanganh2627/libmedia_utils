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
#include "VPPThread.h"

#include <stdint.h>

#include <android/native_window.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/OMXCodec.h>

namespace android {

struct MediaBuffer;
struct MediaBufferObserver;
struct OMXCodec;
class VPPThread;

enum VPPBufferStatus {
    VPP_BUFFER_FREE = 0,        //vpp output buffer is free
    VPP_BUFFER_READY_FOR_USE,   //vpp output buffer is full, but it is not put into RenderList
    VPP_BUFFER_USED             //vpp output is put into RenderList
};

enum {
    VPP_OK = 0,
    VPP_FAIL = -1
};

class VPPProcessor : public MediaBufferObserver {

public:
    static const uint32_t INPUT_BUFFER_COUNT = 5;
    static const uint32_t OUTPUT_BUFFER_COUNT = 6;

public:
    VPPProcessor();
    virtual ~VPPProcessor();

    /*
     * seek
     */
    void seek();

    /*
     * check whether there is empty input buffer
     * @return:
     *      if yes, canSetDecoderBufferToVPP return true
     *      else, canSetDecoderBufferToVPP return false
     */
    bool canSetDecoderBufferToVPP();

    /*
     * set video decoder buffer to VPPProcesor
     * @param:
     *      buff: video decoder buffer
     * @return:
     *      VPP_OK: success,
     *      VPP_FAIL: buff is not set to VPPProcessor
     */
    status_t setDecoderBufferToVPP(MediaBuffer *buff);

    /*
     * Read output from VPPProcessor for rendering
     * @param:
     *      buffer: the buffers get from VPPProcessor
     * @return:
     *      VPP_OK: success
     *      VPP_FAIL: fail
     *      ERROR_END_OF_STREAM: got end of stream
     */
    virtual status_t read(MediaBuffer **buffer);

    /*
     * set native window handle to VPPProcessor and also dequeue VPP buffers
     * @param:
     *      native: native window handle
     * @return:
     *      VPP_OK: success
     *      VPP_FAIL: fail
     */
    status_t setNativeWindow(const sp<ANativeWindow> &native);

    /*
     * set decoder's bufferInfo to VPPProcessor
     * @param:
     *      codec: decoder
     * @return:
     *      VPP_OK: success
     *      VPP_FAIL: fail
     */
    status_t setBufferInfoFromDecoder(OMXCodec *codec);

    /*
     * callback function for release MediaBuffer
     * (This is the virtual function of MediaBufferObserver)
     * @param:
     *      buffer: the buffer is releasing
     */
    virtual void signalBufferReturned(MediaBuffer *buffer);

    /*
     * get VPP status from VppSettings
     * @return:
     *      true: vpp on
     *      false: vpp off
     */
    static bool getVppStatus();

private:
    /* intialize all buffers */
    void initBuffers();
    /*
     * completely release buffer
     * which mean reduce its reference count to ZERO
     */
    status_t releaseBuffer(MediaBuffer * buff);
    /* completely release all buffers */
    void resetBuffers();
    void printBuffers();
    void printRenderList();
    /* stop thread if needed then release all buffers */
    void reset();

    OMXCodec::BufferInfo *findBufferInfo(MediaBuffer *buff);
    status_t dequeueOutputBuffersFromNativeWindow();
    status_t cancelBufferToNativeWindow(MediaBuffer *buff);
    MediaBuffer * dequeueBufferFromNativeWindow();

    int64_t getBufferTimestamp(MediaBuffer * buff);

    status_t addOutputBufferToRenderList(uint32_t pos);

private:
    // mInputBuffer is used to contain decoder buffer
    MediaBuffer* mInputBuffer[INPUT_BUFFER_COUNT];
    // mOutputBuffer is used to contain VPP output buffer
    MediaBuffer* mOutputBuffer[OUTPUT_BUFFER_COUNT];
    VPPBufferStatus mOutputBufferStatus[OUTPUT_BUFFER_COUNT];
    // mRenderList is used to render
    List<MediaBuffer *> mRenderList;

    uint32_t mInputCount;//input buffer total count
    uint32_t mOutputCount;//output buffer total count

    uint32_t mInputVppPos;//process pos in input buffers
    uint32_t mOutputVppPos;//process pos in output buffers

    uint32_t mOutputAddToRenderPos;

    int64_t mLastRenderTime;

    sp<VPPThread> mThread;
    friend class VPPThread;

    sp<ANativeWindow> mNativeWindow;
    // mBufferInfos is all buffer Infos allocated by OMXCodec
    Vector<OMXCodec::BufferInfo> * mBufferInfos;
};

} /* namespace android */

#endif /* __VPP_PROCESSOR_H */
