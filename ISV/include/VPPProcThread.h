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

#ifndef __VPP_PROC_THREAD_H
#define __VPP_PROC_THREAD_H

#include <media/stagefright/MetaData.h>
#include "VPPWorker.h"

#include <utils/threads.h>
#include <utils/Errors.h>

#define ISV_COMPONENT_LOCK_DEBUG 0
#define ISV_THREAD_DEBUG 0

using namespace android;

typedef enum {
    kPortIndexInput  = 0,
    kPortIndexOutput = 1
}PORT_INDEX;

class VPPProcThreadObserver: public RefBase
{
public:
    virtual OMX_ERRORTYPE releaseBuffer(PORT_INDEX index, OMX_BUFFERHEADERTYPE* pBuffer, bool bFlush) = 0;
};

class VPPProcThread : public Thread {
    public:

        VPPProcThread(bool canCallJava, VPPWorker* vppWorker, sp<VPPProcThreadObserver> observer, uint32_t width, uint32_t height);
        virtual ~VPPProcThread();

        virtual status_t readyToRun();

        // Derived class must implement threadLoop(). The thread starts its life
        // here. There are two ways of using the Thread object:
        // 1) loop: if threadLoop() returns true, it will be called again if
        //          requestExit() wasn't called.
        // 2) once: if threadLoop() returns false, the thread will exit upon return.
        virtual bool threadLoop();
        bool isCurrentThread() const;

        void start();
        void stop();
        bool isReadytoRun();

        //add output buffer into mOutputBuffers
        void addOutput(OMX_BUFFERHEADERTYPE* output);
        //add intput buffer into mInputBuffers
        void addInput(OMX_BUFFERHEADERTYPE* input);
        //notify flush and wait flush finish
        void notifyFlush();
        void waitFlushFinished();

    private:
        VPPProcThread(const VPPProcThread &);
        VPPProcThread &operator=(const VPPProcThread &);
        bool getBufForFirmwareOutput(Vector<buffer_handle_t> *fillBufList,
                             uint32_t *fillBufNum);
        bool updateFirmwareOutputBufStatus(uint32_t fillBufNum);
        bool getBufForFirmwareInput(Vector<buffer_handle_t> *procBufList,
                            buffer_handle_t *inputBuf,
                            uint32_t *procBufNum );
        void updateFirmwareInputBufStatus(uint32_t procBufNum);
        void flush();
        inline bool isFrameRateValid(uint32_t fps);
    private:
        sp<VPPProcThreadObserver> mpOwner;
        android_thread_id_t mThreadId;
        bool mThreadRunning;

        VPPWorker *mVPPWorker;
        sp<ISVProfile> mISVProfile;

        Vector<OMX_BUFFERHEADERTYPE*> mOutputBuffers;
        Mutex mOutputLock; // to protect access to mOutputBuffers
        uint32_t mOutputProcIdx;

        Vector<OMX_BUFFERHEADERTYPE*> mInputBuffers;
        Mutex mInputLock; // to protect access to mFillBuffers
        uint32_t mInputProcIdx;

        // conditon for thread running
        Mutex mLock;
        Condition mRunCond;

        // condition for seek finish
        Mutex mEndLock;
        Condition mEndCond;

        uint32_t mNumTaskInProcesing;
        uint32_t mNumRetry;
        int64_t mLastTimeStamp;
        bool mError;
        bool mbFlush;
        bool mFlagEnd;
        bool mFrcOn;

        // VPP filter configuration
        uint32_t mFilters;
        FilterParam mFilterParam;
};

#endif /* __VPP_THREAD_H*/
