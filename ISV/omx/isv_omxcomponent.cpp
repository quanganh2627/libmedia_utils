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


#include <OMX_Component.h>
#include "isv_omxcomponent.h"
#include <media/hardware/HardwareAPI.h>
#include "isv_profile.h"

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "isv-omxil"

using namespace android;

/**********************************************************************************
 * component methods & helpers
 */
#define GET_ISVOMX_COMPONENT(hComponent)                                    \
    ISVComponent *pComp = static_cast<ISVComponent*>                        \
        ((static_cast<OMX_COMPONENTTYPE*>(hComponent))->pComponentPrivate); \
    if (!pComp)                                                             \
        return OMX_ErrorBadParameter;

Vector<ISVComponent*> ISVComponent::g_isv_components;

ISVComponent::ISVComponent(
        OMX_PTR pAppData)
    :   mComponent(NULL),
        mpCallBacks(NULL),
        mCore(NULL),
        mpISVCallBacks(NULL),
        mVPP(NULL),
        mFilters(0),
        mProcThread(NULL),
        mThreadRunning(false),
        mProcThreadObserver(NULL),
        mISVProfile(NULL),
        mNumISVBuffers(MIN_ISV_BUFFER_NUM),
        mNumDecoderBuffers(0),
        mNumDecoderBuffersBak(0),
        mWidth(0),
        mHeight(0),
        mUseAndroidNativeBufferIndex(0),
        mUseAndroidNativeBuffer(false),
        mUseAndroidNativeBuffer2(false),
        mVPPEnabled(false),
        mVPPOn(false),
        mVPPFlushing(false),
        mInitialized(false)
{
    LOGI("%s", __func__);
    memset(&mFilterParam, 0, sizeof(mFilterParam));
    memset(&mBaseComponent, 0, sizeof(OMX_COMPONENTTYPE));
    /* handle initialization */
    SetTypeHeader(&mBaseComponent, sizeof(mBaseComponent));
    mBaseComponent.pApplicationPrivate = pAppData;
    mBaseComponent.pComponentPrivate = static_cast<OMX_PTR>(this);

    /* connect handle's functions */
    mBaseComponent.GetComponentVersion = NULL;
    mBaseComponent.SendCommand = SendCommand;
    mBaseComponent.GetParameter = GetParameter;
    mBaseComponent.SetParameter = SetParameter;
    mBaseComponent.GetConfig = GetConfig;
    mBaseComponent.SetConfig = SetConfig;
    mBaseComponent.GetExtensionIndex = GetExtensionIndex;
    mBaseComponent.GetState = GetState;
    mBaseComponent.ComponentTunnelRequest = NULL;
    mBaseComponent.UseBuffer = UseBuffer;
    mBaseComponent.AllocateBuffer = AllocateBuffer;
    mBaseComponent.FreeBuffer = FreeBuffer;
    mBaseComponent.EmptyThisBuffer = EmptyThisBuffer;
    mBaseComponent.FillThisBuffer = FillThisBuffer;
    mBaseComponent.SetCallbacks = SetCallbacks;
    mBaseComponent.ComponentDeInit = NULL;
    mBaseComponent.UseEGLImage = NULL;
    mBaseComponent.ComponentRoleEnum = ComponentRoleEnum;
    g_isv_components.push_back(static_cast<ISVComponent*>(this));
}

ISVComponent::~ISVComponent()
{
    LOGI("%s", __func__);
    if (mpISVCallBacks) {
        free(mpISVCallBacks);
        mpISVCallBacks = NULL;
    }

    for (OMX_U32 i = 0; i < g_isv_components.size(); i++) {
        if (g_isv_components.itemAt(i) == static_cast<ISVComponent*>(this)) {
            g_isv_components.removeAt(i);
        }
    }

    memset(&mBaseComponent, 0, sizeof(OMX_COMPONENTTYPE));
    memset(&mFilterParam, 0, sizeof(mFilterParam));
    deinit();
}

status_t ISVComponent::init(int32_t width, int32_t height)
{
    if (mInitialized)
        return STATUS_OK;

    bool frcOn = false;
    //FIXME: here we can know video frame width/height first, and then check vpp
    //dynamic on/off as early as possible
    if (mISVProfile == NULL)
        mISVProfile = new ISVProfile(width, height);

    if (mProcThreadObserver == NULL)
        mProcThreadObserver = new ISVProcThreadObserver(&mBaseComponent, mComponent, mpCallBacks);

    mVPPOn = mISVProfile->isFRCOn() || mISVProfile->isVPPOn();

    // get platform VPP cap first
    mFilters = mISVProfile->getFilterStatus();

    // turn off filters if dynamic vpp/frc setting is off
    if (!mISVProfile->isVPPOn())
        mFilters &= FilterFrameRateConversion;

    if (!mISVProfile->isFRCOn())
        mFilters &= ~FilterFrameRateConversion;

    frcOn = mFilters & FilterFrameRateConversion;

    if (mVPP == NULL) {
        mVPP = new VPPWorker();
        if (STATUS_OK != mVPP->init(width, height)) {
            LOGE("%s: mVPP init failed, set mVPPEnabled -->false", __func__);
            mVPPEnabled = false;
            return STATUS_ERROR;
        }
    }

    if (mProcThread == NULL) {
        //FIXME: we don't know mFilterParam.frameRate
        mProcThread = new VPPProcThread(false, mVPP, mProcThreadObserver, frcOn, mFilterParam.frameRate);
        mProcThread->start();
    }

    //FIXME: only for test without enabling VPP setting
    //mVPPOn = true;
    //mFilters |= FilterSharpening;
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: mVPPOn %d, mFilters 0x%08x", __func__, mVPPOn, mFilters);
    mInitialized = true;
    return STATUS_OK;
}

void ISVComponent::deinit()
{
    if (mProcThread != NULL) {
        mProcThread->stop();
        mProcThread = NULL;
    }

    if (mVPP != NULL) {
        delete mVPP;
        mVPP = NULL;
    }

    mProcThreadObserver = NULL;
    mISVProfile = NULL;

    mVPPOn = false;
    mFilters = 0;
    mInitialized = false;
}

OMX_CALLBACKTYPE* ISVComponent::getCallBacks(OMX_CALLBACKTYPE* pCallBacks)
{
    //reset component callback functions
    mpCallBacks = pCallBacks;
    if (mpISVCallBacks) {
        free(mpISVCallBacks);
        mpISVCallBacks = NULL;
    }

    mpISVCallBacks = (OMX_CALLBACKTYPE *)calloc(1, sizeof(OMX_CALLBACKTYPE));
    if (!mpISVCallBacks) {
        LOGE("%s: failed to alloc isv callbacks", __func__);
        return NULL;
    }
    mpISVCallBacks->EventHandler = EventHandler;
    mpISVCallBacks->EmptyBufferDone = pCallBacks->EmptyBufferDone;
    mpISVCallBacks->FillBufferDone = FillBufferDone;
    return mpISVCallBacks;
}

OMX_ERRORTYPE ISVComponent::SendCommand(
    OMX_IN  OMX_HANDLETYPE hComponent,
    OMX_IN  OMX_COMMANDTYPE Cmd,
    OMX_IN  OMX_U32 nParam1,
    OMX_IN  OMX_PTR pCmdData)
{
    GET_ISVOMX_COMPONENT(hComponent);

    return pComp->ISV_SendCommand(Cmd, nParam1, pCmdData);
}

OMX_ERRORTYPE ISVComponent::ISV_SendCommand(
    OMX_IN  OMX_COMMANDTYPE Cmd,
    OMX_IN  OMX_U32 nParam1,
    OMX_IN  OMX_PTR pCmdData)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: Cmd index 0x%08x, nParam1 %d", __func__, Cmd, nParam1);

    if (mVPPEnabled && mVPPOn) {
        if ((Cmd == OMX_CommandFlush && nParam1 == kPortIndexOutput)
                || (Cmd == OMX_CommandStateSet && nParam1 == OMX_StateIdle)
                || (Cmd == OMX_CommandPortDisable && nParam1 == 1)) {
            LOGD_IF(ISV_COMPONENT_DEBUG, "%s: receive flush command, notify vpp thread to flush(Seek begin)", __func__);
            mVPPFlushing = true;
            mProcThread->notifyFlush();
        }
    }

    return OMX_SendCommand(mComponent, Cmd, nParam1, pCmdData);
}

OMX_ERRORTYPE ISVComponent::GetParameter(
    OMX_IN  OMX_HANDLETYPE hComponent,
    OMX_IN  OMX_INDEXTYPE nParamIndex,
    OMX_INOUT OMX_PTR pComponentParameterStructure)
{
    GET_ISVOMX_COMPONENT(hComponent);

    return pComp->ISV_GetParameter(nParamIndex, pComponentParameterStructure);
}

OMX_ERRORTYPE ISVComponent::ISV_GetParameter(
    OMX_IN  OMX_INDEXTYPE nParamIndex,
    OMX_INOUT OMX_PTR pComponentParameterStructure)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: nIndex 0x%08x", __func__, nParamIndex);

    OMX_ERRORTYPE err = OMX_GetParameter(mComponent, nParamIndex, pComponentParameterStructure);

    if (err == OMX_ErrorNone && mVPPEnabled) {
        OMX_PARAM_PORTDEFINITIONTYPE *def =
            static_cast<OMX_PARAM_PORTDEFINITIONTYPE*>(pComponentParameterStructure);

        if (nParamIndex == OMX_IndexParamPortDefinition
                && def->nPortIndex == kPortIndexOutput) {
            LOGD_IF(ISV_COMPONENT_DEBUG, "%s: orignal bufferCountActual %d, bufferCountMin %d",  __func__, def->nBufferCountActual, def->nBufferCountMin);
            def->nBufferCountActual += mNumISVBuffers;
            def->nBufferCountMin += mNumISVBuffers;
        }
    }

    return err;
}

OMX_ERRORTYPE ISVComponent::SetParameter(
    OMX_IN  OMX_HANDLETYPE hComponent,
    OMX_IN  OMX_INDEXTYPE nIndex,
    OMX_IN  OMX_PTR pComponentParameterStructure)
{
    GET_ISVOMX_COMPONENT(hComponent);
 
    return pComp->ISV_SetParameter(nIndex, pComponentParameterStructure);
}

OMX_ERRORTYPE ISVComponent::ISV_SetParameter(
    OMX_IN  OMX_INDEXTYPE nIndex,
    OMX_IN  OMX_PTR pComponentParameterStructure)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: nIndex 0x%08x", __func__, nIndex);

    if (nIndex == static_cast<OMX_INDEXTYPE>(OMX_IndexExtSetISVMode)) {
        ISV_MODE* def = static_cast<ISV_MODE*>(pComponentParameterStructure);

        if (*def == ISV_AUTO) {
            mVPPEnabled = true;
            LOGD_IF(ISV_COMPONENT_DEBUG, "%s: mVPPEnabled -->true", __func__);
        } else if (*def == ISV_DISABLE)
            mVPPEnabled = false;
        return OMX_ErrorNone;
    }

    OMX_ERRORTYPE err = OMX_SetParameter(mComponent, nIndex, pComponentParameterStructure);
    if (err == OMX_ErrorNone && mVPPEnabled) {
        if (nIndex == OMX_IndexParamPortDefinition) {
            OMX_PARAM_PORTDEFINITIONTYPE *def =
                static_cast<OMX_PARAM_PORTDEFINITIONTYPE*>(pComponentParameterStructure);

            if (def->nPortIndex == kPortIndexOutput) {
                //set the buffer count we should fill to decoder before feed buffer to VPP
                mNumDecoderBuffersBak = mNumDecoderBuffers = def->nBufferCountActual - mNumISVBuffers + EXTRA_INPUT_NUM;
                OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def->format.video;
                //FIXME: we don't support scaling yet, so set src region equal to dst region
                mFilterParam.srcWidth = mFilterParam.dstWidth = video_def->nFrameWidth;
                mFilterParam.srcHeight = mFilterParam.dstHeight = video_def->nFrameHeight;

                //FIXME: init itself here
                if (mWidth != mFilterParam.srcWidth
                        || mHeight != mFilterParam.srcHeight) {
                    deinit();
                    if (STATUS_OK == init(mFilterParam.srcWidth, mFilterParam.srcHeight)) {
                        mWidth = mFilterParam.srcWidth;
                        mHeight = mFilterParam.srcHeight;
                    }
                }
                if (mVPP && STATUS_OK != mVPP->setBufferCount(def->nBufferCountActual)) {
                    LOGE("%s: failed to set ISV buffer count, set VPPEnabled -->false", __func__);
                    mVPPEnabled = false;
                }
                LOGD_IF(ISV_COMPONENT_DEBUG, "%s: video frame width %d, height %d",  __func__, 
                        video_def->nFrameWidth, video_def->nFrameHeight);
            }

            if (def->nPortIndex == kPortIndexInput) {
                OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def->format.video;
                mFilterParam.frameRate = video_def->xFramerate;

                if (mISVProfile != NULL && mFilterParam.frameRate != 0) {
                    mFilterParam.frcRate = mISVProfile->getFRCRate(mFilterParam.frameRate);
                }
                LOGD_IF(ISV_COMPONENT_DEBUG, "%s: frame rate is set to %d",  __func__, mFilterParam.frameRate);
            }
        }

        if (mUseAndroidNativeBuffer
                && nIndex == static_cast<OMX_INDEXTYPE>(mUseAndroidNativeBufferIndex)) {
            UseAndroidNativeBufferParams *def =
                static_cast<UseAndroidNativeBufferParams*>(pComponentParameterStructure);

            if (mVPP) {
                if (STATUS_OK != mVPP->useBuffer(def->nativeBuffer)) {
                    LOGE("%s: failed to register graphic buffers to ISV, set mVPPEnabled -->false", __func__);
                    mVPPEnabled = false;
                }
            }
        }
    }
    return err;
}

OMX_ERRORTYPE ISVComponent::GetConfig(
    OMX_IN  OMX_HANDLETYPE hComponent,
    OMX_IN  OMX_INDEXTYPE nIndex,
    OMX_INOUT OMX_PTR pComponentConfigStructure)
{
    GET_ISVOMX_COMPONENT(hComponent);

    return pComp->ISV_GetConfig(nIndex, pComponentConfigStructure);
}

OMX_ERRORTYPE ISVComponent::ISV_GetConfig(
    OMX_IN  OMX_INDEXTYPE nIndex,
    OMX_INOUT OMX_PTR pComponentConfigStructure)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: nIndex 0x%08x", __func__, nIndex);

    return OMX_GetConfig(mComponent, nIndex, pComponentConfigStructure);
}

OMX_ERRORTYPE ISVComponent::SetConfig(
    OMX_IN  OMX_HANDLETYPE hComponent,
    OMX_IN  OMX_INDEXTYPE nIndex,
    OMX_IN  OMX_PTR pComponentConfigStructure)
{
    GET_ISVOMX_COMPONENT(hComponent);

    return pComp->ISV_SetConfig(nIndex, pComponentConfigStructure);
}

OMX_ERRORTYPE ISVComponent::ISV_SetConfig(
    OMX_IN  OMX_INDEXTYPE nIndex,
    OMX_IN  OMX_PTR pComponentConfigStructure)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: nIndex 0x%08x", __func__, nIndex);

    return OMX_SetConfig(mComponent, nIndex, pComponentConfigStructure);
}

OMX_ERRORTYPE ISVComponent::GetExtensionIndex(
    OMX_IN  OMX_HANDLETYPE hComponent,
    OMX_IN  OMX_STRING cParameterName,
    OMX_OUT OMX_INDEXTYPE* pIndexType)
{
    GET_ISVOMX_COMPONENT(hComponent);

    return pComp->ISV_GetExtensionIndex(cParameterName, pIndexType);
}

OMX_ERRORTYPE ISVComponent::ISV_GetExtensionIndex(
    OMX_IN  OMX_STRING cParameterName,
    OMX_OUT OMX_INDEXTYPE* pIndexType)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: cParameterName %s", __func__, cParameterName);
    if(!strncmp(cParameterName, "OMX.intel.index.SetISVMode", strlen(cParameterName))) {
        *pIndexType = static_cast<OMX_INDEXTYPE>(OMX_IndexExtSetISVMode);
        return OMX_ErrorNone;
    }

    OMX_ERRORTYPE err = OMX_GetExtensionIndex(mComponent, cParameterName, pIndexType);

    if(err == OMX_ErrorNone &&
            !strncmp(cParameterName, "OMX.google.android.index.useAndroidNativeBuffer2", strlen(cParameterName)))
        mUseAndroidNativeBuffer2 = true;

    if(err == OMX_ErrorNone &&
            !strncmp(cParameterName, "OMX.google.android.index.useAndroidNativeBuffer", strlen(cParameterName))) {
        mUseAndroidNativeBuffer = true;
        mUseAndroidNativeBufferIndex = static_cast<uint32_t>(*pIndexType);
    }
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: cParameterName %s, nIndex 0x%08x", __func__,
            cParameterName, *pIndexType);
    return err;
}

OMX_ERRORTYPE ISVComponent::GetState(
    OMX_IN  OMX_HANDLETYPE hComponent,
    OMX_OUT OMX_STATETYPE* pState)
{
    GET_ISVOMX_COMPONENT(hComponent);

    return pComp->ISV_GetState(pState);
}

OMX_ERRORTYPE ISVComponent::ISV_GetState(
    OMX_OUT OMX_STATETYPE* pState)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s", __func__);

    return OMX_GetState(mComponent, pState);
}

OMX_ERRORTYPE ISVComponent::UseBuffer(
    OMX_IN OMX_HANDLETYPE hComponent,
    OMX_INOUT OMX_BUFFERHEADERTYPE **ppBufferHdr,
    OMX_IN OMX_U32 nPortIndex,
    OMX_IN OMX_PTR pAppPrivate,
    OMX_IN OMX_U32 nSizeBytes,
    OMX_IN OMX_U8 *pBuffer)
{
    GET_ISVOMX_COMPONENT(hComponent);

    return pComp->ISV_UseBuffer(ppBufferHdr, nPortIndex,
                                 pAppPrivate, nSizeBytes, pBuffer);
}

OMX_ERRORTYPE ISVComponent::ISV_UseBuffer(
    OMX_INOUT OMX_BUFFERHEADERTYPE **ppBufferHdr,
    OMX_IN OMX_U32 nPortIndex,
    OMX_IN OMX_PTR pAppPrivate,
    OMX_IN OMX_U32 nSizeBytes,
    OMX_IN OMX_U8 *pBuffer)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s", __func__);

    OMX_ERRORTYPE err = OMX_UseBuffer(mComponent, ppBufferHdr, nPortIndex,
            pAppPrivate, nSizeBytes, pBuffer);
#ifndef USE_IVP
    if(err == OMX_ErrorNone
            && mVPPEnabled
            && mUseAndroidNativeBuffer2) {
        if (mVPP) {
            if (STATUS_OK != mVPP->useBuffer(reinterpret_cast<buffer_handle_t>(pBuffer))) {
                LOGE("%s: failed to register graphic buffers to ISV, set mVPPEnabled -->false", __func__);
                mVPPEnabled = false;
            } else
                LOGD_IF(ISV_COMPONENT_DEBUG, "%s: mVPP useBuffer success. buffer handle %u", __func__, pBuffer);
        }
    }
#endif
    return err;
}

OMX_ERRORTYPE ISVComponent::AllocateBuffer(
    OMX_IN OMX_HANDLETYPE hComponent,
    OMX_INOUT OMX_BUFFERHEADERTYPE **ppBuffer,
    OMX_IN OMX_U32 nPortIndex,
    OMX_IN OMX_PTR pAppPrivate,
    OMX_IN OMX_U32 nSizeBytes)
{
    GET_ISVOMX_COMPONENT(hComponent);

    return pComp->ISV_AllocateBuffer(ppBuffer, nPortIndex,
                                      pAppPrivate, nSizeBytes);
}

OMX_ERRORTYPE ISVComponent::ISV_AllocateBuffer(
    OMX_INOUT OMX_BUFFERHEADERTYPE **ppBuffer,
    OMX_IN OMX_U32 nPortIndex,
    OMX_IN OMX_PTR pAppPrivate,
    OMX_IN OMX_U32 nSizeBytes)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s", __func__);

    return OMX_AllocateBuffer(mComponent, ppBuffer, nPortIndex,
                                      pAppPrivate, nSizeBytes);
}

OMX_ERRORTYPE ISVComponent::FreeBuffer(
    OMX_IN  OMX_HANDLETYPE hComponent,
    OMX_IN  OMX_U32 nPortIndex,
    OMX_IN  OMX_BUFFERHEADERTYPE *pBuffer)
{
    GET_ISVOMX_COMPONENT(hComponent);

    return pComp->ISV_FreeBuffer(nPortIndex, pBuffer);
}

OMX_ERRORTYPE ISVComponent::ISV_FreeBuffer(
    OMX_IN  OMX_U32 nPortIndex,
    OMX_IN  OMX_BUFFERHEADERTYPE *pBuffer)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: pBuffer %u", __func__, pBuffer);

    OMX_ERRORTYPE err = OMX_FreeBuffer(mComponent, nPortIndex, pBuffer);
    if(err == OMX_ErrorNone && mVPPEnabled) {
        if (mVPP) {
            if (STATUS_OK != mVPP->freeBuffer(reinterpret_cast<buffer_handle_t>(pBuffer->pBuffer)))
                LOGW("%s: buffer handle %u has not been registered into ISV", __func__);
        }
    }
    return err;
}

OMX_ERRORTYPE ISVComponent::EmptyThisBuffer(
    OMX_IN  OMX_HANDLETYPE hComponent,
    OMX_IN  OMX_BUFFERHEADERTYPE* pBuffer)
{
    GET_ISVOMX_COMPONENT(hComponent);

    return pComp->ISV_EmptyThisBuffer(pBuffer);
}

OMX_ERRORTYPE ISVComponent::ISV_EmptyThisBuffer(
    OMX_IN  OMX_BUFFERHEADERTYPE* pBuffer)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: pBuffer %p", __func__, pBuffer);

    return OMX_EmptyThisBuffer(mComponent, pBuffer);
}

OMX_ERRORTYPE ISVComponent::FillThisBuffer(
    OMX_IN  OMX_HANDLETYPE hComponent,
    OMX_IN  OMX_BUFFERHEADERTYPE *pBuffer)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: API entry.", __func__);
    GET_ISVOMX_COMPONENT(hComponent);

    return pComp->ISV_FillThisBuffer(pBuffer);
}

OMX_ERRORTYPE ISVComponent::ISV_FillThisBuffer(
    OMX_IN  OMX_BUFFERHEADERTYPE *pBuffer)
{
    if(!mVPPEnabled || !mVPPOn)
        return OMX_FillThisBuffer(mComponent, pBuffer);

    if (mNumDecoderBuffers > 0) {
        mNumDecoderBuffers--;
        LOGD_IF(ISV_COMPONENT_DEBUG, "%s: fill pBuffer %u to the decoder, decoder still need extra %d buffers", __func__,
                pBuffer, mNumDecoderBuffers);
        return OMX_FillThisBuffer(mComponent, pBuffer);
    }
    mProcThread->addOutput(pBuffer);

    return OMX_ErrorNone;
}

OMX_ERRORTYPE ISVComponent::FillBufferDone(
        OMX_OUT OMX_HANDLETYPE hComponent,
        OMX_OUT OMX_PTR pAppData,
        OMX_OUT OMX_BUFFERHEADERTYPE* pBuffer)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: API entry. ISV component num %d, component handle %p on index 0", __func__,
            g_isv_components.size(),
            g_isv_components.itemAt(0));
    for (OMX_U32 i = 0; i < g_isv_components.size(); i++) {
        if (static_cast<OMX_HANDLETYPE>(g_isv_components.itemAt(i)->mComponent) == hComponent)
            return g_isv_components.itemAt(i)->ISV_FillBufferDone(hComponent, pAppData, pBuffer);
    }
    return OMX_ErrorUndefined;
}

OMX_ERRORTYPE ISVComponent::ISV_FillBufferDone(
        OMX_OUT OMX_HANDLETYPE hComponent,
        OMX_OUT OMX_PTR pAppData,
        OMX_OUT OMX_BUFFERHEADERTYPE* pBuffer)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: %p <== buffer_handle_t %p. mVPPEnabled %d, mVPPOn %d", __func__,
            pBuffer, pBuffer->pBuffer, mVPPEnabled, mVPPOn);
    if (!mpCallBacks) {
        LOGE("%s: no call back functions were registered.", __func__);
        return OMX_ErrorUndefined;
    }

    if(!mVPPEnabled || !mVPPOn || mVPPFlushing)
        return mpCallBacks->FillBufferDone(&mBaseComponent, pAppData, pBuffer);

    if (STATUS_OK != mVPP->configFilters(&mFilters, &mFilterParam, pBuffer->nFlags)) {
        LOGE("%s: failed to configFilters, set mVPPEnabled -->false");
        mVPPEnabled = false;
    }

    mProcThread->addInput(pBuffer);

    return OMX_ErrorNone;
}

OMX_ERRORTYPE ISVComponent::EventHandler(
        OMX_IN OMX_HANDLETYPE hComponent,
        OMX_IN OMX_PTR pAppData,
        OMX_IN OMX_EVENTTYPE eEvent,
        OMX_IN OMX_U32 nData1,
        OMX_IN OMX_U32 nData2,
        OMX_IN OMX_PTR pEventData)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s: API entry. ISV component num %d, component handle %p on index 0", __func__,
            g_isv_components.size(),
            g_isv_components.itemAt(0));
    for (OMX_U32 i = 0; i < g_isv_components.size(); i++) {
        if (static_cast<OMX_HANDLETYPE>(g_isv_components.itemAt(i)->mComponent) == hComponent)
            return g_isv_components.itemAt(i)->ISV_EventHandler(hComponent, pAppData, eEvent, nData1, nData2, pEventData);
    }
    return OMX_ErrorUndefined;
}

OMX_ERRORTYPE ISVComponent::ISV_EventHandler(
        OMX_IN OMX_HANDLETYPE hComponent,
        OMX_IN OMX_PTR pAppData,
        OMX_IN OMX_EVENTTYPE eEvent,
        OMX_IN OMX_U32 nData1,
        OMX_IN OMX_U32 nData2,
        OMX_IN OMX_PTR pEventData)
{
    if (!mpCallBacks) {
        LOGE("%s: no call back functions were registered.", __func__);
        return OMX_ErrorUndefined;
    }

    if(!mVPPEnabled || !mVPPOn)
        return mpCallBacks->EventHandler(&mBaseComponent, pAppData, eEvent, nData1, nData2, pEventData);

    switch (eEvent) {
        case OMX_EventCmdComplete:
        {
            LOGD_IF(ISV_COMPONENT_DEBUG, "%s: OMX_EventCmdComplete Cmd type 0x%08x, data2 %d", __func__,
                    nData1, nData2);
            if (((OMX_COMMANDTYPE)nData1 == OMX_CommandFlush && nData2 == kPortIndexOutput)
                || ((OMX_COMMANDTYPE)nData1 == OMX_CommandStateSet && nData2 == OMX_StateIdle)
                || ((OMX_COMMANDTYPE)nData1 == OMX_CommandPortDisable && nData2 == 1)) {
                mProcThread->waitFlushFinished();
                mVPPFlushing = false;
                mNumDecoderBuffers = mNumDecoderBuffersBak;
            }
            break;
        }

        case OMX_EventError:
        {
            //do we need do anything here?
            LOGE("%s: ERROR(0x%08x, %d)", __func__, nData1, nData2);
            //mProcThread->flush();
            break;
        }

        case OMX_EventPortSettingsChanged:
        {
            //FIXME: do we need clear ISV buffer queues for this situation?
            //mProcThread->notifyFlush();
            break;
        }

        default:
        {
            LOGD_IF(ISV_COMPONENT_DEBUG, "%s: EVENT(%d, %ld, %ld)", __func__, eEvent, nData1, nData2);
            break;
        }
    }
    return mpCallBacks->EventHandler(&mBaseComponent, pAppData, eEvent, nData1, nData2, pEventData);
}

OMX_ERRORTYPE ISVComponent::SetCallbacks(
    OMX_IN  OMX_HANDLETYPE hComponent,
    OMX_IN  OMX_CALLBACKTYPE* pCallbacks,
    OMX_IN  OMX_PTR pAppData)
{
    GET_ISVOMX_COMPONENT(hComponent);

    return pComp->ISV_SetCallbacks(pCallbacks, pAppData);
}

OMX_ERRORTYPE ISVComponent::ISV_SetCallbacks(
    OMX_IN  OMX_CALLBACKTYPE* pCallbacks,
    OMX_IN  OMX_PTR pAppData)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s", __func__);

    if (mVPPEnabled) {
        if (mpISVCallBacks)
            free(mpISVCallBacks);
        mpISVCallBacks->EventHandler = EventHandler;
        mpISVCallBacks->EmptyBufferDone = pCallbacks->EmptyBufferDone;
        mpISVCallBacks->FillBufferDone = FillBufferDone;
        mpCallBacks = pCallbacks;
    }
    return mComponent->SetCallbacks(mComponent, mpISVCallBacks, pAppData);
}

OMX_ERRORTYPE ISVComponent::ComponentRoleEnum(
    OMX_IN OMX_HANDLETYPE hComponent,
    OMX_OUT OMX_U8 *cRole,
    OMX_IN OMX_U32 nIndex)
{
    GET_ISVOMX_COMPONENT(hComponent);

    return pComp->ISV_ComponentRoleEnum(cRole, nIndex);
}

OMX_ERRORTYPE ISVComponent::ISV_ComponentRoleEnum(
    OMX_OUT OMX_U8 *cRole,
    OMX_IN OMX_U32 nIndex)
{
    LOGD_IF(ISV_COMPONENT_DEBUG, "%s", __func__);

    return mComponent->ComponentRoleEnum(mComponent, cRole, nIndex);
}


void ISVComponent::SetTypeHeader(OMX_PTR type, OMX_U32 size)
{
    OMX_U32 *nsize;
    OMX_VERSIONTYPE *nversion;

    if (!type)
        return;

    nsize = (OMX_U32 *)type;
    nversion = (OMX_VERSIONTYPE *)((OMX_U8 *)type + sizeof(OMX_U32));

    *nsize = size;
    nversion->nVersion = OMX_SPEC_VERSION;
}


ISVProcThreadObserver::ISVProcThreadObserver(
        OMX_COMPONENTTYPE *pBaseComponent,
        OMX_COMPONENTTYPE *pComponent,
        OMX_CALLBACKTYPE *pCallBacks)
    :   mBaseComponent(pBaseComponent),
        mComponent(pComponent),
        mpCallBacks(pCallBacks)
{
    ALOGV("VPPProcThreadObserver!");
}

ISVProcThreadObserver::~ISVProcThreadObserver()
{
    ALOGV("~VPPProcThreadObserver!");
    mBaseComponent = NULL;
    mComponent = NULL;
    mpCallBacks = NULL;
}

OMX_ERRORTYPE ISVProcThreadObserver::releaseBuffer(PORT_INDEX index, OMX_BUFFERHEADERTYPE* pBuffer, bool bFLush)
{
    if (!mBaseComponent || !mComponent || !mpCallBacks)
        return OMX_ErrorUndefined;

    OMX_ERRORTYPE err = OMX_ErrorNone;
    if (bFLush) {
        pBuffer->nFilledLen = 0;
        pBuffer->nOffset = 0;
        OMX_ERRORTYPE err = mpCallBacks->FillBufferDone(mBaseComponent, mBaseComponent->pApplicationPrivate, pBuffer);
        LOGD_IF(ISV_COMPONENT_DEBUG, "%s: flush pBuffer %u", __func__, pBuffer);
        return err;
    }

    if (index == kPortIndexInput) {
        pBuffer->nFilledLen = 0;
        pBuffer->nOffset = 0;
        pBuffer->nFlags = 0;
        err = OMX_FillThisBuffer(mComponent, pBuffer);
        LOGD_IF(ISV_COMPONENT_DEBUG, "%s: FillBuffer pBuffer %u", __func__, pBuffer);
    } else {
        err = mpCallBacks->FillBufferDone(mBaseComponent, mBaseComponent->pApplicationPrivate, pBuffer);
        LOGD_IF(ISV_COMPONENT_DEBUG, "%s: FillBufferDone pBuffer %u, timeStamp %.2f ms", __func__, pBuffer, pBuffer->nTimeStamp/1E3);
    }

    return err;
}
