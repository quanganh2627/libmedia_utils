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

#include <math.h>
#include "VPPProcThread.h"
#include "isv_profile.h"

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "isv-omxil"

using namespace android;

VPPProcThread::VPPProcThread(bool canCallJava,
        VPPWorker* vppWorker,
        sp<VPPProcThreadObserver> owner,
        bool frcOn, int32_t frameRate)
    :Thread(canCallJava),
    mpOwner(owner),
    mThreadId(NULL),
    mThreadRunning(false),
    mVPPWorker(vppWorker),
    mOutputProcIdx(0),
    mInputProcIdx(0),
    mNumTaskInProcesing(0),
    mFirstTimeStamp(0),
    mFrameRate(frameRate),
    mError(false),
    mbFlush(false),
    mFlagEnd(false),
    mFirstInputFrame(true),
    mFrcOn(frcOn)
{
    mOutputBuffers.clear();
    mInputBuffers.clear();
}

VPPProcThread::~VPPProcThread() {
    LOGV("VPPProcThread is deleted");
    flush();
    mOutputBuffers.clear();
    mInputBuffers.clear();
}

status_t VPPProcThread::readyToRun()
{
    mThreadId = androidGetThreadId();
    //do init ops here
    return Thread::readyToRun();
}

void VPPProcThread::start()
{
    LOGD_IF(ISV_THREAD_DEBUG, "VPPProcThread::start");
    this->run("VPPProcThread", ANDROID_PRIORITY_NORMAL);
    mThreadRunning = true;
    return;
}

void VPPProcThread::stop()
{
    LOGD_IF(ISV_THREAD_DEBUG, "VPPProcThread::stop");
    if(mThreadRunning) {
        this->requestExit();
        {
            Mutex::Autolock autoLock(mLock);
            mRunCond.signal();
        }
        this->requestExitAndWait();
        mThreadRunning = false;
    }
    return;
}

bool VPPProcThread::getBufForFirmwareOutput(Vector<buffer_handle_t> *fillBufList,uint32_t *fillBufNum){
    uint32_t i = 0;
    // output buffer number for filling
    *fillBufNum = 0;
    uint32_t needFillNum = 0;
    OMX_BUFFERHEADERTYPE *outputBuffer;

    //output data available
    needFillNum = mVPPWorker->getFillBufCount();
    if (mOutputBuffers.size() < needFillNum ||
            mInputBuffers.empty()) {
        LOGE("%s: no enough input or output buffer which need to be sync", __func__);
        return false;
    }

    if ((needFillNum == 0) || (needFillNum > 4))
       return false;

    Mutex::Autolock autoLock(mOutputLock);
    for (i = 0; i < needFillNum; i++) {
        //fetch the render buffer from the top of output buffer queue
        outputBuffer = mOutputBuffers.itemAt(i);
        buffer_handle_t fillBuf = reinterpret_cast<buffer_handle_t>(outputBuffer->pBuffer);
        fillBufList->push_back(fillBuf);
    }

    *fillBufNum  = i;
    return true;
}


bool VPPProcThread::updateFirmwareOutputBufStatus(uint32_t fillBufNum) {
    int64_t timeUs;
    OMX_BUFFERHEADERTYPE *outputBuffer;
    OMX_BUFFERHEADERTYPE *inputBuffer;
    OMX_ERRORTYPE err;

    if (mInputBuffers.empty()) {
        LOGE("%s: input buffer queue is empty. no buffer need to be sync", __func__);
        return false;
    }

    if (mOutputBuffers.size() < fillBufNum) {
        LOGE("%s: no enough output buffer which need to be sync", __func__);
        return false;
    }
    // remove one buffer from intput buffer queue
    {
        Mutex::Autolock autoLock(mInputLock);
        inputBuffer = mInputBuffers.itemAt(0);
        err = mpOwner->releaseBuffer(kPortIndexInput, inputBuffer, false);
        if (err != OMX_ErrorNone) {
            LOGE("%s: failed to fillInputBuffer", __func__);
            return false;
        }
        mInputBuffers.removeAt(0);
        LOGD_IF(ISV_THREAD_DEBUG, "%s: fetch buffer %u from input buffer queue for fill to decoder, and then queue size is %d", __func__,
                inputBuffer, mInputBuffers.size());
        mInputProcIdx--;
    }

    //set the time stamp for interpreted frames
    {
        Mutex::Autolock autoLock(mOutputLock);
        timeUs = mOutputBuffers[0]->nTimeStamp;

        for(uint32_t i = 0; i < fillBufNum; i++) {
            outputBuffer = mOutputBuffers.itemAt(i);
            if (fillBufNum > 1) {
                if(mFrameRate == 15)
                    timeUs -= 1000000ll * (fillBufNum - i - 1) / 30;
                else
                    timeUs -= 1000000ll * (fillBufNum - i - 1) / 60;
                outputBuffer->nTimeStamp = timeUs;
            }
            //return filled buffers for rendering
            err = mpOwner->releaseBuffer(kPortIndexOutput, outputBuffer, false);
            if (err != OMX_ErrorNone) {
                LOGE("%s: failed to releaseOutputBuffer", __func__);
                return false;
            }
            // remove filled buffers from output buffer queue
            mOutputBuffers.removeAt(i);
            LOGD_IF(ISV_THREAD_DEBUG, "%s: fetch buffer %u from output buffer queue for render, and then queue size is %d", __func__,
                    outputBuffer, mOutputBuffers.size());
        }
        mOutputProcIdx -= fillBufNum;
    }
    return true;
}


bool VPPProcThread::getBufForFirmwareInput(Vector<buffer_handle_t> *procBufList,
                                   buffer_handle_t *inputBuf,
                                   uint32_t *procBufNum)
{
    OMX_BUFFERHEADERTYPE *outputBuffer;
    OMX_BUFFERHEADERTYPE *inputBuffer;

    int32_t procBufCount = mVPPWorker->getProcBufCount();
    if ((procBufCount == 0) || (procBufCount > 4)) {
       return false;
    }
    //fetch a input buffer for processing
    {
        LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: acqiring mInputLock", __func__);
        Mutex::Autolock autoLock(mInputLock);
        LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: acqired mInputLock", __func__);
        if (mbFlush) {
            procBufCount = 1;
            *inputBuf = NULL;
        } else {
            inputBuffer = mInputBuffers.itemAt(mInputProcIdx);
            *inputBuf = reinterpret_cast<buffer_handle_t>(inputBuffer->pBuffer);
        }
        LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: releasing mInputLock", __func__);
    }
    //fetch output buffers for processing
    {
        LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: acqiring mOutputLock", __func__);
        Mutex::Autolock autoLock(mOutputLock);
        LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: acqired mOutputLock", __func__);
        for (int32_t i = 0; i < procBufCount; i++) {
            outputBuffer = mOutputBuffers.itemAt(mOutputProcIdx + i);
            procBufList->push_back(reinterpret_cast<buffer_handle_t>(outputBuffer->pBuffer));
        }
        *procBufNum = procBufCount;
        LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: releasing mOutputLock", __func__);
    }
    return true;
}


void VPPProcThread::updateFirmwareInputBufStatus(uint32_t procBufNum)
{
    OMX_BUFFERHEADERTYPE *outputBuffer;
    OMX_BUFFERHEADERTYPE *inputBuffer;

    inputBuffer = mInputBuffers.itemAt(mInputProcIdx);
    mInputProcIdx++;

    Mutex::Autolock autoLock(mOutputLock);
    for(uint32_t i = 0; i < procBufNum; i++) {
        outputBuffer = mOutputBuffers.editItemAt(mOutputProcIdx + i);
        // set output buffer timestamp as the same as input
        outputBuffer->nTimeStamp = inputBuffer->nTimeStamp;
        outputBuffer->nFilledLen = inputBuffer->nFilledLen;
        outputBuffer->nOffset = inputBuffer->nOffset;
        outputBuffer->nFlags = inputBuffer->nFlags;
        //outputBuffer->nTickCount = inputBuffer->nTickCount;
        //outputBuffer->pMarkData = intputBuffer->pMarkData;
    }
    mOutputProcIdx += procBufNum;
    return;
}


bool VPPProcThread::isReadytoRun()
{
    if (mInputProcIdx < mInputBuffers.size() 
            && (mOutputBuffers.size() - mOutputProcIdx) >= mVPPWorker->getProcBufCount())
       return true;
    else
       return false;
}


bool VPPProcThread::threadLoop() {
    uint32_t procBufNum = 0, fillBufNum = 0;
    buffer_handle_t inputBuf;
    Vector<buffer_handle_t> procBufList;
    Vector<buffer_handle_t> fillBufList;
    uint32_t flags = 0;
    bool bGetBufSuccess = true;

    Mutex::Autolock autoLock(mLock);

    if (!isReadytoRun() && !mbFlush) {
        mRunCond.wait(mLock);
    }

    if (isReadytoRun() || mbFlush) {
        procBufList.clear();
        bool bGetInBuf = getBufForFirmwareInput(&procBufList, &inputBuf, &procBufNum);
        if (bGetInBuf) {
            if (!mbFlush)
                flags = mInputBuffers[mInputProcIdx]->nFlags;
            status_t ret = mVPPWorker->process(inputBuf, procBufList, procBufNum, mbFlush, flags);
            // for seek and EOS
            if (mbFlush) {
                mVPPWorker->reset();
                flush();

                mFirstInputFrame = true;
                mNumTaskInProcesing = 0;
                mInputProcIdx = 0;
                mOutputProcIdx = 0;

                mbFlush = false;

                Mutex::Autolock endLock(mEndLock);
                mEndCond.signal();
                return true;
            }
            if (ret == STATUS_OK) {
                mNumTaskInProcesing++;
                updateFirmwareInputBufStatus(procBufNum);
            } else {
                LOGE("process error %d ...", __LINE__);
            }
        }
    }

    LOGV("mNumTaskInProcesing %d", mNumTaskInProcesing);
    while (mNumTaskInProcesing > mVPPWorker->mNumForwardReferences && bGetBufSuccess ) {
        fillBufList.clear();
        bGetBufSuccess = getBufForFirmwareOutput(&fillBufList, &fillBufNum);
        LOGV("bGetOutput %d, buf num %d", bGetBufSuccess, fillBufNum);
        if (bGetBufSuccess) {
            status_t ret = mVPPWorker->fill(fillBufList, fillBufNum);
            if (ret == STATUS_OK) {
                mNumTaskInProcesing--;
                LOGV("mNumTaskInProcesing: %d ...", mNumTaskInProcesing);
                updateFirmwareOutputBufStatus(fillBufNum);
            } else {
                mError = true;
                ALOGE("VPP read firmware data error! Thread EXIT...");
                return false;
            }
        }
    }

    LOGV("after mNumTaskInProcesing %d ...", mNumTaskInProcesing);
    return true;
}


bool VPPProcThread::isCurrentThread() const {
    return mThreadId == androidGetThreadId();
}

void VPPProcThread::addOutput(OMX_BUFFERHEADERTYPE* output)
{
    if (mbFlush) {
        mpOwner->releaseBuffer(kPortIndexOutput, output, true);
    }
    //push the buffer into the output queue if it is not full
    LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: acqiring mOutputLock", __func__);
    Mutex::Autolock autoLock(mOutputLock);
    LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: acqired mOutputLock", __func__);

    mOutputBuffers.push_back(output);
    LOGD_IF(ISV_THREAD_DEBUG, "%s: hold pBuffer %u in output buffer queue. input queue size is %d, output queue size is %d", __func__,
            output, mInputBuffers.size(), mOutputBuffers.size());

    mRunCond.signal();
    LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: releasing mOutputLock", __func__);
    return;
}

void VPPProcThread::addInput(OMX_BUFFERHEADERTYPE* input)
{
    if (mbFlush)
        mpOwner->releaseBuffer(kPortIndexInput, input, true);

    if (input->nFlags & OMX_BUFFERFLAG_EOS) {
        mpOwner->releaseBuffer(kPortIndexInput, input, true);
        mbFlush = true;
        mRunCond.signal();
    }
    //put the decoded buffer into fill buffer queue
    LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: acqiring mInputLock", __func__);
    Mutex::Autolock autoLock(mInputLock);
    LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: acqired mInputLock", __func__);

    mInputBuffers.push_back(input);
    LOGD_IF(ISV_THREAD_DEBUG, "%s: hold pBuffer %u in input buffer queue. intput queue size is %d. output queue size is %d", __func__,
            input, mInputBuffers.size(), mOutputBuffers.size());

    if (mFrcOn && mFrameRate == 0) {
        if (mFirstInputFrame) {
            mFirstTimeStamp = input->nTimeStamp;
            mFirstInputFrame = false;
        } else if (input->nTimeStamp != mFirstTimeStamp) {
            mFrameRate = ceil(1 / (input->nTimeStamp - mFirstTimeStamp) * 1E6);
            LOGD_IF(ISV_THREAD_DEBUG, "%s: calculate fps is %d", __func__, mFrameRate);
            mRunCond.signal();
        }
    } else
        mRunCond.signal();

    LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: releasing mInputLock", __func__);
    return;
}

void VPPProcThread::notifyFlush()
{
    if (mInputBuffers.empty() && mOutputBuffers.empty()) {
        LOGD_IF(ISV_THREAD_DEBUG, "%s: input and ouput buffer queue is empty, nothing need to do", __func__);
        return;
    }

    Mutex::Autolock autoLock(mLock);
    mbFlush = true;
    mRunCond.signal();
    LOGD_IF(ISV_THREAD_DEBUG, "wake up proc thread");
    return;
}

void VPPProcThread::waitFlushFinished()
{
    Mutex::Autolock endLock(mEndLock);
    LOGD_IF(ISV_THREAD_DEBUG, "waiting mEnd lock(seek finish) ");
    while(mbFlush) {
        mEndCond.wait(mEndLock);
    }
    return;
}

void VPPProcThread::flush()
{
    OMX_BUFFERHEADERTYPE* pBuffer = NULL;
    {
        Mutex::Autolock autoLock(mInputLock);
        while (!mInputBuffers.empty()) {
            pBuffer = mInputBuffers.itemAt(0);
            mpOwner->releaseBuffer(kPortIndexInput, pBuffer, true);
            LOGD_IF(ISV_THREAD_DEBUG, "%s: Flush the pBuffer %u in input buffer queue.", __func__, pBuffer);
            mInputBuffers.removeAt(0);
        }
    }
    {
        Mutex::Autolock autoLock(mOutputLock);
        while (!mOutputBuffers.empty()) {
            pBuffer = mOutputBuffers.itemAt(0);
            mpOwner->releaseBuffer(kPortIndexOutput, pBuffer, true);
            LOGD_IF(ISV_THREAD_DEBUG, "%s: Flush the pBuffer %u in output buffer queue.", __func__, pBuffer);
            mOutputBuffers.removeAt(0);
        }
    }
    //flush finished.
    return;
}
