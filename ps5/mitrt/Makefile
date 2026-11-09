# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
#
#   make         build/ps5rt_start.o, build/ps5rt_new.o, build/ps5rt_netdb.o (with the PS5 payload SDK's compiler),
#                build/libc.raw.elf and build/libps5rt_platform.a (the platform layer for RADV titles, platform/)
#   make test    the host tests (tests/host_test.sh, platform/tests/host_test.sh)
#   make radv-title RADV_ARCHIVE=<libvulkan_radeon.ps5.a> NATIVE_TOOLING=<ps5-native-tool's sources>
#                the smallest RADV title (platform/radv-title), linked with RADV + this runtime + the platform layer
#                and this repository's linker scripts (platform/link), checked (platform/tools/check_link.py):
#                build/radv-title/app/eboot.bin, sce_module/libc.prx
#   make clean
# libc.raw.elf still has to be signed (e.g. ps5-native-tool self --sign) to become sce_module/libc.prx.

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_CXX ?= $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
PS5_CC ?= $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_AR ?= $(PS5_PAYLOAD_SDK)/bin/llvm-ar
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -ffunction-sections -fdata-sections
PLATFORM_CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections
MODULE_SDK ?= 0x02000009
COMPANION_SDK ?= 0x08050001

all: build/ps5rt_start.o build/ps5rt_new.o build/ps5rt_netdb.o build/libc.raw.elf build/libps5rt_platform.a

build/ps5rt_start.o: src/ps5rt_start.cpp
	@mkdir -p build
	$(PS5_CXX) $(CXXFLAGS) -fno-exceptions -c -o $@ $<

build/ps5rt_new.o: src/ps5rt_new.cpp
	@mkdir -p build
	$(PS5_CXX) $(CXXFLAGS) -fexceptions -c -o $@ $<

build/ps5rt_netdb.o: src/ps5rt_netdb.cpp
	@mkdir -p build
	$(PS5_CXX) $(CXXFLAGS) -fno-exceptions -c -o $@ $<

build/libc.raw.elf: tools/mkprx.py tools/sceelf.py libc/exports.txt
	@mkdir -p build
	python3 -B tools/mkprx.py --manifest libc/exports.txt --out $@ --module-sdk $(MODULE_SDK) --companion-sdk $(COMPANION_SDK)
	python3 -B tests/check_prx.py $@ libc/exports.txt >build/check_prx.txt || { cat build/check_prx.txt; rm -f $@; exit 1; }

# ---- the platform layer (platform/): MIT, plus compiler-rt 18.1.8's emutls.c and x86.c (Apache-2.0 WITH LLVM-exception)
PLATFORM_SRC := thread memstream stdio_wrap locale fs qsort_r misc
PLATFORM_CRT := emutls x86
PLATFORM_OBJ := $(PLATFORM_SRC:%=build/platform/ps5rt_%.o) $(PLATFORM_CRT:%=build/platform/crt_%.o)

build/platform/ps5rt_%.o: platform/src/ps5rt_%.c platform/include/ps5rt/platform.h
	@mkdir -p $(dir $@)
	$(PS5_CC) $(PLATFORM_CFLAGS) -Iplatform/include -c -o $@ $<

build/platform/crt_%.o: platform/third_party/compiler-rt-18.1.8/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 -O2 -ffunction-sections -fdata-sections -c -o $@ $<

build/libps5rt_platform.a: $(PLATFORM_OBJ)
	rm -f $@
	$(PS5_AR) rcsD $@ $(PLATFORM_OBJ)

# ---- the RADV test title (platform/radv-title) ----
RADV_ARCHIVE ?=
RADV_INCLUDE ?= $(abspath $(dir $(RADV_ARCHIVE))../include)
NATIVE_TOOLING ?=
LLD ?= ld.lld
HOST_CXX_NATIVE ?= clang++
SDK_LIB := $(PS5_PAYLOAD_SDK)/target/lib
RT := build/radv-title
CHECK_LINK := python3 -B platform/tools/check_link.py
# every system-module stub but those a title may not load (libkernel_sys, libkernel_web) or never needs
TITLE_STUBS = $(filter-out %/libkernel_sys.so %/libkernel_web.so %/libScePosixForWebKit.so %/libSceGLSlimVSH.so,\
	$(sort $(wildcard $(SDK_LIB)/*.so)))

radv-title: $(RT)/app/eboot.bin $(RT)/app/sce_module/libc.prx

radv-title-inputs:
	@test -n "$(RADV_ARCHIVE)" -a -f "$(RADV_ARCHIVE)" || { echo "RADV_ARCHIVE=<libvulkan_radeon.ps5.a> is needed"; exit 1; }
	@test -n "$(NATIVE_TOOLING)" -a -f "$(NATIVE_TOOLING)/native_app_builder.cpp" || \
		{ echo "NATIVE_TOOLING=<the folder of ps5-native-tool's sources (native_app_builder.cpp ...)>"; exit 1; }

$(RT)/radv_title.o: platform/radv-title/radv_title.c | radv-title-inputs
	@mkdir -p $(dir $@)
	$(PS5_CC) $(PLATFORM_CFLAGS) -I$(RADV_INCLUDE) -Iplatform/include -c -o $@ $<

# The PIE. tools/check_link.py: imports (title modules only, the C++ runtime the eboot's own), raw system calls
# (libc.a(syscalls.o) neither loaded nor called) and the --trace (every input allowed: no fork platform layer, no
# boilerplate start-up, libc.a members only from platform/libc-allow.txt).
$(RT)/radv-title-pie.elf: $(RT)/radv_title.o build/ps5rt_start.o build/ps5rt_new.o build/libps5rt_platform.a \
		platform/tools/ps5rt-radv-link.sh platform/tools/check_link.py platform/libc-allow.txt \
		platform/link/ps5rt-pie.ld platform/link/ps5rt-local.map | radv-title-inputs
	$(CHECK_LINK) syscalls-rsp $(SDK_LIB)/libc.a $(RT)/syscalls-y.rsp
	$(LLD) -m elf_x86_64 -pie -z max-page-size=0x4000 --hash-style=gnu --eh-frame-hdr \
		$$(sh platform/tools/ps5rt-radv-link.sh --scripts) \
		-e _start --no-dynamic-linker $$(sh platform/tools/ps5rt-radv-link.sh --flags) -o $@ \
		build/ps5rt_start.o build/ps5rt_new.o $(RT)/radv_title.o \
		$(RADV_ARCHIVE) $$(PS5RT_PLATFORM_LIB=build/libps5rt_platform.a sh platform/tools/ps5rt-radv-link.sh --archive) \
		$(SDK_LIB)/libc++.a $(SDK_LIB)/libc++abi.a $(SDK_LIB)/libunwind.a \
		--as-needed $(TITLE_STUBS) -L $(SDK_LIB) $(SDK_LIB)/libc.a -lpthread @$(RT)/syscalls-y.rsp \
		--trace >$(RT)/radv-title-pie.trace
	$(CHECK_LINK) imports $@ $(SDK_LIB) || { rm -f $@; exit 1; }
	$(CHECK_LINK) syscalls $(RT)/radv-title-pie.trace || { rm -f $@; exit 1; }
	$(CHECK_LINK) trace $(RT)/radv-title-pie.trace --sdk-lib $(SDK_LIB) --libc-allow platform/libc-allow.txt \
		--own build --archive $(RADV_ARCHIVE) --archive build/libps5rt_platform.a \
		--require ps5rt_start.o --require ps5rt_new.o --require radv_title.o || { rm -f $@; exit 1; }

$(RT)/ps5-native-tool: | radv-title-inputs
	@mkdir -p $(dir $@)
	$(HOST_CXX_NATIVE) -std=c++20 -O2 -o $@ $(addprefix $(NATIVE_TOOLING)/,native_app_builder.cpp elf_object.cpp \
		self_container.cpp sce_module_writer.cpp) -lz

# the eboot's version record names its components: the start-up object and the modules it loads
$(RT)/eboot.elf: $(RT)/radv-title-pie.elf $(RT)/ps5-native-tool
	$(RT)/ps5-native-tool link --in $< --out $@ --stub-dir $(SDK_LIB) --module-sdk $(MODULE_SDK) \
		--companion-sdk $(COMPANION_SDK) --file-name eboot.elf --component ps5rt_start.o \
		$$(readelf -dW $< | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/--component \1/p')

$(RT)/app/eboot.bin: $(RT)/eboot.elf $(RT)/ps5-native-tool
	@mkdir -p $(dir $@)
	$(RT)/ps5-native-tool self --sign --in $< --out $@ --magic 0x1D3D154F

$(RT)/app/sce_module/libc.prx: build/libc.raw.elf $(RT)/ps5-native-tool
	@mkdir -p $(dir $@)
	$(RT)/ps5-native-tool self --sign --in $< --out $@

test:
	sh tests/host_test.sh build/test
	sh platform/tests/host_test.sh build/test-platform

clean:
	rm -rf build

.PHONY: all test clean radv-title radv-title-inputs
