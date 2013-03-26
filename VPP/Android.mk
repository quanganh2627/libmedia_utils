ifeq ($(TARGET_HAS_VPP),true)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
        VPPProcessor.cpp \
        VPPProcThread.cpp \
        VPPFillThread.cpp \
        VPPWorker.cpp \

LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/av/include \
        $(TOP)/frameworks/native/include \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TARGET_OUT_HEADERS)/libva

LOCAL_SHARED_LIBRARIES := libva

LOCAL_COPY_HEADERS_TO := libmedia_utils_vpp

LOCAL_COPY_HEADERS := \
    VPPProcessor.h \
    VPPProcThread.h \
    VPPFillThread.h \
    VPPWorker.h

LOCAL_CFLAGS += -DTARGET_HAS_VPP
ifeq ($(TARGET_VPP_USE_GEN),true)
	LOCAL_CFLAGS += -DTARGET_VPP_USE_GEN
endif

LOCAL_MODULE:=  libvpp
LOCAL_MODULE_TAGS := optional

include $(BUILD_STATIC_LIBRARY)
endif

