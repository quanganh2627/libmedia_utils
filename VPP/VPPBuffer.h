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

#ifndef __VPP_BUFFER_H
#define __VPP_BUFFER_H

#include <ui/GraphicBuffer.h>
#include <stdint.h>
#include <media/stagefright/foundation/AMessage.h>

namespace android {

static const char VPP_STATUS_STORAGE[] = "/data/data/com.intel.vpp/shared_prefs/vpp_settings.xml";

enum VPPStatus {
    VPP_OK = 0,
    VPP_BUFFER_NOT_READY,
    VPP_FAIL = -1
};

// Input buffer transition:
// FREE->LOADED->PROCESSING->READY->FREE
// Output buffer transition:
// FREE->PROCESSING->READY->RENDERING->FREE
//                       |_>FREE
enum VPPBufferStatus {
    VPP_BUFFER_FREE = 0,        //free, not being used
    VPP_BUFFER_PROCESSING,      //sent to VSP driver for process
    VPP_BUFFER_READY,           //VSP process done, ready to use
    VPP_BUFFER_LOADED,          //input only, decoded buffer loaded
    VPP_BUFFER_RENDERING        //output only, vpp buffer in RenderList
};

struct VPPVideoInfo {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
};

/*
 * VPPBuffer is an adaptor for buffers from AwesomePlayer and NuPlayer
 * and used by VPP internally
 */
class VPPBuffer {
public:
    static const int MAX_VPP_BUFFER_NUMBER = 32;

    VPPBuffer(){}
    ~VPPBuffer(){}

    static bool isVppOn()
    {
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

public:
    sp<GraphicBuffer> mGraphicBuffer;
    VPPBufferStatus mStatus;
    int64_t mTimeUs;
    uint32_t mFlags;
    sp<AMessage> mCodecMsg;  // only used by NuPlayerVPPProcessor

private:
    VPPBuffer(const VPPBuffer &);
    VPPBuffer &operator=(const VPPBuffer &);

};

} /* namespace android */

#endif /* __VPP_BUFFER_H */
