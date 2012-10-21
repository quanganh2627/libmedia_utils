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
#include "VPPWorker.h"

#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/foundation/ADebug.h>
#include <utils/Log.h>

#define LOG_TAG "VPPWorker"

#define FRCX 2 //TODO: used for prototype

namespace android {

VPPWorker::VPPWorker() {}
VPPWorker::~VPPWorker() {}

#if 0
status_t VPPWorker::process(sp<ANativeWindow> &native, MediaBuffer *input, MediaBuffer *output) {
    if (input == NULL || output == NULL)
        return -1;

    void *inputData[3] = {0, 0, 0};
    void *outputData[3] = {0, 0, 0};
    GraphicBufferMapper &mapper = GraphicBufferMapper::get();

    GraphicBuffer* inputGraphicBuffer = input->graphicBuffer().get();
    int width = inputGraphicBuffer->getWidth();
    int height = inputGraphicBuffer->getHeight();

    Rect inputBounds = inputGraphicBuffer->getBounds();
    ANativeWindowBuffer * inputBuf = inputGraphicBuffer->getNativeBuffer();

    CHECK_EQ(0, native->lockBuffer(native.get(), inputBuf));
    CHECK_EQ(0, mapper.lock(inputBuf->handle, 0x3/*USAGE_SW_READ_OFTEN*/, inputBounds, inputData));
    CHECK(inputData[0] != NULL);

    GraphicBuffer* outputGraphicBuffer = output->graphicBuffer().get();
    Rect outputBounds = outputGraphicBuffer->getBounds();
    ANativeWindowBuffer * outputBuf = outputGraphicBuffer->getNativeBuffer();

    CHECK_EQ(0, native->lockBuffer(native.get(), outputBuf));
    CHECK_EQ(0, mapper.lock(outputBuf->handle, 0x30/*USAGE_SW_WRITE_OFTEN*/, outputBounds, outputData));
    CHECK(outputData[0] != NULL);

    memcpy(outputData[0], inputData[0], width * height * 1.5);

    CHECK_EQ(0, mapper.unlock(inputBuf->handle));
    CHECK_EQ(0, mapper.unlock(outputBuf->handle));

    int64_t timeUs;
    CHECK(input->meta_data()->findInt64(kKeyTime, &timeUs));
    output->meta_data()->setInt64(kKeyTime, timeUs);

    return OK;
}
#endif

status_t VPPWorker::process(sp<ANativeWindow> &native, MediaBuffer *input, Vector<MediaBuffer *> &output, uint32_t* outputCount, bool isRestart/*TODO: isRestart only used for prototype*/) {
    if (input == NULL || output.size() == 0)
        return -1;

    void *inputData[3] = {0, 0, 0};
    GraphicBufferMapper &mapper = GraphicBufferMapper::get();

    GraphicBuffer* inputGraphicBuffer = input->graphicBuffer().get();
    int width = inputGraphicBuffer->getStride();
    int height = inputGraphicBuffer->getHeight();

    Rect inputBounds = inputGraphicBuffer->getBounds();
    ANativeWindowBuffer * inputBuf = inputGraphicBuffer->getNativeBuffer();

    CHECK_EQ(0, native->lockBuffer(native.get(), inputBuf));
    CHECK_EQ(0, mapper.lock(inputBuf->handle, 0x3/*USAGE_SW_READ_OFTEN*/, inputBounds, inputData));
    CHECK(inputData[0] != NULL);

    if (isRestart) {
        *outputCount = 1;

        void *outputData[3] = {0, 0, 0};
        GraphicBuffer* outputGraphicBuffer = output[0]->graphicBuffer().get();
        Rect outputBounds = outputGraphicBuffer->getBounds();
        ANativeWindowBuffer * outputBuf = outputGraphicBuffer->getNativeBuffer();

        CHECK_EQ(0, native->lockBuffer(native.get(), outputBuf));
        CHECK_EQ(0, mapper.lock(outputBuf->handle, 0x30/*USAGE_SW_WRITE_OFTEN*/, outputBounds, outputData));
        CHECK(outputData[0] != NULL);

        memcpy(outputData[0], inputData[0], width * height * 1.5);

        CHECK_EQ(0, mapper.unlock(inputBuf->handle));
        CHECK_EQ(0, mapper.unlock(outputBuf->handle));

        int64_t timeUs;
        CHECK(input->meta_data()->findInt64(kKeyTime, &timeUs));
        output[0]->meta_data()->setInt64(kKeyTime, timeUs);


    } else {
        int64_t timeUs;
        CHECK(input->meta_data()->findInt64(kKeyTime, &timeUs));
        *outputCount = FRCX;

        void *outputData[2][3] = {0, 0, 0, 0, 0, 0};
        for (int i = 0; i < FRCX; i++) {
            GraphicBuffer* outputGraphicBuffer = output[i]->graphicBuffer().get();
            Rect outputBounds = outputGraphicBuffer->getBounds();
            ANativeWindowBuffer * outputBuf = outputGraphicBuffer->getNativeBuffer();

            CHECK_EQ(0, native->lockBuffer(native.get(), outputBuf));
            CHECK_EQ(0, mapper.lock(outputBuf->handle, 0x30/*USAGE_SW_WRITE_OFTEN*/, outputBounds, outputData[i]));
            CHECK(outputData[i][0] != NULL);

            memcpy(outputData[i][0], inputData[0], width * height * 1.5);

            CHECK_EQ(0, mapper.unlock(outputBuf->handle));
            output[i]->meta_data()->setInt64(kKeyTime, timeUs - 1000000ll * (FRCX - i - 1) / 60); //TODO: suppose output fps = 60
        }
        CHECK_EQ(0, mapper.unlock(inputBuf->handle));
    }
    return OK;

}

};
