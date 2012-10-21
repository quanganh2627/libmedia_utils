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

#ifndef __VPP_WORKER_H
#define __VPP_WORKER_H

#include <media/stagefright/MediaBuffer.h>
#include <utils/Errors.h>
#include <utils/Vector.h>
#include <android/native_window.h>

namespace android {

class VPPWorker {
public:
    VPPWorker();
    ~VPPWorker();
//    status_t process(sp<ANativeWindow> &native, MediaBuffer *input, MediaBuffer *output);
    status_t process(sp<ANativeWindow> &native, MediaBuffer *input, Vector<MediaBuffer *> &output,
                     uint32_t* outputCount, bool isRestart /* TODO: isRestart only used for prototype */);
};

} /* namespace android */

#endif /* __VPP_WORKER_H */

