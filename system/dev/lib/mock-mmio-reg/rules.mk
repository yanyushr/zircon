# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_TYPE := userlib

MODULE_SRCS := $(LOCAL_DIR)/mock-mmio-reg.cpp

MODULE_STATIC_LIBS := \
    system/ulib/ddk \
    system/ulib/fbl \

MODULE_LIBS := \
    system/ulib/c \
    system/ulib/unittest \

include make/module.mk
