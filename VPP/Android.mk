ifeq ($(TARGET_HAS_VPP),true)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
        VPPSetting.cpp \
        VPPProcessor.cpp \
        VPPProcThread.cpp \
        VPPFillThread.cpp \
        VPPWorker.cpp \
        NuPlayerVPPProcessor.cpp

LOCAL_C_INCLUDES:= \
        $(call include-path-for, frameworks-av) \
        $(call include-path-for, frameworks-native) \
        $(call include-path-for, frameworks-native)/media/openmax \
        $(TARGET_OUT_HEADERS)/libva

LOCAL_SHARED_LIBRARIES := libva

LOCAL_COPY_HEADERS_TO := libmedia_utils_vpp

LOCAL_COPY_HEADERS := \
    VPPProcessor.h \
    VPPBuffer.h \
    VPPSetting.h \
    VPPProcThread.h \
    VPPFillThread.h \
    VPPWorker.h \
    NuPlayerVPPProcessor.h

LOCAL_CFLAGS += -DTARGET_HAS_VPP
ifeq ($(TARGET_VPP_USE_GEN),true)
	LOCAL_CFLAGS += -DTARGET_VPP_USE_GEN
endif

LOCAL_MODULE:=  libvpp
LOCAL_MODULE_TAGS := optional

include $(BUILD_STATIC_LIBRARY)
endif

