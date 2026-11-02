# shpack 构建入口
#
#   make                 构建宿主 packer/builder/bundler + 本机 stub
#   make ARCH=aarch64    交叉构建 aarch64 stub
#   make ARCH=armv7      交叉构建 armv7 stub（需 build-tools/armv7-g++）
#   make ARCH=i386       交叉构建 i386 stub（需 build-tools/i386-g++）
#   make fix-perms       修复网页上传丢失的可执行位
#   make clean
SHELL := /bin/bash

ARCH ?= $(shell uname -m)

CROSS_COMPILE ?= $(if $(filter aarch64,$(ARCH)),aarch64-linux-gnu-,$(if $(filter armv7,$(ARCH)),arm-linux-gnueabihf-,$(if $(filter i386,$(ARCH)),i686-linux-gnu-,$(if $(filter riscv64,$(ARCH)),riscv64-linux-gnu-,))))
# 优先使用 scripts/fetch-cross.sh 生成的无 root 工具链包装器
TARGET_CXX ?= $(if $(wildcard build-tools/$(ARCH)-g++),build-tools/$(ARCH)-g++,$(CROSS_COMPILE)g++)

HOST_CXX ?= g++
WARN := -Wall -Wextra -Wpedantic
CXXFLAGS ?= -std=c++17 -O2 $(WARN)
STUB_CXXFLAGS := -std=c++17 -Os $(WARN) -fno-exceptions -fno-rtti \
                 -ffunction-sections -fdata-sections -DNDEBUG
STUB_LDFLAGS := -static -s -Wl,--gc-sections
ZSTD_CPPFLAGS ?=
ZSTD_LIBS ?= -lzstd -pthread

COMMON_SRC := src/sha256.cpp src/lz4.cpp src/util.cpp
PACKAGE_SRC := $(COMMON_SRC) src/package.cpp
BUNDLE_SRC := $(PACKAGE_SRC) src/bundle.cpp
HEADERS := src/format.h src/sha256.h src/lz4.h src/util.h src/package.h src/bundle.h
PACKER_SRC := $(PACKAGE_SRC) src/packer.cpp
STUB_SRC := $(COMMON_SRC) src/launcher.cpp
BUILDER_SRC := $(BUNDLE_SRC) src/builder.cpp
BUNDLER_SRC := $(BUNDLE_SRC) src/bundler.cpp

BUILD := build
HOST_DIR ?= $(BUILD)/host
TARGET_DIR := $(BUILD)/$(ARCH)

.PHONY: all packer builder bundler stub clean check fix-perms

all: packer builder bundler stub

packer: $(HOST_DIR)/shpack

builder: $(HOST_DIR)/shpack-builder

bundler: $(HOST_DIR)/shpack-bundler

stub: $(TARGET_DIR)/shpack-stub

$(HOST_DIR)/shpack: $(PACKER_SRC) $(HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(CXXFLAGS) -o $@ $(PACKER_SRC) -static

$(HOST_DIR)/shpack-builder: $(BUILDER_SRC) $(HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(CXXFLAGS) $(ZSTD_CPPFLAGS) -o $@ $(BUILDER_SRC) -static $(ZSTD_LIBS)

$(HOST_DIR)/shpack-bundler: $(BUNDLER_SRC) $(HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(CXXFLAGS) -o $@ $(BUNDLER_SRC) -static

$(TARGET_DIR)/shpack-stub: $(STUB_SRC) $(HEADERS)
	@mkdir -p $(dir $@)
	$(TARGET_CXX) $(STUB_CXXFLAGS) -o $@ $(STUB_SRC) $(STUB_LDFLAGS)

check: all
	@file $(HOST_DIR)/shpack $(HOST_DIR)/shpack-builder $(HOST_DIR)/shpack-bundler \
	       $(TARGET_DIR)/shpack-stub
	@echo "--- 动态依赖检查（应为 'not a dynamic executable'）---"
	@ldd $(TARGET_DIR)/shpack-stub 2>&1 || true

fix-perms:
	bash fix-perms.sh

clean:
	rm -rf $(BUILD) .build
