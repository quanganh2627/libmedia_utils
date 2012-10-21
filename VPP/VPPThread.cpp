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
#include "VPPThread.h"
#include <utils/Log.h>

#define LOG_TAG "VPPThread"

#define FRCX 2 //TODO: used for prototype

namespace android {

VPPThread::VPPThread(bool canCallJava, VPPProcessor* vppProcessor):
    Thread(canCallJava), mThreadId(NULL), mVPPProcessor(vppProcessor), mQuit(false), mSeek(false) {
    mWorker = new VPPWorker();
}

VPPThread::~VPPThread() {
    mQuit = false;
    mSeek = false;
    LOGV("vppthread is deleted");
}

status_t VPPThread::readyToRun() {
    mThreadId = androidGetThreadId();
    //do init ops here
    return Thread::readyToRun();
}

bool VPPThread::threadLoop() {
    uint32_t i = 0;
    uint32_t frcx = (mVPPProcessor->mOutputCount == 0) ? 1 : FRCX;
    bool flag = false;
    while (i < frcx) {
        if (mVPPProcessor->mOutputBufferStatus[(mVPPProcessor->mOutputVppPos + i) % VPPProcessor::OUTPUT_BUFFER_COUNT] != VPP_BUFFER_FREE) {
            flag = true;
            break;
        }
        i++;
    }
    if (flag || (mVPPProcessor->mInputBuffer[mVPPProcessor->mInputVppPos]) == NULL) {
        Mutex::Autolock autoLock(mLock);
        LOGV("waiting....................................");
        mRunCond.wait(mLock);
     }

    if ((mVPPProcessor->mInputBuffer[mVPPProcessor->mInputVppPos]) != NULL) {
        uint32_t i = 0;
        uint32_t frcx = (mVPPProcessor->mOutputCount == 0) ? 1 : FRCX;
        Vector<MediaBuffer *> buffers;
        uint32_t outputCount = 0;

        while (i < frcx) {
            uint32_t outputVppPos = (mVPPProcessor->mOutputVppPos + i) % VPPProcessor::OUTPUT_BUFFER_COUNT;
            if (mVPPProcessor->mOutputBufferStatus[outputVppPos] != VPP_BUFFER_FREE) {
                break;
            }
            buffers.push_back(mVPPProcessor->mOutputBuffer[outputVppPos]);
            i++;
        }

        if (i == frcx) {
            usleep(mVPPProcessor->mInputCount == 0 ? 50000ll : 15000ll);

            mWorker->process(mVPPProcessor->mNativeWindow,
                    mVPPProcessor->mInputBuffer[mVPPProcessor->mInputVppPos],
                    buffers, &outputCount, (mVPPProcessor->mOutputCount == 0) ? true : false);
            if (outputCount != 0) {
                uint32_t outputVppPos = 0;
                for (uint32_t i = 0; i < outputCount; i++) {
                    outputVppPos = (mVPPProcessor->mOutputVppPos + i) % VPPProcessor::OUTPUT_BUFFER_COUNT;
                    mVPPProcessor->mOutputBufferStatus[outputVppPos] = VPP_BUFFER_READY_FOR_USE;
                    mVPPProcessor->mOutputBuffer[outputVppPos]->add_ref();
                    LOGV("do vpp, output buffer = %p, pos = %d, ref_count = %d, outputVppPos = %d, outputCount = %d",
                            mVPPProcessor->mOutputBuffer[outputVppPos], outputVppPos,
                            mVPPProcessor->mOutputBuffer[outputVppPos]->refcount(),
                            mVPPProcessor->mOutputVppPos, outputCount);
                }
                mVPPProcessor->mInputVppPos = (mVPPProcessor->mInputVppPos + 1) % VPPProcessor::INPUT_BUFFER_COUNT;
                mVPPProcessor->mOutputCount = mVPPProcessor->mOutputCount + outputCount;
                mVPPProcessor->mOutputVppPos = (mVPPProcessor->mOutputVppPos + outputCount) % VPPProcessor::OUTPUT_BUFFER_COUNT;
            }
        }
    }

    {
        Mutex::Autolock autoLock(mLock);
        if (mQuit) {
            mResetCond.signal();
            return false;
        }

        if (mSeek) {
            mResetCond.signal();
            mRunCond.wait(mLock);
        }
    }

    return true;
}

bool VPPThread::isCurrentThread() const {
    return mThreadId == androidGetThreadId();
}

} /* namespace android */
