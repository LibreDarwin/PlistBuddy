# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
# Reimplementation of Apple's /usr/libexec/PlistBuddy.
#
# Build layout: every artifact lives under build/; the tool goes to
# build/release/ or build/debug/ per CONFIG.
#
# Portable to both GNU make and BSD make (bmake): no pattern rules, no
# ifeq/ifdef/.if conditionals and no $(if)/$(shell) functions.  Per-config
# flags come from make/<CONFIG>.mk so both make variants behave identically.

CONFIG ?= release
SDK    ?= /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
CC     := /Users/sunneva/xnuports-root/devel/xcode-tools/build/release/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang

-include make/$(CONFIG).mk

BUILD_DIR := build/$(CONFIG)
OBJDIR    := $(BUILD_DIR)/obj

CFLAGS := $(OPT) -std=c11 -D_DARWIN_C_SOURCE -isysroot "$(SDK)" -Wall -Wextra

PLISTBUDDY := $(BUILD_DIR)/PlistBuddy
OBJS       := $(OBJDIR)/PlistBuddy.o

all: $(PLISTBUDDY)

$(PLISTBUDDY): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -framework CoreFoundation

$(OBJDIR)/PlistBuddy.o: src/PlistBuddy.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/PlistBuddy.c

# Byte-parity check against Apple's /usr/libexec/PlistBuddy, run by
# tests/parity.sh.  MY is a path relative to the project root because this
# Makefile must stay free of $(shell)/$(CURDIR) to work under both GNU make
# and bmake.  The script is not a prerequisite, or make would try to build it.
parity: all
	MY="build/$(CONFIG)/PlistBuddy" bash tests/parity.sh

test: parity

clean:
	rm -rf build

.PHONY: all parity test clean
