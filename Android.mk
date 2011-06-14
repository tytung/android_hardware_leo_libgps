LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := optional

LOCAL_MODULE := libgps

LOCAL_SHARED_LIBRARIES := libutils libcutils librpc

LOCAL_C_INCLUDES := \
    $(TARGET_OUT_HEADERS)/librpc

LOCAL_SRC_FILES := \
		leo-gps.c \
		leo-gps-rpc.c \
		time.cpp \

include $(BUILD_SHARED_LIBRARY)
