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

#ifndef __VPP_THREAD_H
#define __VPP_THREAD_H

#include "VPPProcessor.h"
#include "VPPWorker.h"

#include <utils/threads.h>
#include <utils/Errors.h>

namespace android {

class VPPProcessor;

class VPPThread : public Thread {
    public:

        VPPThread(bool canCallJava, VPPProcessor* vppProcessor);
        virtual ~VPPThread();

        virtual status_t readyToRun();

        // Derived class must implement threadLoop(). The thread starts its life
        // here. There are two ways of using the Thread object:
        // 1) loop: if threadLoop() returns true, it will be called again if
        //          requestExit() wasn't called.
        // 2) once: if threadLoop() returns false, the thread will exit upon return.
        virtual bool threadLoop();
        bool isCurrentThread() const;

    public:
        Mutex mLock;
        // main thread send this condition to VPPThread,
        // VPPThread wait this condition to run
        Condition mRunCond;
        // VPPThread send this condition to main thread,
        // so that main thread is able to reset all buffers.
        Condition mResetCond;
        bool mQuit;
        bool mSeek;

    private:
        android_thread_id_t mThreadId;
        VPPProcessor *mVPPProcessor;
        VPPWorker *mWorker;

};

} /* END namespace android */
#endif /* __VPP_THREAD_H*/
