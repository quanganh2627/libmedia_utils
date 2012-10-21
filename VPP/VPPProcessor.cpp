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

VPPProcessor::VPPProcessor()
        :mInputCount(0), mOutputCount(0),
         mInputVppPos(0), mOutputVppPos(0),
         mOutputAddToRenderPos(0),
         mLastRenderTime(-1),
         mNativeWindow(NULL),
         mBufferInfos(NULL) {

    initBuffers();

    mThread = new VPPThread(false, this);
    mThread->run("VPPThread", ANDROID_PRIORITY_NORMAL);
}

VPPProcessor::~VPPProcessor() {
    LOGV("DELETE VPPProcessor-");
    reset();
}

//static
bool VPPProcessor::getVppStatus() {
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

    if(strstr(buf, "true") == NULL) {
        fclose(handle);
        return false;
    }

    fclose(handle);
    return true;
}

status_t VPPProcessor::setBufferInfoFromDecoder(OMXCodec *codec) {
    if (codec == NULL)
        return VPP_FAIL;

    if (mBufferInfos == NULL) {
        mBufferInfos = &codec->mPortBuffers[codec->kPortIndexOutput];
    }
    return VPP_OK;
}

bool VPPProcessor::canSetDecoderBufferToVPP() {
    //invoke VPPThread as many as possible
    {
        Mutex::Autolock autoLock(mThread->mLock);
        mThread->mRunCond.signal();
    }

    //put VPP output which still in output array to RenderList
    while (mOutputBufferStatus[mOutputAddToRenderPos] == VPP_BUFFER_READY_FOR_USE) {
        CHECK(addOutputBufferToRenderList(mOutputAddToRenderPos) == VPP_OK);
        mOutputAddToRenderPos = (mOutputAddToRenderPos + 1) % OUTPUT_BUFFER_COUNT;
    }

    uint32_t index = mInputCount % INPUT_BUFFER_COUNT;
    if (mInputBuffer[index] == NULL)
        return true;
    return false;
}

status_t VPPProcessor::setDecoderBufferToVPP(MediaBuffer *buff) {
    if (buff != NULL) {
        //put buff in inputBuffers when there is empty buffer
        uint32_t index = mInputCount % INPUT_BUFFER_COUNT;
        if ((mInputBuffer[index] == NULL) &&
                (mInputBuffer[(index + INPUT_BUFFER_COUNT - 1) % INPUT_BUFFER_COUNT] != buff)) {

            buff->add_ref();
            mInputBuffer[index] = buff;
            mInputCount ++;
            mRenderList.push_back(buff);
            return VPP_OK;

        }
    }
    return VPP_FAIL;
}

void VPPProcessor::printBuffers() {
    for (uint32_t i = 0; i < INPUT_BUFFER_COUNT; i++) {
        LOGV("input %d.   %p,  time = %lld", i, mInputBuffer[i], mInputBuffer[i] ? getBufferTimestamp(mInputBuffer[i]) : 0);
    }
    for (uint32_t i = 0; i < OUTPUT_BUFFER_COUNT; i++) {
        LOGV("output %d.   %p,  status = %d, time = %lld", i, mOutputBuffer[i], mOutputBufferStatus[i],
                (mOutputBuffer[i] && (mOutputBufferStatus[i] != VPP_BUFFER_FREE)) ? getBufferTimestamp(mOutputBuffer[i]) : 0);
    }

}

void VPPProcessor::printRenderList() {
    List<MediaBuffer*>::iterator it;
    for (it = mRenderList.begin(); it != mRenderList.end(); it++) {
        LOGV("renderList: %p, timestamp = %lld", *it, (*it) ? getBufferTimestamp(*it) : 0);
    }
}

status_t VPPProcessor::read(MediaBuffer **buffer) {
    printBuffers();
    printRenderList();
    if (mRenderList.empty()) {
        LOGV("GOT END OF STREAM!!!");
        *buffer = NULL;

        // play to the end so stop thread
        Mutex::Autolock autoLock(mThread->mLock);
        mThread->mQuit = true;
        mThread->mRunCond.signal();

        return ERROR_END_OF_STREAM;
    }

    *buffer = *(mRenderList.begin());
    mLastRenderTime = getBufferTimestamp(*buffer);
    if (mLastRenderTime == -1)
        return VPP_FAIL;
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
    LOGV("entering seek");
    /* invoke thread if it is waiting */
    {
        Mutex::Autolock autoLock(mThread->mLock);
        mThread->mSeek = true;
        mThread->mRunCond.signal();
        LOGV("VPPProcessor::waiting.........................");
        if (mThread->mResetCond.waitRelative(mThread->mLock, 100000000ll) == TIMED_OUT) {
            //Retry. If we don't get feedback from VPPThread, send signal again and wait.
            mThread->mRunCond.signal();
            mThread->mResetCond.wait(mThread->mLock);
        }

    }

    LOGV("reset after seek");
    // got resetCond means vppthread is not running, main thread is able to reset all buffers.
    resetBuffers();

    mThread->mSeek = false;
    mThread->mRunCond.signal();
}


void VPPProcessor::reset() {
    LOGV("entering reset mQuit = %d\n", mThread->mQuit);
    /* invoke thread if it is waiting */
    {
        Mutex::Autolock autoLock(mThread->mLock);
        if (!mThread->mQuit) {

            mThread->mQuit = true;
            mThread->mRunCond.signal();
            LOGV("VPPProcessor::waiting.....................");
            if (mThread->mResetCond.waitRelative(mThread->mLock, 100000000ll) == TIMED_OUT) {
                //Retry. If we don't get feedback from VPPThread, send signal again and wait.
                mThread->mRunCond.signal();
                mThread->mResetCond.wait(mThread->mLock);
            }
        }
    }

    LOGV("clear all buffers");
    // got resetCond means vppthread is not running, main thread is able to reset all buffers.
    resetBuffers();
    return;
}

void VPPProcessor::resetBuffers() {
    for (uint32_t i = 0; i < INPUT_BUFFER_COUNT; i++) {
        if (mInputBuffer[i] != NULL) {
            while (mInputBuffer[i]->refcount() > 0)
                mInputBuffer[i]->release();
            mInputBuffer[i] = NULL;
        }
    }

    for (uint32_t i = 0; i < OUTPUT_BUFFER_COUNT; i++) {
        if (mOutputBufferStatus[i] != VPP_BUFFER_FREE) {
            //while (mOutputBuffer[i]->refcount() > 0)
                mOutputBuffer[i]->release();
            //mOutputBuffer[i] = NULL;
            mOutputBufferStatus[i] = VPP_BUFFER_FREE;
        }
    }

    mInputCount = 0;
    mOutputCount = 0;
    mInputVppPos = 0;
    mOutputVppPos = 0;
    mOutputAddToRenderPos = 0;

    mRenderList.clear();
}

void VPPProcessor::initBuffers() {
    //init input buffers
    for (uint32_t i = 0; i < INPUT_BUFFER_COUNT; i++) {
        mInputBuffer[i] = NULL;
    }

    //init vpp buffers
    for (uint32_t i = 0; i < OUTPUT_BUFFER_COUNT; i++) {
        mOutputBuffer[i] = NULL;
        mOutputBufferStatus[i] = VPP_BUFFER_FREE;
    }
}

status_t VPPProcessor::releaseBuffer(MediaBuffer * buff) {
    if (buff == NULL)
        return VPP_FAIL;

    for (uint32_t i = 0; i < INPUT_BUFFER_COUNT; i++) {
        if (buff == mInputBuffer[i]) {
            mInputBuffer[i] = NULL;
            break;
        }
    }
    while (buff->refcount() > 0) {
        buff->release();
    }
    buff = NULL;
    return OK;
}

status_t VPPProcessor::addOutputBufferToRenderList(uint32_t pos) {
    CHECK(pos >= 0 && pos < OUTPUT_BUFFER_COUNT);
    MediaBuffer* buff = mOutputBuffer[pos];
    if (buff == NULL) return VPP_FAIL;

    List<MediaBuffer*>::iterator it;
    int64_t timeBuffer = getBufferTimestamp(buff);
    if (timeBuffer == -1)
        return VPP_FAIL;

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
    if ((mLastRenderTime >= timeBuffer) || (it == mRenderList.begin() && timeBuffer < timeRenderList)) {
        LOGV("1. vpp output comes too late, drop it, timeBuffer = %lld\n", timeBuffer);
        //vpp output comes too late, drop it
        if (releaseBuffer(buff) != VPP_OK)
            return VPP_FAIL;
    } else if (timeBuffer == timeRenderList) {
        LOGV("2. timeBuffer = %lld, timeRenderList = %lld, going to erase %p, insert %p\n", timeBuffer, timeRenderList, *it, buff);
        //same timestamp, use vpp output to replace the input
        List<MediaBuffer*>::iterator erase = mRenderList.erase(it);
        mRenderList.insert(erase, buff);
        mOutputBufferStatus[pos] = VPP_BUFFER_USED;
    } else if (timeBuffer < timeRenderList) {
        LOGV("3. timeBuffer = %lld, timeRenderList = %lld\n", timeBuffer, timeRenderList);
        //x.5 frame, just insert it
        mRenderList.insert(it, buff);
        mOutputBufferStatus[pos] = VPP_BUFFER_USED;
    }

    for (uint32_t i = 0; i < INPUT_BUFFER_COUNT; i++) {
        if (mInputBuffer[i] != NULL) {
            int64_t timeInputBuffer = getBufferTimestamp(mInputBuffer[i]);
            if (timeInputBuffer == -1)
                return VPP_FAIL;

            if (timeBuffer >= timeInputBuffer && timeInputBuffer < mLastRenderTime) {
                if (releaseBuffer(mInputBuffer[i]) != VPP_OK)
                    return VPP_FAIL;
            }
        }
    }

    return VPP_OK;

}

status_t VPPProcessor::setNativeWindow(const sp<ANativeWindow> &native) {
    if (mNativeWindow != NULL)
        return VPP_OK;

    mNativeWindow = native;
    status_t err = dequeueOutputBuffersFromNativeWindow();
    return err;
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
    int err = mNativeWindow->cancelBuffer(mNativeWindow.get(), buff->graphicBuffer().get());
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
    ANativeWindowBuffer *buff;
    int err = mNativeWindow->dequeueBuffer(mNativeWindow.get(), &buff);
    if (err != 0) {
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
    return info->mMediaBuffer;
}

status_t VPPProcessor::dequeueOutputBuffersFromNativeWindow() {
    MediaBuffer *buf = NULL;
    for (uint32_t i = 0; i < OUTPUT_BUFFER_COUNT; i++) {
        buf = dequeueBufferFromNativeWindow();
        if (buf == NULL)
            return VPP_FAIL;

        mOutputBuffer[i] = buf;
        mOutputBufferStatus[i] = VPP_BUFFER_FREE;
        LOGV("mOutputBuffer = %p", mOutputBuffer[i]);
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
            CHECK(err == VPP_OK);
        }

        metaData->setInt32(kKeyRendered, 0);
        buff->setObserver(NULL);
        info->mStatus = OMXCodec::OWNED_BY_NATIVE_WINDOW;

        MediaBuffer * mediaBuffer = dequeueBufferFromNativeWindow();
        CHECK(mediaBuffer != NULL);

        for (uint32_t i = 0; i < OUTPUT_BUFFER_COUNT; i++) {
            if (buff == mOutputBuffer[i]) {
                mOutputBuffer[i] = mediaBuffer;
                mOutputBufferStatus[i] = VPP_BUFFER_FREE;
                break;
            }
        }
    } else if (info->mStatus == OMXCodec::OWNED_BY_VPP) {
        if (mThread->mQuit) {
            status_t err = cancelBufferToNativeWindow(buff);
            CHECK(err == VPP_OK);

            buff->setObserver(NULL);
            info->mStatus = OMXCodec::OWNED_BY_NATIVE_WINDOW;

            for (uint32_t i = 0; i < OUTPUT_BUFFER_COUNT; i++) {
                if (buff == mOutputBuffer[i]) {
                    mOutputBuffer[i] = NULL;
                    mOutputBufferStatus[i] = VPP_BUFFER_FREE;
                    break;
                }
            }
        } else if (!mThread->mQuit && !mThread->mSeek) {
            status_t err = cancelBufferToNativeWindow(buff);
            CHECK(err == VPP_OK);

            buff->setObserver(NULL);
            info->mStatus = OMXCodec::OWNED_BY_NATIVE_WINDOW;

            MediaBuffer * mediaBuffer = dequeueBufferFromNativeWindow();
            CHECK(mediaBuffer != NULL);

            for (uint32_t i = 0; i < OUTPUT_BUFFER_COUNT; i++) {
                if (buff == mOutputBuffer[i]) {
                    mOutputBuffer[i] = mediaBuffer;
                    mOutputBufferStatus[i] = VPP_BUFFER_FREE;
                    break;
                }
            }
        }
    }

    return;
}

} /* namespace android */
