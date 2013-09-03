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

VPPProcThread::VPPProcThread(bool canCallJava, VPPWorker* vppWorker,
                VPPBuffer *inputBuffer, const uint32_t inputBufferNum,
                VPPBuffer *outputBuffer, const uint32_t outputBufferNum):
    Thread(canCallJava),
    mWait(false), mError(false), mEOS(false), mSeek(false),
    mThreadId(NULL),
    mVPPWorker(vppWorker),
    mInput(inputBuffer),
    mOutput(outputBuffer),
    mInputBufferNum(inputBufferNum),
    mOutputBufferNum(outputBufferNum),
    mInputProcIdx(0),
    mOutputProcIdx(0) {
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
    bool isEndFlag = false;

    Mutex::Autolock autoLock(mLock);
    // if input buffer or output buffer is not ready, wait!
    if (mWait && !mSeek) {
        LOGV("waiting for input and output buffer ready...");
        mRunCond.wait(mLock);
    }

    if (mInput[mInputProcIdx].mStatus != VPP_BUFFER_LOADED && !mEOS && !mSeek) {
        // if not in EOS and no valid input, wait!
        mWait = true;
        return true;
    } else {
        mWait = false;
        if (mInput[mInputProcIdx].mStatus != VPP_BUFFER_LOADED && (mEOS || mSeek)) {
            // It's the time to send END FLAG
            LOGV("set end flag");
            isEndFlag = true;
        }
        if (!isEndFlag) {
            flags = mInput[mInputProcIdx].mFlags;
            procBufNum = mVPPWorker->getProcBufCount();
            inputBuf = mInput[mInputProcIdx].mGraphicBuffer.get();
            // get input buffer timestamp
            timeUs = mInput[mInputProcIdx].mTimeUs;
        } else {
            procBufNum = 1;
            inputBuf = NULL;
        }
        // prepare output vectors for processing
        for (i = 0; i < procBufNum; i++) {
            uint32_t procPos = (mOutputProcIdx + i) % mOutputBufferNum;
            if (mOutput[procPos].mStatus != VPP_BUFFER_FREE) {
                mWait = true;
                return true;
            }
            sp<GraphicBuffer> procBuf = mOutput[procPos].mGraphicBuffer.get();
            procBufList.push_back(procBuf);
        }

        // submit input and output pairs into VSP for process
        status_t ret = mVPPWorker->process(inputBuf, procBufList, procBufNum, isEndFlag, flags);
        if (ret == STATUS_OK) {
            if (!isEndFlag) {
                mInput[mInputProcIdx].mStatus = VPP_BUFFER_PROCESSING;
                mInputProcIdx = (mInputProcIdx + 1) % mInputBufferNum;
                for(i = 0; i < procBufNum; i++) {
                    uint32_t procPos = (mOutputProcIdx + i) % mOutputBufferNum;
                    mOutput[procPos].mStatus = VPP_BUFFER_PROCESSING;
                    // set output buffer timestamp as the same as input
                    mOutput[procPos].mTimeUs = timeUs;
                }
                mOutputProcIdx = (mOutputProcIdx + procBufNum) % mOutputBufferNum;
            } else {
                mOutput[mOutputProcIdx].mStatus = VPP_BUFFER_END_FLAG;
                mSeek = false;
                mEOS = false;
                mInputProcIdx = 0;
                mOutputProcIdx = 0;
            }
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
