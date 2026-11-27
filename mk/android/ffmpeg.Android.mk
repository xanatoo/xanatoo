# SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := ffmpeg-hls
LOCAL_SRC_FILES := deps/$(TARGET_ARCH_ABI)/lib/libhls.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/deps/$(TARGET_ARCH_ABI)/include
LOCAL_STATIC_LIBRARIES := mbedtls

include $(PREBUILT_STATIC_LIBRARY)
