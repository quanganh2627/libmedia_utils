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
#define LOG_TAG "VPPThread"
#include "VPPThread.h"
#include <utils/Log.h>

namespace android {

VPPThread::VPPThread(bool canCallJava, VPPProcessor* vppProcessor, VPPWorker* vppWorker):
    Thread(canCallJava),
    mSeek(false), mWait(false), mError(false),
    mThreadId(NULL),
    mVPPProcessor(vppProcessor),
    mVPPWorker(vppWorker),
    mInputProcIdx(0), mInputFillIdx(0),
    mOutputProcIdx(0), mOutputFillIdx(0),
    mFlagEnd(false) {
}

VPPThread::~VPPThread() {
    LOGV("vppthread is deleted");
}

status_t VPPThread::readyToRun() {
    mThreadId = androidGetThreadId();
    //do init ops here
    return Thread::readyToRun();
}

bool VPPThread::threadLoop() {
    LOGV("threadLoop");
    uint32_t i = 0;
    int64_t timeUs = 0;
    // output buffer number for processing
    uint32_t procBufNum;
    // output buffer number for filling
    uint32_t fillBufNum;
    // output vectors for processing
    Vector< sp<GraphicBuffer> > procBufList;
    // output vectors for output
    Vector< sp<GraphicBuffer> > fillBufList;
    sp<GraphicBuffer> inputBuf;
    bool isLastFrame = false;

    // if input buffer or output buffer is not ready, wait!
    if (mWait) {
        Mutex::Autolock autoLock(mLock);
        LOGV("waiting for input and output buffer ready...");
        mRunCond.wait(mLock);
    }

    // if we are asked to seek, send reset signal to notify caller
    // we are ready to seek, and then wait for seek completion signal
    if (mSeek) {
        Mutex::Autolock autoLock(mLock);
        mResetCond.signal();
        mRunCond.wait(mLock);
        mInputProcIdx = 0;
        mInputFillIdx = 0;
        mOutputProcIdx = 0;
        mOutputFillIdx = 0;
    }

    if (mFlagEnd) {
        // FlagEnd has been submitted, no need to continue processing
    } else if (mVPPProcessor->mInput[mInputProcIdx].status != VPP_BUFFER_LOADED && !mVPPProcessor->mEOS) {
        // if not in EOS and no valid input, wait!
        mWait = true;
        return true;
    } else {
        mWait = false;
        if (mVPPProcessor->mInput[mInputProcIdx].status != VPP_BUFFER_LOADED && mVPPProcessor->mEOS) {
            // It's the time to send END FLAG
            LOGV("set isLastFrame flag");
            isLastFrame = true;
        }
        if (!isLastFrame) {
            MediaBuffer* inputMediaBuffer = mVPPProcessor->mInput[mInputProcIdx].buffer;
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
        status_t ret = mVPPWorker->process(inputBuf, procBufList, procBufNum, isLastFrame);
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
            mError = true;
            return false;
        }
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
            mVPPProcessor->mInput[mInputFillIdx].status = VPP_BUFFER_READY;
            mInputFillIdx = (mInputFillIdx + 1) % mVPPProcessor->mInputBufferNum;
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
            mError = true;
            return false;
        }
    }
    return true;
}

bool VPPThread::isCurrentThread() const {
    return mThreadId == androidGetThreadId();
}

} /* namespace android */
