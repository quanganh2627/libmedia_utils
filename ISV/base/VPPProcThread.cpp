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
#include <utils/Errors.h>

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "isv-omxil"

using namespace android;

#define MAX_RETRY_NUM   8

VPPProcThread::VPPProcThread(bool canCallJava,
        VPPWorker* vppWorker,
        sp<VPPProcThreadObserver> owner,
        uint32_t width, uint32_t height)
    :Thread(canCallJava),
    mpOwner(owner),
    mThreadId(NULL),
    mThreadRunning(false),
    mVPPWorker(vppWorker),
    mOutputProcIdx(0),
    mInputProcIdx(0),
    mNumTaskInProcesing(0),
    mNumRetry(0),
    mLastTimeStamp(0),
    mError(false),
    mbFlush(false),
    mbBypass(false),
    mFlagEnd(false),
    mFilters(0)
{
    //FIXME: for 1920 x 1088, we also consider it as 1080p
    if (mISVProfile == NULL)
        mISVProfile = new ISVProfile(width, (height == 1088) ? 1080 : height);

    // get platform VPP cap first
    mFilters = mISVProfile->getFilterStatus();

    // turn off filters if dynamic vpp/frc setting is off
    if (!ISVProfile::isVPPOn())
        mFilters &= FilterFrameRateConversion;

    if (!ISVProfile::isFRCOn())
        mFilters &= ~FilterFrameRateConversion;

    memset(&mFilterParam, 0, sizeof(mFilterParam));
    //FIXME: we don't support scaling yet, so set src region equal to dst region
    mFilterParam.srcWidth = mFilterParam.dstWidth = width;
    mFilterParam.srcHeight = mFilterParam.dstHeight = height;
    mOutputBuffers.clear();
    mInputBuffers.clear();
}

VPPProcThread::~VPPProcThread() {
    LOGV("VPPProcThread is deleted");
    flush();
    mOutputBuffers.clear();
    mInputBuffers.clear();

    mVPPWorker = NULL;
    mISVProfile = NULL;
    mFilters = 0;
    memset(&mFilterParam, 0, sizeof(mFilterParam));
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
    if (mOutputProcIdx < needFillNum ||
            mInputProcIdx < 1) {
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


status_t VPPProcThread::updateFirmwareOutputBufStatus(uint32_t fillBufNum) {
    int64_t timeUs;
    OMX_BUFFERHEADERTYPE *outputBuffer;
    OMX_BUFFERHEADERTYPE *inputBuffer;
    OMX_ERRORTYPE err;

    if (mInputBuffers.empty()) {
        LOGE("%s: input buffer queue is empty. no buffer need to be sync", __func__);
        return UNKNOWN_ERROR;
    }

    if (mOutputBuffers.size() < fillBufNum) {
        LOGE("%s: no enough output buffer which need to be sync", __func__);
        return UNKNOWN_ERROR;
    }
    // remove one buffer from intput buffer queue
    {
        Mutex::Autolock autoLock(mInputLock);
        inputBuffer = mInputBuffers.itemAt(0);
        err = mpOwner->releaseBuffer(kPortIndexInput, inputBuffer, false);
        if (err != OMX_ErrorNone) {
            LOGE("%s: failed to fillInputBuffer", __func__);
            return UNKNOWN_ERROR;
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
                if(mFilterParam.frameRate == 15)
                    outputBuffer->nTimeStamp = timeUs - 1000000ll * (fillBufNum - i - 1) / 30;
                else
                    outputBuffer->nTimeStamp = timeUs - 1000000ll * (fillBufNum - i - 1) / 60;
            }

            //return filled buffers for rendering
            err = mpOwner->releaseBuffer(kPortIndexOutput, outputBuffer, false);
            if (err != OMX_ErrorNone) {
                LOGE("%s: failed to releaseOutputBuffer", __func__);
                return UNKNOWN_ERROR;
            }

            LOGD_IF(ISV_THREAD_DEBUG, "%s: fetch buffer %u(timestamp %.2f ms) from output buffer queue for render, and then queue size is %d", __func__,
                    outputBuffer, outputBuffer->nTimeStamp/1E3, mOutputBuffers.size());
        }
        // remove filled buffers from output buffer queue
        mOutputBuffers.removeItemsAt(0, fillBufNum);
        mOutputProcIdx -= fillBufNum;
    }
    return OK;
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
        if (mbFlush) {
            outputBuffer = mOutputBuffers.itemAt(0);
            procBufList->push_back(reinterpret_cast<buffer_handle_t>(outputBuffer->pBuffer));
        } else {
            for (int32_t i = 0; i < procBufCount; i++) {
                outputBuffer = mOutputBuffers.itemAt(mOutputProcIdx + i);
                procBufList->push_back(reinterpret_cast<buffer_handle_t>(outputBuffer->pBuffer));
            }
        }
        *procBufNum = procBufCount;
        LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: releasing mOutputLock", __func__);
    }

    return true;
}


status_t VPPProcThread::updateFirmwareInputBufStatus(uint32_t procBufNum)
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
    return OK;
}


bool VPPProcThread::isReadytoRun()
{
    LOGD_IF(ISV_THREAD_DEBUG, "%s: mVPPWorker->getProcBufCount() return %d", __func__,
            mVPPWorker->getProcBufCount());
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
        LOGD_IF(ISV_THREAD_DEBUG, "%s: bGetOutput %d, buf num %d", __func__,
                bGetBufSuccess, fillBufNum);
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

    if (mbBypass) {
        // return this buffer to decoder
        mpOwner->releaseBuffer(kPortIndexInput, output, false);
        return;
    }

    {
        //push the buffer into the output queue if it is not full
        LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: acqiring mOutputLock", __func__);
        Mutex::Autolock autoLock(mOutputLock);
        LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: acqired mOutputLock", __func__);

        mOutputBuffers.push_back(output);
        LOGD_IF(ISV_THREAD_DEBUG, "%s: hold pBuffer %u in output buffer queue. Input queue size is %d, mInputProIdx %d.\
                Output queue size is %d, mOutputProcIdx %d", __func__,
                output, mInputBuffers.size(), mInputProcIdx,
                mOutputBuffers.size(), mOutputProcIdx);
        LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: releasing mOutputLock", __func__);
    }

    {
        Mutex::Autolock autoLock(mLock);
        mRunCond.signal();
    }
    return;
}

inline bool VPPProcThread::isFrameRateValid(uint32_t fps)
{
    return (fps == 15 || fps == 24 || fps == 25 || fps == 30 || fps == 50 || fps == 60) ? true : false;
}

status_t VPPProcThread::configFilters(OMX_BUFFERHEADERTYPE* buffer)
{
    if ((mFilters & FilterFrameRateConversion) != 0) {
        if (!isFrameRateValid(mFilterParam.frameRate)) {
            if (mNumRetry++ < MAX_RETRY_NUM) {
                int64_t deltaTime = buffer->nTimeStamp - mLastTimeStamp;
                mLastTimeStamp = buffer->nTimeStamp;
                if (deltaTime != 0)
                    mFilterParam.frameRate = ceil(1.0 / deltaTime * 1E6);
                if (!isFrameRateValid(mFilterParam.frameRate)) {
                    return NOT_ENOUGH_DATA;
                } else {
                    if (mFilterParam.frameRate == 50 || mFilterParam.frameRate == 60) {
                        LOGD_IF(ISV_THREAD_DEBUG, "%s: %d fps don't need do FRC, so disable FRC", __func__,
                                mFilterParam.frameRate);
                        mFilters &= ~FilterFrameRateConversion;
                        mFilterParam.frcRate = FRC_RATE_1X;
                    } else {
                        mFilterParam.frcRate = mISVProfile->getFRCRate(mFilterParam.frameRate);
                        LOGD_IF(ISV_THREAD_DEBUG, "%s: calculate fps is %d, frc rate is %d", __func__,
                                mFilterParam.frameRate, mFilterParam.frcRate);
                    }
                }
            } else {
                LOGD_IF(ISV_THREAD_DEBUG, "%s: exceed max retry to get a valid frame rate(%d), disable FRC", __func__,
                        mFilterParam.frameRate);
                mFilters &= ~FilterFrameRateConversion;
                mFilterParam.frcRate = FRC_RATE_1X;
            }
        }
    }

    if ((buffer->nFlags & OMX_BUFFERFLAG_TFF) != 0 ||
            (buffer->nFlags & OMX_BUFFERFLAG_BFF) != 0)
        mFilters |= FilterDeinterlacing;
    else
        mFilters &= ~FilterDeinterlacing;

    if (mFilters == 0) {
        LOGI("%s: no filter need to be config, bypass VPP", __func__);
        return UNKNOWN_ERROR;
    }

    //config filters to mVPPWorker
    return (mVPPWorker->configFilters(mFilters, &mFilterParam) == STATUS_OK) ? OK : UNKNOWN_ERROR;
}

void VPPProcThread::addInput(OMX_BUFFERHEADERTYPE* input)
{
    if (mbFlush) {
        mpOwner->releaseBuffer(kPortIndexInput, input, true);
        return;
    }

    if (mbBypass) {
        // return this buffer to framework
        mpOwner->releaseBuffer(kPortIndexOutput, input, false);
        return;
    }

    if (input->nFlags & OMX_BUFFERFLAG_EOS) {
        mpOwner->releaseBuffer(kPortIndexInput, input, true);
        notifyFlush();
        return;
    }

    status_t ret = configFilters(input);
    if (ret == NOT_ENOUGH_DATA) {
        // release this buffer if frc is not ready.
        mpOwner->releaseBuffer(kPortIndexInput, input, false);
        LOGD_IF(ISV_THREAD_DEBUG, "%s: frc rate is not ready, release this buffer %u, fps %d", __func__,
                input, mFilterParam.frameRate);
        return;
    } else if (ret == UNKNOWN_ERROR) {
        LOGD_IF(ISV_THREAD_DEBUG, "%s: configFilters failed, bypass VPP", __func__);
        mbBypass = true;
        mpOwner->releaseBuffer(kPortIndexOutput, input, false);
        return;
    }

    {
        //put the decoded buffer into fill buffer queue
        LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: acqiring mInputLock", __func__);
        Mutex::Autolock autoLock(mInputLock);
        LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: acqired mInputLock", __func__);

        mInputBuffers.push_back(input);
        LOGD_IF(ISV_THREAD_DEBUG, "%s: hold pBuffer %u in input buffer queue. Intput queue size is %d, mInputProIdx %d.\
                Output queue size is %d, mOutputProcIdx %d", __func__,
                input, mInputBuffers.size(), mInputProcIdx,
                mOutputBuffers.size(), mOutputProcIdx);
        LOGD_IF(ISV_COMPONENT_LOCK_DEBUG, "%s: releasing mInputLock", __func__);
    }

    {
        Mutex::Autolock autoLock(mLock);
        mRunCond.signal();
    }
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
