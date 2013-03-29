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
#define LOG_TAG "VPPProcThread"
#include "VPPProcThread.h"
#include <utils/Log.h>

namespace android {

VPPProcThread::VPPProcThread(bool canCallJava, VPPProcessor* vppProcessor, VPPWorker* vppWorker):
    Thread(canCallJava),
    mWait(false), mError(false),
    mThreadId(NULL),
    mVPPProcessor(vppProcessor),
    mVPPWorker(vppWorker),
    mInputProcIdx(0),
    mOutputProcIdx(0),
    mFlagEnd(false) {
}

VPPProcThread::~VPPProcThread() {
    LOGV("VPPProcThread is deleted");
}

status_t VPPProcThread::readyToRun() {
    mThreadId = androidGetThreadId();
    //do init ops here
    return Thread::readyToRun();
}

bool VPPProcThread::threadLoop() {
    uint32_t i = 0, flags = 0;
    int64_t timeUs = 0;
    // output buffer number for processing
    uint32_t procBufNum;
    // output vectors for processing
    Vector< sp<GraphicBuffer> > procBufList;
    sp<GraphicBuffer> inputBuf;
    bool isLastFrame = false;

    // if input buffer or output buffer is not ready, wait!
    if (mWait) {
        Mutex::Autolock autoLock(mLock);
        LOGV("waiting for input and output buffer ready...");
        mRunCond.wait(mLock);
    }

    if (mFlagEnd) {
        // FlagEnd has been submitted, no need to continue processing
    } else if (mVPPProcessor->mInput[mInputProcIdx].status != VPP_BUFFER_LOADED && !mVPPProcessor->mEOS) {
        // if not in EOS and no valid input, wait!
        mWait = true;
        return true;
    } else {
        Mutex::Autolock autoLock(mLock);
        mWait = false;
        if (mVPPProcessor->mInput[mInputProcIdx].status != VPP_BUFFER_LOADED && mVPPProcessor->mEOS) {
            // It's the time to send END FLAG
            LOGV("set isLastFrame flag");
            isLastFrame = true;
        }
        if (!isLastFrame) {
            MediaBuffer* inputMediaBuffer = mVPPProcessor->mInput[mInputProcIdx].buffer;
            flags = mVPPProcessor->mInput[mInputProcIdx].flags;
            procBufNum = mVPPWorker->getProcBufCount();
            inputBuf = inputMediaBuffer->graphicBuffer().get();
            // get input buffer timestamp
            inputMediaBuffer->meta_data()->findInt64(kKeyTime, &timeUs);
        } else {
            procBufNum = 1;
            inputBuf = NULL;
        }
        // prepare output vectors for processing
        for (i = 0; i < procBufNum; i++) {
            uint32_t procPos = (mOutputProcIdx + i) % mVPPProcessor->mOutputBufferNum;
            if (mVPPProcessor->mOutput[procPos].status != VPP_BUFFER_FREE) {
                mWait = true;
                return true;
            }
            sp<GraphicBuffer> procBuf = mVPPProcessor->mOutput[procPos].buffer->graphicBuffer().get();
            procBufList.push_back(procBuf);
        }

        // submit input and output pairs into VSP for process
        status_t ret = mVPPWorker->process(inputBuf, procBufList, procBufNum, isLastFrame, flags);
        if (ret == STATUS_OK) {
            mVPPProcessor->mInput[mInputProcIdx].status = VPP_BUFFER_PROCESSING;
            mInputProcIdx = (mInputProcIdx + 1) % mVPPProcessor->mInputBufferNum;
            for(i = 0; i < procBufNum; i++) {
                uint32_t procPos = (mOutputProcIdx + i) % mVPPProcessor->mOutputBufferNum;
                mVPPProcessor->mOutput[procPos].status = VPP_BUFFER_PROCESSING;
                mVPPProcessor->mOutput[procPos].buffer->add_ref();
                // set output buffer timestamp as the same as input
                mVPPProcessor->mOutput[procPos].buffer->meta_data()->setInt64(kKeyTime, timeUs);
            }
            mOutputProcIdx = (mOutputProcIdx + procBufNum) % mVPPProcessor->mOutputBufferNum;
            if (isLastFrame)
                mFlagEnd = true;
        }
        else {
            LOGE("process failed");
            mError = true;
            return false;
        }
    }

    return true;
}

bool VPPProcThread::isCurrentThread() const {
    return mThreadId == androidGetThreadId();
}

} /* namespace android */
