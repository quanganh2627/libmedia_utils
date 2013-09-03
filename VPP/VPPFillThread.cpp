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

VPPFillThread::VPPFillThread(bool canCallJava, VPPWorker* vppWorker,
                VPPBuffer *inputBuffer, const uint32_t inputBufferNum,
                VPPBuffer *outputBuffer, const uint32_t outputBufferNum):
    Thread(canCallJava),
    mWait(false), mError(false), mSeek(false),
    mThreadId(NULL),
    mVPPWorker(vppWorker),
    mInput(inputBuffer),
    mOutput(outputBuffer),
    mInputBufferNum(inputBufferNum),
    mOutputBufferNum(outputBufferNum),
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
    bool isEndFlag = false;
    // output vectors for output
    Vector< sp<GraphicBuffer> > fillBufList;

    Mutex::Autolock autoLock(mLock);
    // if input buffer or output buffer is not ready, wait!
    if (mWait && !mSeek) {
        LOGV("waiting for output buffer ready...");
        mRunCond.wait(mLock);
    }

    // fill output buffer available
    fillBufNum = mVPPWorker->getFillBufCount();
    if (fillBufNum > 0) {
        // prepare output vectors for filling
        for (i= 0; i < fillBufNum; i++) {
            uint32_t fillPos = (mOutputFillIdx + i) % mOutputBufferNum;
            if (mOutput[fillPos].mStatus != VPP_BUFFER_PROCESSING) {
                if (mOutput[mOutputFillIdx].mStatus == VPP_BUFFER_END_FLAG) {
                    // END FLAG
                    LOGV("last frame");
                    fillBufNum = 1;
                    isEndFlag = true;
                } else {
                    mWait = true;
                    return true;
                }
            }
            sp<GraphicBuffer> fillBuf = mOutput[fillPos].mGraphicBuffer.get();
            fillBufList.push_back(fillBuf);
        }
        status_t ret = mVPPWorker->fill(fillBufList, fillBufNum);
        if (ret == STATUS_OK) {
            if (mFirstInputFrame) {
                mFirstInputFrame = false;
            } else {
                mInput[mInputFillIdx].mStatus = VPP_BUFFER_READY;
                mInputFillIdx = (mInputFillIdx + 1) % mInputBufferNum;
            }
            if (isEndFlag) {
                mOutput[mOutputFillIdx].mStatus = VPP_BUFFER_FREE;
                mInputFillIdx = 0;
                mOutputFillIdx = 0;
                mFirstInputFrame = true;
                mWait = true;
                if (mSeek) {
                    mSeek = false;
                    Mutex::Autolock endLock(mEndLock);
                    LOGI("send out end signal, mInputFillIdx = %d",mInputFillIdx);
                    mEndCond.signal();
                }
            } else {
                for(i = 0; i < fillBufNum; i++) {
                    uint32_t outputVppPos = (mOutputFillIdx + i) % mOutputBufferNum;
                    mOutput[outputVppPos].mStatus = VPP_BUFFER_READY;
                    if (fillBufNum > 1) {
                        // frc is enabled, output fps is 60, change timeStamp
                        timeUs = mOutput[outputVppPos].mTimeUs;
                        timeUs -= 1000000ll * (fillBufNum - i - 1) / 60;
                        mOutput[outputVppPos].mTimeUs = timeUs;
                    }
                }
                mOutputFillIdx = (mOutputFillIdx + fillBufNum) % mOutputBufferNum;
            }
        } else {
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
