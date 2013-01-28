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
#include "VPPProcessor.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <ui/GraphicBuffer.h>

#include <utils/Log.h>
#define LOG_TAG "VPPProcessor"

#define VPP_STATUS_STORAGE "/data/data/com.intel.vpp/shared_prefs/vpp_settings.xml"

namespace android {

VPPProcessor::VPPProcessor(const sp<ANativeWindow> &native, VPPVideoInfo* pInfo)
        :mInputBufferNum(0), mOutputBufferNum(0),
         mInputLoadPoint(0), mOutputLoadPoint(0),
         mLastRenderBuffer(NULL),
         mNativeWindow(native),
         mBufferInfos(NULL),
         mThreadRunning(false), mEOS(false), mFirstFrameDone(false),
         mTotalDecodedCount(0), mInputCount(0), mVPPProcCount(0), mVPPRenderCount(0) {

    LOGI("construction");
    memset(mInput, 0, MAX_VPP_BUFFER_NUMBER * sizeof(VPPBufferInfo));
    memset(mOutput, 0, MAX_VPP_BUFFER_NUMBER * sizeof(VPPBufferInfo));

    mWorker = new VPPWorker (mNativeWindow);
    mThread = new VPPThread(false, this, mWorker);
    updateVideoInfo(pInfo);
}

VPPProcessor::~VPPProcessor() {
    quitThread();
    mThreadRunning = false;

    if (mWorker != NULL) {
        delete mWorker;
        mWorker == NULL;
    }

    releaseBuffers();
    LOGI("VPPProcessor is deleted");
}

//static
bool VPPProcessor::isVppOn() {
    FILE *handle = fopen(VPP_STATUS_STORAGE, "r");
    if(handle == NULL)
        return false;

    const int MAXLEN = 1024;
    char buf[MAXLEN] = {0};
    memset(buf, 0 ,MAXLEN);
    if(fread(buf, 1, MAXLEN, handle) <= 0) {
        fclose(handle);
        return false;
    }
    buf[MAXLEN - 1] = '\0';

    if(strstr(buf, "true") == NULL) {
        fclose(handle);
        return false;
    }

    fclose(handle);
    return true;
}

status_t VPPProcessor::init(OMXCodec *codec) {
    LOGV("init");
    if (codec == NULL || mWorker == NULL || mThread == NULL)
        return VPP_FAIL;

    // set BufferInfo from decoder
    if (mBufferInfos == NULL) {
        mBufferInfos = &codec->mPortBuffers[codec->kPortIndexOutput];
        if(mBufferInfos == NULL)
            return VPP_FAIL;
        uint32_t size = mBufferInfos->size();
        LOGV("mBufferInfo size is %d",size);
        CHECK(size > mInputBufferNum + mOutputBufferNum);
        if (mInputBufferNum > MAX_VPP_BUFFER_NUMBER || mOutputBufferNum > MAX_VPP_BUFFER_NUMBER) {
            LOGE("input or output buffer number exceeds limitation");
            return VPP_FAIL;
        }
        for (uint32_t i = 0; i < size; i++) {
            MediaBuffer* mediaBuffer = mBufferInfos->editItemAt(i).mMediaBuffer;
            if(mediaBuffer == NULL)
                return VPP_FAIL;
            GraphicBuffer* graphicBuffer = mediaBuffer->graphicBuffer().get();
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
    mThread->run("VPPThread", ANDROID_PRIORITY_NORMAL);
    mThreadRunning = true;

    return VPP_OK;
}

bool VPPProcessor::canSetDecoderBufferToVPP() {
    // invoke VPPThread as many as possible
    {
        Mutex::Autolock autoLock(mThread->mLock);
        mThread->mRunCond.signal();
    }

    // put VPP output which still in output array to RenderList
    CHECK(updateRenderList() == VPP_OK);
    // release obsolete input buffers
    clearInput();
    // in non-EOS status, if input vectors has free position or
    // we have no frame to render, then set Decoder buffer in
    if (!mEOS && (mRenderList.empty() || mInput[mInputLoadPoint].status == VPP_BUFFER_FREE))
        return true;
    return false;
}

status_t VPPProcessor::setDecoderBufferToVPP(MediaBuffer *buff) {
    if (buff != NULL) {
        mRenderList.push_back(buff);
        mTotalDecodedCount ++;
        // put buff in inputBuffers when there is empty buffer
        if (mInput[mInputLoadPoint].status == VPP_BUFFER_FREE) {
            buff->add_ref();
            mInput[mInputLoadPoint].status = VPP_BUFFER_LOADED;
            mInput[mInputLoadPoint].buffer = buff;
            mInputLoadPoint = (mInputLoadPoint + 1) % mInputBufferNum;
            mInputCount ++;
            return VPP_OK;
        }
    }
    return VPP_FAIL;
}

void VPPProcessor::printBuffers() {
    for (uint32_t i = 0; i < mInputBufferNum; i++) {
        LOGV("input %d.   %p,  status = %d, time = %lld", i, mInput[i].buffer, mInput[i].status, getBufferTimestamp(mInput[i].buffer));
    }
    for (uint32_t i = 0; i < mOutputBufferNum; i++) {
        LOGV("output %d.   %p,  status = %d, time = %lld", i, mOutput[i].buffer, mOutput[i].status, getBufferTimestamp(mOutput[i].buffer));
    }

}

void VPPProcessor::printRenderList() {
    List<MediaBuffer*>::iterator it;
    for (it = mRenderList.begin(); it != mRenderList.end(); it++) {
        LOGV("renderList: %p, timestamp = %lld", *it, (*it) ? getBufferTimestamp(*it) : 0);
    }
}

status_t VPPProcessor::read(MediaBuffer **buffer) {
    //printBuffers();
    //printRenderList();
    if (mThread->mError)
        return VPP_FAIL;
    if (mRenderList.empty() || !mFirstFrameDone) {
        if (!mEOS) {
            // no buffer ready to render
            return VPP_BUFFER_NOT_READY;
        }
        LOGV("GOT END OF STREAM!!!");
        *buffer = NULL;

        LOGD("======mTotalDecodedCount=%d, mInputCount=%d, mVPPProcCount=%d, mVPPRenderCount=%d======",
            mTotalDecodedCount, mInputCount, mVPPProcCount, mVPPRenderCount);
        return ERROR_END_OF_STREAM;
    }

    *buffer = *(mRenderList.begin());
    mLastRenderBuffer = *buffer;
    mRenderList.erase(mRenderList.begin());

    OMXCodec::BufferInfo *info = findBufferInfo(*buffer);
    if (info == NULL) return VPP_FAIL;
    info->mStatus = OMXCodec::OWNED_BY_CLIENT;

    return VPP_OK;
}

int64_t VPPProcessor::getBufferTimestamp(MediaBuffer * buff) {
    if (buff == NULL) return -1;
    int64_t timeUs;
    if (!buff->meta_data()->findInt64(kKeyTime, &timeUs))
        return -1;
    return timeUs;
}

void VPPProcessor::seek() {
    LOGV("seek");
    /* invoke thread if it is waiting */
    if (mThreadRunning) {
        Mutex::Autolock autoLock(mThread->mLock);
        mThread->mSeek = true;
        mThread->mRunCond.signal();
        LOGV("VPPProcessor::waiting.........................");
        mThread->mResetCond.wait(mThread->mLock);
    }

    flush();

    mThread->mSeek = false;
    mThread->mRunCond.signal();
}


void VPPProcessor::quitThread() {
    LOGV("quitThread");
    /* invoke thread if it is waiting */
    if(mThreadRunning) {
        Mutex::Autolock autoLock(mThread->mLock);
        mThread->mRunCond.signal();
    }
        mThread->requestExitAndWait();
    return;
}

void VPPProcessor::releaseBuffers() {
    LOGV("releaseBuffers");
    for (uint32_t i = 0; i < mInputBufferNum; i++) {
        if (mInput[i].buffer != NULL) {
            if (mInput[i].buffer->refcount() > 0)
                mInput[i].buffer->release();
            mInput[i].buffer = NULL;
            mInput[i].status = VPP_BUFFER_FREE;
        }
    }

    for (uint32_t i = 0; i < mOutputBufferNum; i++) {
        if (mOutput[i].buffer != NULL) {
            if (mOutput[i].buffer->refcount() > 0)
                mOutput[i].buffer->release();
            else {
                cancelBufferToNativeWindow(mOutput[i].buffer);
                mOutput[i].buffer = NULL;
                mOutput[i].status = VPP_BUFFER_FREE;
            }
        }
    }

    mInputLoadPoint = 0;
    mOutputLoadPoint = 0;

    mRenderList.clear();
}

void VPPProcessor::flush() {
    // flush all input buffers which are not in PROCESSING state
    LOGV("flush");
    bool monitorNewLoadPoint = false;
    mInputLoadPoint = 0;
    for (uint32_t i = 0; i < mInputBufferNum; i++) {
        if (mInput[i].status != VPP_BUFFER_PROCESSING) {
            if (mInput[i].status != VPP_BUFFER_FREE) {
                if (mInput[i].buffer->refcount() > 0)
                        mInput[i].buffer->release();
                mInput[i].buffer = NULL;
                mInput[i].status = VPP_BUFFER_FREE;
            }
            if (monitorNewLoadPoint) {
                // find the 1st frame after PROCESSING block
                mInputLoadPoint = i;
                monitorNewLoadPoint = false;
            }
        }
        else
            monitorNewLoadPoint = true;
    }
    // flush all output buffers which are not in PROCESSING state
    monitorNewLoadPoint = true;
    for (uint32_t i = 0; i < mOutputBufferNum; i++) {
        if (mOutput[i].status != VPP_BUFFER_PROCESSING) {
            if (mOutput[i].status != VPP_BUFFER_FREE) {
                if (mOutput[i].buffer->refcount() > 0)
                    mOutput[i].buffer->release();
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

    mRenderList.clear();
    printBuffers();
    printRenderList();
}

status_t VPPProcessor::clearInput() {
    // release useless input buffer
    for (uint32_t i = 0; i < mInputBufferNum; i++) {
        if (mInput[i].status == VPP_BUFFER_READY) {
            if (mInput[i].buffer !=  mLastRenderBuffer) {
                // to avoid buffer released before rendering
                if (mInput[i].buffer != NULL && mInput[i].buffer->refcount() > 0) {
                    mInput[i].buffer->release();
                }
                mInput[i].buffer = NULL;
                mInput[i].status = VPP_BUFFER_FREE;
            }
        }
    }
    return VPP_OK;
}

status_t VPPProcessor::updateRenderList() {
    LOGV("updateRenderList");
    while (mOutput[mOutputLoadPoint].status == VPP_BUFFER_READY) {
        mFirstFrameDone = true;
        MediaBuffer* buff = mOutput[mOutputLoadPoint].buffer;
        if (buff == NULL) return VPP_FAIL;

        int64_t timeBuffer = getBufferTimestamp(buff);
        if (timeBuffer == -1)
            return VPP_FAIL;

        List<MediaBuffer*>::iterator it;
        int64_t timeRenderList = 0;
        for (it = mRenderList.begin(); it != mRenderList.end(); it++) {
            if (*it == NULL) break;

            timeRenderList = getBufferTimestamp(*it);
            if (timeRenderList == -1)
                return VPP_FAIL;

            if (timeBuffer <= timeRenderList) {
                break;
            }
        }
        if (*it == NULL || (it == mRenderList.begin() && timeBuffer < timeRenderList)) {
            LOGV("1. vpp output comes too late, drop it, timeBuffer = %lld", timeBuffer);
            //vpp output comes too late, drop it
            if (buff->refcount() > 0)
                buff->release();
        } else if (timeBuffer == timeRenderList) {
            LOGV("2. timeBuffer = %lld, timeRenderList = %lld, going to erase %p, insert %p", timeBuffer, timeRenderList, *it, buff);
            //same timestamp, use vpp output to replace the input
            MediaBuffer* input = *it;
            input->release();
            List<MediaBuffer*>::iterator erase = mRenderList.erase(it);
            mRenderList.insert(erase, buff);
            mVPPProcCount ++;
            mVPPRenderCount ++;
            mOutput[mOutputLoadPoint].status = VPP_BUFFER_RENDERING;
        } else if (timeBuffer < timeRenderList) {
            LOGV("3. timeBuffer = %lld, timeRenderList = %lld", timeBuffer, timeRenderList);
            //x.5 frame, just insert it
            mVPPRenderCount ++;
            mRenderList.insert(it, buff);
            mOutput[mOutputLoadPoint].status = VPP_BUFFER_RENDERING;
        }
        mOutputLoadPoint = (mOutputLoadPoint + 1) % mOutputBufferNum;
    }
    return VPP_OK;
}

OMXCodec::BufferInfo* VPPProcessor::findBufferInfo(MediaBuffer *buff) {
    OMXCodec::BufferInfo *info = NULL;
    for (uint32_t i = 0; i < mBufferInfos->size(); i++) {
        if (mBufferInfos->editItemAt(i).mMediaBuffer == buff) {
            info = &mBufferInfos->editItemAt(i);
            break;
        }
    }
    return info;
}

status_t VPPProcessor::cancelBufferToNativeWindow(MediaBuffer *buff) {
    LOGV("VPPProcessor::cancelBufferToNativeWindow buffer = %p", buff);
    int err = mNativeWindow->cancelBuffer(mNativeWindow.get(), buff->graphicBuffer().get(), -1);
    if (err != 0)
        return err;

    OMXCodec::BufferInfo *info = findBufferInfo(buff);
    if (info == NULL) return VPP_FAIL;

    if ((info->mStatus != OMXCodec::OWNED_BY_VPP)
            && info->mStatus != OMXCodec::OWNED_BY_CLIENT)
        return VPP_FAIL;
    info->mStatus = OMXCodec::OWNED_BY_NATIVE_WINDOW;
    info->mMediaBuffer->setObserver(NULL);

    return VPP_OK;
}

MediaBuffer * VPPProcessor::dequeueBufferFromNativeWindow() {
    LOGV("VPPProcessor::dequeueBufferFromNativeWindow");
    if (mNativeWindow == NULL || mBufferInfos == NULL)
        return NULL;

    ANativeWindowBuffer *buff;
    //int err = mNativeWindow->dequeueBuffer(mNativeWindow.get(), &buff);
    int err = native_window_dequeue_buffer_and_wait(mNativeWindow.get(), &buff);
    if (err != 0) {
        LOGE("dequeueBuffer from native window failed");
        return NULL;
    }

    OMXCodec::BufferInfo *info = NULL;
    for (uint32_t i = 0; i < mBufferInfos->size(); i++) {
        sp<GraphicBuffer> graphicBuffer = mBufferInfos->itemAt(i).mMediaBuffer->graphicBuffer();
        if (graphicBuffer->handle == buff->handle) {
            info = &mBufferInfos->editItemAt(i);
            break;
        }
    }
    if (info == NULL) return NULL;

    LOGV("VPPProcessor::dequeueBuffer = %p, status = %d", info->mMediaBuffer, info->mStatus);
    CHECK_EQ((int)info->mStatus, (int)OMXCodec::OWNED_BY_NATIVE_WINDOW);
    info->mStatus = OMXCodec::OWNED_BY_VPP;
    info->mMediaBuffer->setObserver(this);
    sp<MetaData> metaData = info->mMediaBuffer->meta_data();
    metaData->setInt32(kKeyRendered, 0);
    return info->mMediaBuffer;
}

status_t VPPProcessor::initBuffers() {
    MediaBuffer *buf = NULL;
    uint32_t i;
    for (i = 0; i < mInputBufferNum; i++) {
        mInput[i].buffer = NULL;
        mInput[i].status = VPP_BUFFER_FREE;
    }

    for (i = 0; i < mOutputBufferNum; i++) {
        buf = dequeueBufferFromNativeWindow();
        if (buf == NULL)
            return VPP_FAIL;

        mOutput[i].buffer = buf;
        mOutput[i].status = VPP_BUFFER_FREE;
    }
    return VPP_OK;
}

void VPPProcessor::signalBufferReturned(MediaBuffer *buff) {
    LOGV("VPPProcessor::signalBufferReturned, buff = %p", buff);
    if (buff == NULL) return;

    int32_t rendered = 0;
    sp<MetaData> metaData = buff->meta_data();
    if (! metaData->findInt32(kKeyRendered, &rendered)) {
        rendered = 0;
    }

    OMXCodec::BufferInfo *info = findBufferInfo(buff);
    if (info == NULL) return;

    if (info->mStatus == OMXCodec::OWNED_BY_CLIENT) {
        if (!rendered) {
            status_t err = cancelBufferToNativeWindow(buff);
            if (err != VPP_OK) return;
        }

        metaData->setInt32(kKeyRendered, 0);
        buff->setObserver(NULL);
        info->mStatus = OMXCodec::OWNED_BY_NATIVE_WINDOW;

        MediaBuffer * mediaBuffer = dequeueBufferFromNativeWindow();
        if (mediaBuffer == NULL) return;

        for (uint32_t i = 0; i < mOutputBufferNum; i++) {
            if (buff == mOutput[i].buffer) {
                mOutput[i].buffer = mediaBuffer;
                mOutput[i].status = VPP_BUFFER_FREE;
                break;
            }
        }
    } else if (info->mStatus == OMXCodec::OWNED_BY_VPP) {
        if (!mThreadRunning) {
            status_t err = cancelBufferToNativeWindow(buff);
            if (err != VPP_OK) return;

            buff->setObserver(NULL);
            info->mStatus = OMXCodec::OWNED_BY_NATIVE_WINDOW;

            for (uint32_t i = 0; i < mOutputBufferNum; i++) {
                if (buff == mOutput[i].buffer) {
                    mOutput[i].buffer = NULL;
                    mOutput[i].status = VPP_BUFFER_FREE;
                    break;
                }
            }
        } else {
            status_t err = cancelBufferToNativeWindow(buff);
            if (err != VPP_OK) return;

            buff->setObserver(NULL);
            info->mStatus = OMXCodec::OWNED_BY_NATIVE_WINDOW;

            MediaBuffer * mediaBuffer = dequeueBufferFromNativeWindow();
            if (mediaBuffer == NULL) return;

            for (uint32_t i = 0; i < mOutputBufferNum; i++) {
                if (buff == mOutput[i].buffer) {
                    mOutput[i].buffer = mediaBuffer;
                    mOutput[i].status = VPP_BUFFER_FREE;
                    break;
                }
            }
        }
    }

    return;
}

status_t VPPProcessor::updateVideoInfo(VPPVideoInfo * videoInfo)
{
    if (videoInfo == NULL || mWorker == NULL)
        return VPP_FAIL;
    mWorker->setVideoInfo(videoInfo->width, videoInfo->height, videoInfo->fps);
    mInputBufferNum = mWorker->mNumForwardReferences + 3;
    mOutputBufferNum = (mWorker->mNumForwardReferences + 2) * mWorker->mFrcRate;
    if (mInputBufferNum > MAX_VPP_BUFFER_NUMBER || mOutputBufferNum > MAX_VPP_BUFFER_NUMBER) {
        LOGE("buffer number needed are exceeded limitation");
        return VPP_FAIL;
    }
    return VPP_OK;
}

void VPPProcessor::setEOS()
{
    LOGV("setEOS");
    mEOS = true;
}
} /* namespace android */
