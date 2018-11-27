# Copyright 2018 The Fuchsia Authors
#
# Use of this source code is governed by a MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT

SRC_DIR := system/ulib/ffl
LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

KERNEL_INCLUDES += $(LOCAL_DIR)/include \
                   $(SRC_DIR)/include

MODULE_SRCS :=

include make/module.mk

