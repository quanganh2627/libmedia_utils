/*
 * Copyright (C) 2010 The Android Open Source Project
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
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "NuPlayerVPPProcessor"
#include <utils/Log.h>

#include "NuPlayerVPPProcessor.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaDefs.h>

namespace android {

NuPlayerVPPProcessor::NuPlayerVPPProcessor(
        const sp<AMessage> &notify,
        const sp<NativeWindowWrapper> &nativeWindow)
    : mNotify(notify),
      mNativeWindow(nativeWindow),
      mBufferInfos(NULL),
      mInputBufferNum(0),
      mOutputBufferNum(0),
      mThreadRunning(false),
      mInputLoadPoint(0),
      mOutputLoadPoint(0),
      mInputCount(0),
      mACodec(NULL),
      mEOS(false),
      mLastInputTimeUs(-1) {

    mWorker = VPPWorker::getInstance(mNativeWindow->getNativeWindow());
}


NuPlayerVPPProcessor::~NuPlayerVPPProcessor() {
    quitThread();

    if (mWorker != NULL) {
        delete mWorker;
        mWorker = NULL;
    }

    if (mThreadRunning == false) {
        releaseBuffers();
    }
    mNuPlayerVPPProcessor = NULL;
    LOGI("===== VPPInputCount = %d  =====", mInputCount);
}

//static
NuPlayerVPPProcessor* NuPlayerVPPProcessor::mNuPlayerVPPProcessor = NULL;

//static
NuPlayerVPPProcessor* NuPlayerVPPProcessor::getInstance(
        const sp<AMessage> &notify,
        const sp<NativeWindowWrapper> &nativeWindow) {
    if (mNuPlayerVPPProcessor == NULL) {
        // If no instance is existing, create one
        mNuPlayerVPPProcessor = new NuPlayerVPPProcessor(notify, nativeWindow);
        if (mNuPlayerVPPProcessor != NULL && mNuPlayerVPPProcessor->mWorker == NULL) {
            // If VPPWorker instance is not got successfully, delete VPPProcessor
            delete mNuPlayerVPPProcessor;
            mNuPlayerVPPProcessor = NULL;
        }
    } else if (mNuPlayerVPPProcessor->mWorker != NULL &&
            !mNuPlayerVPPProcessor->mWorker->validateNativeWindow(nativeWindow->getNativeWindow()))
        // If one instance is existing, check if the caller share the same NativeWindow handle
        return NULL;

    return mNuPlayerVPPProcessor;
}

void NuPlayerVPPProcessor::invokeThreads() {
    if (!mThreadRunning)
        return;

    mProcThread->mRunCond.signal();
    mFillThread->mRunCond.signal();
}

status_t NuPlayerVPPProcessor::canSetBufferToVPP() {
    if (!mThreadRunning)
        return VPP_FAIL;

    if (mInput[mInputLoadPoint].mStatus == VPP_BUFFER_FREE) {
        return VPP_OK;
    }

    return VPP_FAIL;
}


status_t NuPlayerVPPProcessor::setBufferToVPP(const sp<ABuffer> &buffer, const sp<AMessage> &notifyConsumed) {
    if (!mThreadRunning)
        return VPP_OK;

    if (buffer == NULL || notifyConsumed == NULL)
        return VPP_FAIL;

    /*
     * mLastInputTimeUs >= timeBuf, means this incoming buffer is abnormal,
     * so we never send it to VPP, and mark it as notVppInput
     */
    int64_t timeBuf = getBufferTimestamp(buffer);
    if (mLastInputTimeUs >= timeBuf) {
        notifyConsumed->setInt32("notVppInput", true);
        return VPP_OK;
    }

    IOMX::buffer_id bufferID;
    CHECK(notifyConsumed->findPointer("buffer-id", &bufferID));

    ACodec::BufferInfo *info = findBufferByID(bufferID);
    CHECK(info != NULL);
    sp<GraphicBuffer> graphicBuffer = info->mGraphicBuffer;

    mInput[mInputLoadPoint].mGraphicBuffer = graphicBuffer;
    mInput[mInputLoadPoint].mTimeUs = timeBuf;
    notifyConsumed->setInt32("vppInput", true);
    mInput[mInputLoadPoint].mCodecMsg = notifyConsumed;
    mInput[mInputLoadPoint].mStatus = VPP_BUFFER_LOADED;

    mInputLoadPoint = (mInputLoadPoint + 1) % mInputBufferNum;
    mInputCount ++;
    mLastInputTimeUs = timeBuf;

    return VPP_OK;
}

void NuPlayerVPPProcessor::getBufferFromVPP() {
    if (!mThreadRunning)
        return;

//    printBuffers();

    while (mOutput[mOutputLoadPoint].mStatus == VPP_BUFFER_READY) {

        sp<AMessage> notifyConsumed = new AMessage(ACodec::kWhatOutputBufferDrained, mACodec->id());
        ACodec::BufferInfo *info = findBufferByGraphicBuffer(mOutput[mOutputLoadPoint].mGraphicBuffer);
        CHECK(info != NULL);

        info->mData->meta()->setInt64("timeUs", mOutput[mOutputLoadPoint].mTimeUs);
        notifyConsumed->setPointer("buffer-id", info->mBufferID);
        notifyConsumed->setInt32("vppOutput", true);

        sp<AMessage> vppNotifyConsumed = new AMessage(kWhatFreeBuffer, id());
        vppNotifyConsumed->setInt32("index", mOutputLoadPoint);
        notifyConsumed->setMessage("vppNotifyConsumed", vppNotifyConsumed);

        sp<AMessage> vppNotify = mNotify->dup();
        vppNotify->setInt32("what", kWhatUpdateVppOutput);
        vppNotify->setMessage("notifyConsumed", notifyConsumed);
        vppNotify->setBuffer("buffer", info->mData);
        vppNotify->post();

        LOGV("vpp output buffer index = %d, buffer = %p, timeUs = %lld",
                mOutputLoadPoint, mOutput[mOutputLoadPoint].mGraphicBuffer.get(), mOutput[mOutputLoadPoint].mTimeUs);

        mOutput[mOutputLoadPoint].mStatus = VPP_BUFFER_RENDERING;
        mOutputLoadPoint = (mOutputLoadPoint + 1) % mOutputBufferNum;
    }
    clearInput();
}

void NuPlayerVPPProcessor::onMessageReceived(const sp<AMessage> &msg) {
    switch(msg->what()) {
        case kWhatFreeBuffer:
        {
            onFreeBuffer(msg);
            break;
        }

        default:
        {
            TRESPASS();
            break;
        }
    }
}

void NuPlayerVPPProcessor::onFreeBuffer(const sp<AMessage> &msg) {
    int32_t reuse = 0, index = 0;
    CHECK(msg->findInt32("reuse", &reuse));
    CHECK(msg->findInt32("index", &index));

    /*
     * flushShutdown quits threads
     * and there are still some rendering buffer not processed, process here
     */
    if (!mThreadRunning) {
        ACodec::BufferInfo * info = findBufferByGraphicBuffer(mOutput[index].mGraphicBuffer);
        if (info != NULL) {
            LOGV("cancel buffer after thread quit ; graphicBuffer = %p", mOutput[index].mGraphicBuffer.get());
            cancelBufferToNativeWindow(info);
            resetBuffer(index, VPP_OUTPUT, NULL);
        }
    } else {
        if (reuse == 1) {
            resetBuffer(index, VPP_OUTPUT, mOutput[index].mGraphicBuffer);
        } else if (reuse == 0) {
            ACodec::BufferInfo *info = dequeueBufferFromNativeWindow();
            CHECK(info != NULL);
            resetBuffer(index, VPP_OUTPUT, info->mGraphicBuffer);
        }
    }

}

int64_t NuPlayerVPPProcessor::getBufferTimestamp(sp<ABuffer> buffer) {
    int64_t timeUs;
    CHECK(buffer->meta()->findInt64("timeUs", &timeUs));
    return timeUs;
}

status_t NuPlayerVPPProcessor::validateVideoInfo(VPPVideoInfo *videoInfo){
    if (videoInfo == NULL || mWorker == NULL)
        return VPP_FAIL;
    if (mWorker->configFilters(videoInfo->width, videoInfo->height, videoInfo->fps) != VPP_OK)
        return VPP_FAIL;
    mInputBufferNum = mWorker->mNumForwardReferences + 3;
    mOutputBufferNum = (mWorker->mNumForwardReferences + 2) * mWorker->mFrcRate;
    if (mInputBufferNum > VPPBuffer::MAX_VPP_BUFFER_NUMBER
            || mOutputBufferNum > VPPBuffer::MAX_VPP_BUFFER_NUMBER) {
        LOGE("buffer number needed are exceeded limitation");
        return VPP_FAIL;
    }
    return VPP_OK;
}

// static
bool NuPlayerVPPProcessor::isVppOn() {
    return VPPBuffer::isVppOn();
}

ACodec::BufferInfo * NuPlayerVPPProcessor::findBufferByID(IOMX::buffer_id bufferID) {
    if (mBufferInfos == NULL)
        return NULL;

    for (size_t i = 0; i < mBufferInfos->size(); ++i) {
        ACodec::BufferInfo *info = &mBufferInfos->editItemAt(i);

        if (info != NULL && info->mBufferID == bufferID) {
            return info;
        }
    }

    return NULL;
}

ACodec::BufferInfo * NuPlayerVPPProcessor::findBufferByGraphicBuffer(sp<GraphicBuffer> graphicBuffer) {
    if (mBufferInfos == NULL)
        return NULL;

    for (size_t i = 0; i < mBufferInfos->size(); ++i) {
        ACodec::BufferInfo *info = &mBufferInfos->editItemAt(i);

        if (info != NULL && info->mGraphicBuffer == graphicBuffer) {
            return info;
        }
    }

    return NULL;
}

status_t NuPlayerVPPProcessor::init(sp<ACodec> &codec) {
    LOGI("init");
    if (codec == NULL || mWorker == NULL)
        return VPP_FAIL;

    mACodec = codec;

    // set BufferInfo from decoder
    if (mBufferInfos == NULL) {
        mBufferInfos = &codec->mBuffers[codec->kPortIndexOutput];
        if(mBufferInfos == NULL)
            return VPP_FAIL;
        uint32_t size = mBufferInfos->size();
        if (mInputBufferNum == 0 || mOutputBufferNum == 0
                || size <= mInputBufferNum + mOutputBufferNum
                || mInputBufferNum > VPPBuffer::MAX_VPP_BUFFER_NUMBER
                || mOutputBufferNum > VPPBuffer::MAX_VPP_BUFFER_NUMBER) {
            LOGE("input or output buffer number is invalid");
            return VPP_FAIL;
        }
        for (uint32_t i = 0; i < size; i++) {
            ACodec::BufferInfo *info = &mBufferInfos->editItemAt(i);
            GraphicBuffer* graphicBuffer = info->mGraphicBuffer.get();
            // set graphic buffer config to VPPWorker
            if(mWorker->setGraphicBufferConfig(graphicBuffer) == STATUS_OK)
                continue;
            else {
                LOGE("set graphic buffer config to VPPWorker failed");
                return VPP_FAIL;
            }
        }
    }

    if (initBuffers() != STATUS_OK)
        return VPP_FAIL;

    // init VPPWorker
    if(mWorker->init() != STATUS_OK)
        return VPP_FAIL;

    // VPPThread starts to run
    mProcThread = new VPPProcThread(false, mWorker,
            mInput, mInputBufferNum,
            mOutput, mOutputBufferNum,
            &mEOS);
    mFillThread = new VPPFillThread(false, mWorker,
            mInput, mInputBufferNum,
            mOutput, mOutputBufferNum,
            &mEOS);
    if (mProcThread == NULL || mFillThread == NULL)
        return VPP_FAIL;
    mProcThread->run("VPPProcThread", ANDROID_PRIORITY_NORMAL);
    mFillThread->run("VPPFillThread", ANDROID_PRIORITY_NORMAL);
    mThreadRunning = true;

    return VPP_OK;

}

status_t NuPlayerVPPProcessor::initBuffers() {
    ACodec::BufferInfo *buf = NULL;
    uint32_t i;
    for (i = 0; i < mInputBufferNum; i++) {
        resetBuffer(i, VPP_INPUT, NULL);
    }

    for (i = 0; i < mOutputBufferNum; i++) {
        buf = dequeueBufferFromNativeWindow();
        if (buf == NULL)
            return VPP_FAIL;

        resetBuffer(i, VPP_OUTPUT, buf->mGraphicBuffer);
    }
    return VPP_OK;
}

ACodec::BufferInfo * NuPlayerVPPProcessor::dequeueBufferFromNativeWindow() {
    ANativeWindowBuffer *buf;
    if (native_window_dequeue_buffer_and_wait(mNativeWindow->getNativeWindow().get(), &buf) != 0) {
        LOGE("dequeueBuffer failed.");
        return NULL;
    }

    for (size_t i = mBufferInfos->size(); i-- > 0;) {
        ACodec::BufferInfo *info = &mBufferInfos->editItemAt(i);

        if (info->mGraphicBuffer->handle == buf->handle) {
            CHECK_EQ((int)info->mStatus,
                     (int)ACodec::BufferInfo::OWNED_BY_NATIVE_WINDOW);

            info->mStatus = ACodec::BufferInfo::OWNED_BY_VPP;
            LOGV("dequeueBufferFromNativeWindow graphicBuffer = %p", info->mGraphicBuffer.get());

            return info;
        }
    }

    return NULL;
}

void NuPlayerVPPProcessor::resetBuffer(int32_t index, int32_t type, sp<GraphicBuffer> buffer) {
    VPPBuffer* vppBuffer;

    if (type == VPP_INPUT) {
        vppBuffer = &mInput[index];
    } else {
        vppBuffer = &mOutput[index];
    }

    vppBuffer->mGraphicBuffer = buffer;
    vppBuffer->mTimeUs = 0;
    vppBuffer->mCodecMsg = NULL;
    vppBuffer->mStatus = VPP_BUFFER_FREE;
}

void NuPlayerVPPProcessor::releaseBuffers() {
    LOGI("releaseBuffers");
    for (uint32_t i = 0; i < mInputBufferNum; i++) {
        resetBuffer(i, VPP_INPUT, NULL);
    }

    for (uint32_t i = 0; i < mOutputBufferNum; i++) {
        ACodec::BufferInfo * info = findBufferByGraphicBuffer(mOutput[i].mGraphicBuffer);
        if (info != NULL) {
            cancelBufferToNativeWindow(info);
            resetBuffer(i, VPP_OUTPUT, NULL);
        }
    }

    mInputLoadPoint = 0;
    mOutputLoadPoint = 0;
    mLastInputTimeUs = -1;
}

status_t NuPlayerVPPProcessor::cancelBufferToNativeWindow(ACodec::BufferInfo *info) {
    CHECK((info->mStatus == ACodec::BufferInfo::OWNED_BY_VPP)
            || (info->mStatus == ACodec::BufferInfo::OWNED_BY_NATIVE_WINDOW));

    if (info->mStatus == ACodec::BufferInfo::OWNED_BY_VPP) {
        LOGV("cancelBuffer on buffer %p", info->mGraphicBuffer.get());

        int err = mNativeWindow->getNativeWindow()->cancelBuffer(
                mNativeWindow->getNativeWindow().get(), info->mGraphicBuffer.get(), -1);

        CHECK_EQ(err, 0);

        info->mStatus = ACodec::BufferInfo::OWNED_BY_NATIVE_WINDOW;
    }
    return OK;
}

void NuPlayerVPPProcessor::printBuffers() {
    for (uint32_t i = 0; i < mInputBufferNum; i++) {
        LOGI("input %d.   graphicBuffer = %p,  status = %d, time = %lld", i, mInput[i].mGraphicBuffer.get(), mInput[i].mStatus, mInput[i].mTimeUs);
    }
    for (uint32_t i = 0; i < mOutputBufferNum; i++) {
        LOGI("output %d.   graphicBuffer = %p,  status = %d, time = %lld", i, mOutput[i].mGraphicBuffer.get(), mOutput[i].mStatus, mOutput[i].mTimeUs);
    }

}

void NuPlayerVPPProcessor::clearInput() {
    // release useless input buffer
    for (uint32_t i = 0; i < mInputBufferNum; i++) {
        if (mInput[i].mStatus == VPP_BUFFER_READY) {

            ACodec::BufferInfo *info = findBufferByGraphicBuffer(mInput[i].mGraphicBuffer);
            if (info != NULL) {
                int32_t processing;
                if (info->mData->meta()->findInt32("processing", &processing) && (processing == 1)) {
                    LOGV("CLEARING processing flag, graphicBuffer = %p", mInput[i].mGraphicBuffer.get());
                    processing = 0;
                    info->mData->meta()->setInt32("processing", processing);
                }
            }

            sp<AMessage> vppNotify = mNotify->dup();
            vppNotify->setInt32("what", kWhatUpdateVppInput);
            vppNotify->setMessage("notifyConsumed", mInput[i].mCodecMsg);
            //vppNotify->setBuffer("buffer", buffer);
            vppNotify->post();

            resetBuffer(i, VPP_INPUT, NULL);
        }
    }
}

void NuPlayerVPPProcessor::quitThread() {
    LOGI("quitThread");
    if (mThreadRunning) {
        mFillThread->requestExit();
        {
            Mutex::Autolock autoLock(mFillThread->mLock);
            mFillThread->mRunCond.signal();
        }
        mFillThread->requestExitAndWait();
        mFillThread.clear();

        mProcThread->requestExit();
        {
            Mutex::Autolock autoLock(mProcThread->mLock);
            mProcThread->mRunCond.signal();
        }
        mProcThread->requestExitAndWait();
        mProcThread.clear();
    }
    mThreadRunning = false;
    return;
}

void NuPlayerVPPProcessor::seek() {
    LOGI("seek");
    if(mThreadRunning) {
        Mutex::Autolock procLock(mProcThread->mLock);
        Mutex::Autolock fillLock(mFillThread->mLock);
        flushNoShutdown();
        mLastInputTimeUs = -1;
    }
}

void NuPlayerVPPProcessor::flushNoShutdown() {
    // flush all input buffers which are not in PROCESSING state
    bool monitorNewLoadPoint = false;
    mInputLoadPoint = 0;
    for (uint32_t i = 0; i < mInputBufferNum; i++) {
        LOGI("INPUT %d, status = %d, graphicBuffer = %p, time = %lld", i, mInput[i].mStatus, mInput[i].mGraphicBuffer.get(), mInput[i].mTimeUs);
        if (mInput[i].mStatus != VPP_BUFFER_PROCESSING) {
            if (mInput[i].mStatus != VPP_BUFFER_FREE) {
                // clear "processing" flag
                ACodec::BufferInfo *info = findBufferByGraphicBuffer(mInput[i].mGraphicBuffer);
                if (info != NULL) {
                    info->mData->meta()->setInt32("processing", 0);
                }

                sp<AMessage> vppNotify = mNotify->dup();
                vppNotify->setInt32("what", kWhatUpdateVppInput);
                vppNotify->setMessage("notifyConsumed", mInput[i].mCodecMsg);
                vppNotify->post();

                resetBuffer(i, VPP_INPUT, NULL);
            }
            if (monitorNewLoadPoint) {
                // find the 1st frame after PROCESSING block
                mInputLoadPoint = i;
                monitorNewLoadPoint = false;
            }
        } else {
            monitorNewLoadPoint = true;
            ACodec::BufferInfo *info = findBufferByGraphicBuffer(mInput[i].mGraphicBuffer);
            if (info != NULL) {
                info->mData->meta()->setInt32("processing", 1);
                LOGE("set processing flag for graphicBuffer %p", mInput[i].mGraphicBuffer.get());
            }
        }
    }

    // process all VPP_BUFFER_READY status buffer first to avoid mOutputLoadPoint conflict that in Threads
    while (mOutput[mOutputLoadPoint].mStatus == VPP_BUFFER_READY) {
        resetBuffer(mOutputLoadPoint, VPP_OUTPUT, mOutput[mOutputLoadPoint].mGraphicBuffer);
        mOutputLoadPoint = (mOutputLoadPoint + 1) % mOutputBufferNum;
    }

    // flush all output buffers which are not in PROCESSING state
    monitorNewLoadPoint = true;
    for (uint32_t i = 0; i < mOutputBufferNum; i++) {
        LOGI("OUTPUT %d, status = %d, graphicBuffer = %p, time = %lld", i, mOutput[i].mStatus, mOutput[i].mGraphicBuffer.get(), mOutput[i].mTimeUs);
        if (mOutput[i].mStatus != VPP_BUFFER_PROCESSING) {
            if (mOutput[i].mStatus != VPP_BUFFER_FREE
                    && mOutput[i].mStatus != VPP_BUFFER_RENDERING) {
                resetBuffer(i, VPP_OUTPUT, mOutput[i].mGraphicBuffer);
            }
            monitorNewLoadPoint = true;
        } else {
            if (monitorNewLoadPoint) {
                // find the 1st frame in PROCESSING state
                mOutputLoadPoint = i;
                monitorNewLoadPoint = false;
            }
        }
    }

}

void NuPlayerVPPProcessor::setEOS() {
    if (!mThreadRunning)
        return;
    LOGI("set eos");
    mEOS = true;
}

void NuPlayerVPPProcessor::flushShutdown() {
    LOGV("flushShutdown");

    if(!mThreadRunning)
        return;

    quitThread();

    if (mWorker != NULL) {
        delete mWorker;
        mWorker = NULL;
    }

    printBuffers();

    if (!mThreadRunning) {
        for (uint32_t i = 0; i < mInputBufferNum; i++) {
            if(mInput[i].mStatus != VPP_BUFFER_FREE) {
                // last frame is NULL, do not need to release
                if (mInput[i].mGraphicBuffer != NULL) {
                    sp<AMessage> vppNotify = mNotify->dup();
                    vppNotify->setInt32("what", kWhatUpdateVppInput);
                    vppNotify->setMessage("notifyConsumed", mInput[i].mCodecMsg);
                    vppNotify->post();
                }
            }
            resetBuffer(i, VPP_INPUT, NULL);
        }

        for (uint32_t i = 0; i < mOutputBufferNum; i++) {
            if (mOutput[i].mStatus != VPP_BUFFER_RENDERING) {
                ACodec::BufferInfo * info = findBufferByGraphicBuffer(mOutput[i].mGraphicBuffer);
                if (info != NULL) {
                    cancelBufferToNativeWindow(info);
                    resetBuffer(i, VPP_OUTPUT, NULL);
                }
            }
        }
    }
    mInputLoadPoint = 0;
    mOutputLoadPoint = 0;
}

} //namespace android
