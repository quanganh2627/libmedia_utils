/*
 * Copyright (C) 2011 The Android Open Source Project
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

/*
 ******************************************************************************
 * M4OSA_ERR M4MCS_intApplyVPP_NV12(M4VPP_Context pContext, M4VIFI_ImagePlane* pPlaneIn,
 *                               M4VIFI_ImagePlane* pPlaneOut)
 * @brief    Do the video rendering and the resize (if needed)
 * @note    It is called by the video encoder
 * @param    pContext    (IN) VPP context, which actually is the MCS internal context in our case
 * @param    pPlaneIn    (IN) Contains the image
 * @param    pPlaneOut    (IN/OUT) Pointer to an array of 2 planes that will contain the output
 *                                  NV12 image
 * @return    M4NO_ERROR:    No error
 * @return    M4MCS_ERR_VIDEO_DECODE_ERROR: the video decoding failed
 * @return    M4MCS_ERR_RESIZE_ERROR: the resizing failed
 * @return    Any error returned by an underlaying module
 ******************************************************************************
 */


/**
 ********************************************************************
 * Includes
 ********************************************************************
 */
/* OSAL headers */
#include "M4OSA_Memory.h"       /* OSAL memory management */
#include "M4OSA_Debug.h"        /* OSAL debug management */


/* Core headers */
#include "M4MCS_InternalTypes.h"
#include "M4MCS_ErrorCodes.h"

/**
 * Video preprocessing interface definition */
#include "M4VPP_API.h"

/**
 * Video filters */
#include "M4VIFI_FiltersAPI.h" /**< for M4VIFI_ResizeBilinearYUV420toYUV420() */
#include "M4AIR_API_NV12.h"
#include "VideoEditorToolsNV12.h"

#define UV_PLANE_BORDER_VALUE   0x80

M4OSA_ERR M4VSS3GPP_intRotateVideo_NV12(M4VIFI_ImagePlane* pPlaneIn,
    M4OSA_UInt32 rotationDegree);

M4OSA_ERR M4VSS3GPP_intSetNV12Plane(M4VIFI_ImagePlane* planeIn,
    M4OSA_UInt32 width, M4OSA_UInt32 height);

M4OSA_ERR M4MCS_intApplyVPP_NV12(M4VPP_Context pContext,
    M4VIFI_ImagePlane* pPlaneIn, M4VIFI_ImagePlane* pPlaneOut)
{
    M4OSA_ERR        err = M4NO_ERROR;
    M4OSA_UInt32     yuvFrameWidth = 0, yuvFrameHeight = 0;

/* This part is used only if video codecs are compiled*/
#ifndef M4MCS_AUDIOONLY
    /**
     * The VPP context is actually the MCS context! */
    M4MCS_InternalContext *pC = (M4MCS_InternalContext*)(pContext);

    M4_MediaTime mtCts = pC->dViDecCurrentCts;

    /**
     * When Closing after an error occured, it may happen that pReaderVideoAU->m_dataAddress has
     * not been allocated yet. When closing in pause mode, the decoder can be null.
     * We don't want an error to be returned because it would interrupt the close process and
     * thus some resources would be locked. So we return M4NO_ERROR.
     */
    /* Initialize to black plane the output plane if the media rendering
     is black borders */
    if(pC->MediaRendering == M4MCS_kBlackBorders)
    {
        memset((void *)pPlaneOut[0].pac_data,Y_PLANE_BORDER_VALUE,
            (pPlaneOut[0].u_height*pPlaneOut[0].u_stride));
        memset((void *)pPlaneOut[1].pac_data,UV_PLANE_BORDER_VALUE,
            (pPlaneOut[1].u_height*pPlaneOut[1].u_stride));
    }
    else if ((M4OSA_NULL == pC->ReaderVideoAU.m_dataAddress) ||
             (M4OSA_NULL == pC->pViDecCtxt))
    {
        /**
         * We must fill the input of the encoder with a dummy image, because
         * encoding noise leads to a huge video AU, and thus a writer buffer overflow. */
        memset((void *)pPlaneOut[0].pac_data,0,
             pPlaneOut[0].u_stride * pPlaneOut[0].u_height);
        memset((void *)pPlaneOut[1].pac_data,0,
             pPlaneOut[1].u_stride * pPlaneOut[1].u_height);

        M4OSA_TRACE1_0("M4MCS_intApplyVPP_NV12: pReaderVideoAU->m_dataAddress is M4OSA_NULL,\
                       returning M4NO_ERROR");
        return M4NO_ERROR;
    }
    if(pC->isRenderDup == M4OSA_FALSE)
    {
        /**
         *    m_pPreResizeFrame different than M4OSA_NULL means that resizing is needed */
        if (M4OSA_NULL != pC->pPreResizeFrame)
        {
            /** FB 2008/10/20:
            Used for cropping and black borders*/
            M4AIR_Params Params;

            M4OSA_TRACE3_0("M4MCS_intApplyVPP_NV12: Need to resize");
            err = pC->m_pVideoDecoder->m_pFctRender(pC->pViDecCtxt, &mtCts,
                pC->pPreResizeFrame, M4OSA_TRUE);
            if (M4NO_ERROR != err)
            {
                M4OSA_TRACE1_1("M4MCS_intApplyVPP_NV12: m_pFctRender returns 0x%x!", err);
                return err;
            }

            if(pC->MediaRendering == M4MCS_kResizing)
            {
                /*
                 * Call the resize filter. From the intermediate frame to the encoder
                 * image plane
                 */
                yuvFrameWidth = pC->pPreResizeFrame[0].u_width;
                yuvFrameHeight = pC->pPreResizeFrame[0].u_height;

                // Rotate the buffer if the original video has rotation information in MCS process.
                if (pC->EncodingVideoFormat != M4ENCODER_kNULL
                    && pC->pReaderVideoStream->videoRotationDegrees != 0) {
                   err = M4VSS3GPP_intRotateVideo_NV12(pC->pPreResizeFrame,
                           pC->pReaderVideoStream->videoRotationDegrees);
                   if (M4NO_ERROR != err)
                   {
                       M4OSA_TRACE1_1("M4MCS_intApplyVPP_NV12: M4VSS3GPP_intRotateVideo_NV12 returns 0x%x!", err);
                       return err;
                   }
                }

                err = M4VIFI_ResizeBilinearNV12toNV12(M4OSA_NULL,
                    pC->pPreResizeFrame, pPlaneOut);
                if (M4NO_ERROR != err)
                {
                    M4OSA_TRACE1_1("M4MCS_intApplyVPP_NV12: M4ViFilResizeBilinearNV12toNV12\
                                   returns 0x%x!", err);
                    return err;
                }
            }
            else
            {
                M4VIFI_ImagePlane pImagePlanesTemp[2];
                M4VIFI_ImagePlane* pPlaneTemp;
                M4OSA_UInt8* pOutPlaneY = pPlaneOut[0].pac_data +
                                          pPlaneOut[0].u_topleft;
                M4OSA_UInt8* pOutPlaneUV = pPlaneOut[1].pac_data +
                                          pPlaneOut[1].u_topleft;
                M4OSA_UInt8* pInPlaneY = M4OSA_NULL;
                M4OSA_UInt8* pInPlaneUV = M4OSA_NULL;
                M4OSA_UInt32 i = 0;

                // Rotate the buffer if the original video has rotation information in MCS process.
                if (pC->EncodingVideoFormat != M4ENCODER_kNULL
                    && pC->pReaderVideoStream->videoRotationDegrees != 0) {
                   yuvFrameWidth = pC->pPreResizeFrame[0].u_width;
                   yuvFrameHeight = pC->pPreResizeFrame[0].u_height;
                   err = M4VSS3GPP_intRotateVideo_NV12(pC->pPreResizeFrame, pC->pReaderVideoStream->videoRotationDegrees);
                   if (M4NO_ERROR != err)
                   {
                       M4OSA_TRACE1_1("M4MCS_intApplyVPP_NV12: M4VSS3GPP_intRotateVideo_NV12 returns 0x%x!", err);
                       return err;
                   }
                }

                /*FB 2008/10/20: to keep media aspect ratio*/
                /*Initialize AIR Params*/
                Params.m_inputCoord.m_x = 0;
                Params.m_inputCoord.m_y = 0;
                Params.m_inputSize.m_height = pC->pPreResizeFrame->u_height;
                Params.m_inputSize.m_width = pC->pPreResizeFrame->u_width;
                Params.m_outputSize.m_width = pPlaneOut->u_width;
                Params.m_outputSize.m_height = pPlaneOut->u_height;
                Params.m_bOutputStripe = M4OSA_FALSE;
                Params.m_outputOrientation = M4COMMON_kOrientationTopLeft;
                /**
                Media rendering: Black borders*/
                if(pC->MediaRendering == M4MCS_kBlackBorders)
                {
                    pImagePlanesTemp[0].u_width = pPlaneOut[0].u_width;
                    pImagePlanesTemp[0].u_height = pPlaneOut[0].u_height;
                    pImagePlanesTemp[0].u_stride = pPlaneOut[0].u_width;
                    pImagePlanesTemp[0].u_topleft = 0;

                    pImagePlanesTemp[1].u_width = pPlaneOut[1].u_width;
                    pImagePlanesTemp[1].u_height = pPlaneOut[1].u_height;
                    pImagePlanesTemp[1].u_stride = pPlaneOut[1].u_width;
                    pImagePlanesTemp[1].u_topleft = 0;


                    /* Allocates plan in local image plane structure */
                    pImagePlanesTemp[0].pac_data =
                        (M4OSA_UInt8*)M4OSA_32bitAlignedMalloc(pImagePlanesTemp[0]\
                        .u_width * pImagePlanesTemp[0].u_height, M4VS,
                        (M4OSA_Char *)"M4xVSS_PictureCallbackFct: temporary plane bufferY") ;
                    if(pImagePlanesTemp[0].pac_data == M4OSA_NULL)
                    {
                        M4OSA_TRACE1_0("Error alloc in M4MCS_intApplyVPP_NV12");
                        return M4ERR_ALLOC;
                    }
                    pImagePlanesTemp[1].pac_data =
                        (M4OSA_UInt8*)M4OSA_32bitAlignedMalloc(pImagePlanesTemp[1]\
                        .u_width * pImagePlanesTemp[1].u_height, M4VS,
                        (M4OSA_Char *)"M4xVSS_PictureCallbackFct: temporary plane bufferU") ;
                    if(pImagePlanesTemp[1].pac_data == M4OSA_NULL)
                    {
                        M4OSA_TRACE1_0("Error alloc in M4MCS_intApplyVPP_NV12");
                        return M4ERR_ALLOC;
                    }

                    pInPlaneY = pImagePlanesTemp[0].pac_data ;
                    pInPlaneUV = pImagePlanesTemp[1].pac_data ;

                    memset((void *)pImagePlanesTemp[0].pac_data,Y_PLANE_BORDER_VALUE,
                        (pImagePlanesTemp[0].u_height*pImagePlanesTemp[0].u_stride));
                    memset((void *)pImagePlanesTemp[1].pac_data,UV_PLANE_BORDER_VALUE,
                        (pImagePlanesTemp[1].u_height*pImagePlanesTemp[1].u_stride));
                    if((M4OSA_UInt32)((pC->pPreResizeFrame->u_height * pPlaneOut->u_width)\
                         /pC->pPreResizeFrame->u_width) <= pPlaneOut->u_height)
                         //Params.m_inputSize.m_height < Params.m_inputSize.m_width)
                    {
                        /*it is height so black borders will be on the top and on the bottom side*/
                        Params.m_outputSize.m_width = pPlaneOut->u_width;
                        Params.m_outputSize.m_height =
                             (M4OSA_UInt32)
                             ((pC->pPreResizeFrame->u_height * pPlaneOut->u_width)\
                             /pC->pPreResizeFrame->u_width);
                        /*number of lines at the top*/
                        pImagePlanesTemp[0].u_topleft =
                             (M4MCS_ABS((M4OSA_Int32)
                             (pImagePlanesTemp[0].u_height\
                             -Params.m_outputSize.m_height)>>1)) *
                             pImagePlanesTemp[0].u_stride;
                        pImagePlanesTemp[0].u_height = Params.m_outputSize.m_height;
                        pImagePlanesTemp[1].u_topleft =
                             (M4MCS_ABS((M4OSA_Int32)(pImagePlanesTemp[1].u_height\
                             -(Params.m_outputSize.m_height>>1)))>>1)\
                             * pImagePlanesTemp[1].u_stride;
                        pImagePlanesTemp[1].u_height = Params.m_outputSize.m_height>>1;

                    }
                    else
                    {
                        /*it is width so black borders will be on the left and right side*/
                        Params.m_outputSize.m_height = pPlaneOut->u_height;
                        Params.m_outputSize.m_width =
                             (M4OSA_UInt32)((pC->pPreResizeFrame->u_width
                             * pPlaneOut->u_height)\
                             /pC->pPreResizeFrame->u_height);

                        pImagePlanesTemp[0].u_topleft =
                             (M4MCS_ABS((M4OSA_Int32)(pImagePlanesTemp[0].u_width-\
                                Params.m_outputSize.m_width)>>1));
                        pImagePlanesTemp[0].u_width = Params.m_outputSize.m_width;
                        pImagePlanesTemp[1].u_topleft =
                             (M4MCS_ABS((M4OSA_Int32)(pImagePlanesTemp[1].u_width-\
                                Params.m_outputSize.m_width))>>1);
                        pImagePlanesTemp[1].u_width = Params.m_outputSize.m_width;

                    }

                    /*Width and height have to be even*/
                    Params.m_outputSize.m_width = (Params.m_outputSize.m_width>>1)<<1;
                    Params.m_outputSize.m_height = (Params.m_outputSize.m_height>>1)<<1;
                    Params.m_inputSize.m_width = (Params.m_inputSize.m_width>>1)<<1;
                    Params.m_inputSize.m_height = (Params.m_inputSize.m_height>>1)<<1;
                    pImagePlanesTemp[0].u_width = (pImagePlanesTemp[0].u_width>>1)<<1;
                    pImagePlanesTemp[1].u_width = (pImagePlanesTemp[1].u_width>>1)<<1;
                    pImagePlanesTemp[0].u_height = (pImagePlanesTemp[0].u_height>>1)<<1;
                    pImagePlanesTemp[1].u_height = (pImagePlanesTemp[1].u_height>>1)<<1;


                    /*Check that values are coherent*/
                    if(Params.m_inputSize.m_height == Params.m_outputSize.m_height)
                    {
                        Params.m_inputSize.m_width = Params.m_outputSize.m_width;
                    }
                    else if(Params.m_inputSize.m_width == Params.m_outputSize.m_width)
                    {
                        Params.m_inputSize.m_height = Params.m_outputSize.m_height;
                    }
                    pPlaneTemp = pImagePlanesTemp;
                }
                /**
                Media rendering: Cropping*/
                if(pC->MediaRendering == M4MCS_kCropping)
                {
                    Params.m_outputSize.m_height = pPlaneOut->u_height;
                    Params.m_outputSize.m_width = pPlaneOut->u_width;
                    if((Params.m_outputSize.m_height * Params.m_inputSize.m_width)\
                         /Params.m_outputSize.m_width<Params.m_inputSize.m_height)
                    {
                        /*height will be cropped*/
                        Params.m_inputSize.m_height =
                             (M4OSA_UInt32)((Params.m_outputSize.m_height \
                             * Params.m_inputSize.m_width) /
                             Params.m_outputSize.m_width);
                        Params.m_inputSize.m_height =
                            (Params.m_inputSize.m_height>>1)<<1;
                        Params.m_inputCoord.m_y =
                            (M4OSA_Int32)((M4OSA_Int32)
                            ((pC->pPreResizeFrame->u_height\
                            - Params.m_inputSize.m_height))>>1);
                    }
                    else
                    {
                        /*width will be cropped*/
                        Params.m_inputSize.m_width =
                             (M4OSA_UInt32)((Params.m_outputSize.m_width\
                                 * Params.m_inputSize.m_height) /
                                 Params.m_outputSize.m_height);
                        Params.m_inputSize.m_width =
                             (Params.m_inputSize.m_width>>1)<<1;
                        Params.m_inputCoord.m_x =
                            (M4OSA_Int32)((M4OSA_Int32)
                            ((pC->pPreResizeFrame->u_width\
                            - Params.m_inputSize.m_width))>>1);
                    }
                    pPlaneTemp = pPlaneOut;
                }
                /**
                 * Call AIR functions */
                if(M4OSA_NULL == pC->m_air_context)
                {
                    err = M4AIR_create_NV12(&pC->m_air_context, M4AIR_kNV12P);
                    if(err != M4NO_ERROR)
                    {
                        M4OSA_TRACE1_1("M4xVSS_PictureCallbackFct:\
                         Error when initializing AIR_NV12: 0x%x", err);
                        return err;
                    }
                }

                err = M4AIR_configure_NV12(pC->m_air_context, &Params);
                if(err != M4NO_ERROR)
                {
                    M4OSA_TRACE1_1("M4xVSS_PictureCallbackFct:\
                     Error when configuring AIR_NV12: 0x%x", err);
                    M4AIR_cleanUp_NV12(pC->m_air_context);
                    return err;
                }

                err = M4AIR_get_NV12(pC->m_air_context, pC->pPreResizeFrame,
                                pPlaneTemp);
                if(err != M4NO_ERROR)
                {
                    M4OSA_TRACE1_1("M4xVSS_PictureCallbackFct:\
                     Error when getting AIR_NV12 plane: 0x%x", err);
                    M4AIR_cleanUp_NV12(pC->m_air_context);
                    return err;
                }
                if(pC->MediaRendering == M4MCS_kBlackBorders)
                {
                    for(i=0; i<pPlaneOut[0].u_height; i++)
                    {
                        memcpy((void *)pOutPlaneY,(void *)pInPlaneY,
                            pPlaneOut[0].u_width);
                        pInPlaneY += pPlaneOut[0].u_width;
                        pOutPlaneY += pPlaneOut[0].u_stride;
                    }
                    for(i=0; i<pPlaneOut[1].u_height; i++)
                    {
                        memcpy((void *)pOutPlaneUV,(void *)pInPlaneUV,
                            pPlaneOut[1].u_width);
                        pInPlaneUV += pPlaneOut[1].u_width;
                        pOutPlaneUV += pPlaneOut[1].u_stride;
                    }

                    for(i=0; i<2; i++)
                    {
                        if(pImagePlanesTemp[i].pac_data != M4OSA_NULL)
                        {
                            free(pImagePlanesTemp[i].pac_data);
                            pImagePlanesTemp[i].pac_data = M4OSA_NULL;
                        }
                    }
                }
            }

            if (pC->EncodingVideoFormat != M4ENCODER_kNULL
                && pC->pReaderVideoStream->videoRotationDegrees !=0
                && pC->pReaderVideoStream->videoRotationDegrees !=180) {
               M4VSS3GPP_intSetNV12Plane(pC->pPreResizeFrame,yuvFrameWidth, yuvFrameHeight);
            }

        }
        else
        {
            M4OSA_TRACE3_0("M4MCS_intApplyVPP_NV12: Don't need resizing");
            err = pC->m_pVideoDecoder->m_pFctRender(pC->pViDecCtxt,
                                                    &mtCts, pPlaneOut,
                                                    M4OSA_TRUE);
            if (M4NO_ERROR != err)
            {
                M4OSA_TRACE1_1("M4MCS_intApplyVPP_NV12: m_pFctRender returns 0x%x!", err);
                return err;
            }
        }
        pC->lastDecodedPlane = pPlaneOut;
    }
    else
    {
        /* Copy last decoded plane to output plane */
        memcpy((void *)pPlaneOut[0].pac_data,
                        (void *)pC->lastDecodedPlane[0].pac_data,
                         (pPlaneOut[0].u_height * pPlaneOut[0].u_width));
        memcpy((void *)pPlaneOut[1].pac_data,
                        (void *)pC->lastDecodedPlane[1].pac_data,
                          (pPlaneOut[1].u_height * pPlaneOut[1].u_width));

        pC->lastDecodedPlane = pPlaneOut;
    }


#endif /*M4MCS_AUDIOONLY*/
    M4OSA_TRACE3_0("M4MCS_intApplyVPP_NV12: returning M4NO_ERROR");
    return M4NO_ERROR;
}

