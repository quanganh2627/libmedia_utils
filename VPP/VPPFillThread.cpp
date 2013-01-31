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
#define LOG_TAG "VPPFillThread"
#include "VPPFillThread.h"
#include <utils/Log.h>

namespace android {

VPPFillThread::VPPFillThread(bool canCallJava, VPPProcessor* vppProcessor, VPPWorker* vppWorker):
    Thread(canCallJava),
    mWait(false), mError(false),
    mThreadId(NULL),
    mVPPProcessor(vppProcessor),
    mVPPWorker(vppWorker),
    mFirstInputFrame(true),
    mInputFillIdx(0),
    mOutputFillIdx(0) {
}

VPPFillThread::~VPPFillThread() {
    LOGV("VPPFillThread is deleted");
}

status_t VPPFillThread::readyToRun() {
    mThreadId = androidGetThreadId();
    //do init ops here
    return Thread::readyToRun();
}

bool VPPFillThread::threadLoop() {
    uint32_t i = 0;
    int64_t timeUs;
    // output buffer number for filling
    uint32_t fillBufNum;
    // output vectors for output
    Vector< sp<GraphicBuffer> > fillBufList;

    // if input buffer or output buffer is not ready, wait!
    if (mWait) {
        Mutex::Autolock autoLock(mLock);
        LOGV("waiting for output buffer ready...");
        mRunCond.wait(mLock);
    }

    // if we are asked to seek, send reset signal to notify caller
    // we are ready to seek, and then wait for seek completion signal
    if (mVPPProcessor->mSeeking) {
        Mutex::Autolock autoLock(mLock);
        mResetCond.signal();
        mRunCond.wait(mLock);
    }

    // fill output buffer available
    fillBufNum = mVPPWorker->getFillBufCount();
    if (fillBufNum > 0) {
        // prepare output vectors for filling
        for (i= 0; i < fillBufNum; i++) {
            uint32_t fillPos = (mOutputFillIdx + i) % mVPPProcessor->mOutputBufferNum;
            if (mVPPProcessor->mOutput[fillPos].status != VPP_BUFFER_PROCESSING) {
                mWait = true;
                return true;
            }
            sp<GraphicBuffer> fillBuf = mVPPProcessor->mOutput[fillPos].buffer->graphicBuffer().get();
            fillBufList.push_back(fillBuf);
        }
        status_t ret = mVPPWorker->fill(fillBufList, fillBufNum);
        if (ret == STATUS_OK) {
            if(mFirstInputFrame) {
                mFirstInputFrame = false;
            } else {
                mVPPProcessor->mInput[mInputFillIdx].status = VPP_BUFFER_READY;
                mInputFillIdx = (mInputFillIdx + 1) % mVPPProcessor->mInputBufferNum;
            }
            for(i = 0; i < fillBufNum; i++) {
                uint32_t outputVppPos = (mOutputFillIdx + i) % mVPPProcessor->mOutputBufferNum;
                mVPPProcessor->mOutput[outputVppPos].status = VPP_BUFFER_READY;
                if (fillBufNum > 1) {
                    // frc is enabled, output fps is 60, change timeStamp
                    mVPPProcessor->mOutput[outputVppPos].buffer->meta_data()->findInt64(kKeyTime, &timeUs);
                    timeUs -= 1000000ll * (fillBufNum - i - 1) / 60;
                    mVPPProcessor->mOutput[outputVppPos].buffer->meta_data()->setInt64(kKeyTime, timeUs);
                }
            }
            mOutputFillIdx = (mOutputFillIdx + fillBufNum) % mVPPProcessor->mOutputBufferNum;
        }
        else {
            ALOGE("FillError! Thread EXIT...");
            mError = true;
            return false;
        }
    }
    return true;
}

bool VPPFillThread::isCurrentThread() const {
    return mThreadId == androidGetThreadId();
}

} /* namespace android */
