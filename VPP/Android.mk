ifeq ($(TARGET_HAS_VPP),true)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
        VPPProcessor.cpp \
        VPPThread.cpp \
        VPPWorker.cpp \

LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/av/include \
        $(TOP)/frameworks/native/include \
        $(TOP)/frameworks/native/include/media/openmax

LOCAL_COPY_HEADERS_TO := libmedia_utils_vpp

LOCAL_COPY_HEADERS := \
    VPPProcessor.h \
    VPPThread.h \
    VPPWorker.h

LOCAL_CFLAGS += -DTARGET_HAS_VPP

LOCAL_MODULE:=  libvpp
LOCAL_MODULE_TAGS := optional

include $(BUILD_STATIC_LIBRARY)
endif

