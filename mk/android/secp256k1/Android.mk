# SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# libsecp256k1 (nixpkgs' secp256k1.src) as a static ndk-build module: the
# BIP-340 Schnorr signatures behind Nostr signature checks and signing
# accounts (src/nostr_verify.cpp, src/nostr_sign.cpp). Mbed TLS has no
# Schnorr signatures. Upstream's default table sizes; only the modules
# Nostr needs.
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := secp256k1
LOCAL_SRC_FILES := src/secp256k1.c src/precomputed_ecmult.c src/precomputed_ecmult_gen.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_CFLAGS := -O2 -Wno-unused-function -Wno-unused-parameter \
	-DENABLE_MODULE_EXTRAKEYS=1 -DENABLE_MODULE_SCHNORRSIG=1

include $(BUILD_STATIC_LIBRARY)
