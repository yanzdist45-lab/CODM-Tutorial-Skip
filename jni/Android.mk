LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := codm_tutorial_skip
LOCAL_SRC_FILES := main.cpp

LOCAL_LDLIBS := -llog -ldl

LOCAL_CFLAGS := \
    -O2 \
    -fvisibility=hidden \
    -fno-exceptions \
    -fno-rtti

LOCAL_CPPFLAGS := \
    -std=c++17

include $(BUILD_SHARED_LIBRARY)
