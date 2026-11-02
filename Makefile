# shpack 构建入口
#
#   make                 构建本机架构的 packer + stub
#   make ARCH=aarch64    交叉构建 aarch64 的 stub（需要 aarch64-linux-gnu-g++）
#   make clean
#
# 说明：packer 始终用宿主编译器构建（它只在构建机运行）；
#       stub 用目标架构编译器构建，并且必须 -static。
SHELL := /bin/bash

ARCH ?= $(shell uname -m)
CROSS_COMPILE ?= $(if $(filter aarch64,$(ARCH)),aarch64-linux-gnu-,$(if $(filter armv7,$(ARCH)),arm-linux-gnueabihf-,))

HOST_CXX ?= g++
TARGET_CXX ?= $(CROSS_COMPILE)g++

WARN := -Wall -Wextra -Wpedantic
CXXFLAGS ?= -std=c++17 -O2 $(WARN)
STUB_CXXFLAGS := -std=c++17 -Os $(WARN) -fno-exceptions -fno-rtti \
                 -ffunction-sections -fdata-sections -DNDEBUG
STUB_LDFLAGS := -static -s -Wl,--gc-sections

COMMON_SRC := src/sha256.cpp src/lz4.cpp src/util.cpp
HEADERS := src/format.h src/sha256.h src/lz4.h src/util.h
PACKER_SRC := $(COMMON_SRC) src/packer.cpp
STUB_SRC := $(COMMON_SRC) src/launcher.cpp

BUILD := build
HOST_DIR := $(BUILD)/host
TARGET_DIR := $(BUILD)/$(ARCH)

.PHONY: all packer stub clean check fix-perms

all: packer stub

packer: $(HOST_DIR)/shpack

stub: $(TARGET_DIR)/shpack-stub

$(HOST_DIR)/shpack: $(PACKER_SRC) $(HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(CXXFLAGS) -o $@ $(PACKER_SRC) -static

$(TARGET_DIR)/shpack-stub: $(STUB_SRC) $(HEADERS)
	@mkdir -p $(dir $@)
	$(TARGET_CXX) $(STUB_CXXFLAGS) -o $@ $(STUB_SRC) $(STUB_LDFLAGS)

check: all
	@file $(HOST_DIR)/shpack $(TARGET_DIR)/shpack-stub
	@echo "--- 动态依赖检查（应为 'not a dynamic executable'）---"
	@ldd $(TARGET_DIR)/shpack-stub 2>&1 || true

fix-perms:
	bash fix-perms.sh

clean:
	rm -rf $(BUILD) .build
