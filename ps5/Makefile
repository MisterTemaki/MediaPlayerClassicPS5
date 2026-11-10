# Media Player Classic PS5
#   make deps   -> build/deps/prefix-{ps5,host}   FFmpeg, libsmb2, Mbed TLS, zlib, libass (tools/build-deps.sh; once)
#   make deps-subs -> the same prefixes: only FreeType, FriBidi, HarfBuzz, libass (tools/build-deps.sh subs)
#   make deps-disc -> the same prefixes: libudfread, libbluray, libdvdread, libdvdnav, then FFmpeg again with its DVD-Video
#                  demuxer (tools/build-deps.sh disc; 1.4)
#   make ps5    -> build/ps5/MediaPlayerClassicPS5.elf   the payload to send with the PS5 Payload Manager / ELF loader:
#                  it installs or updates the dashboard app /data/homebrew/$(TITLE_ID)/ (the player itself, icon
#                  included) and stays running as the helper that lets the app reach /data and USB drives
#   make app    -> build/app/$(TITLE_ID)/     the app folder on its own (eboot.bin, sce_module/, sce_sys/)
#   make host   -> build/host/                Linux builds of the same code (PS5 calls faked by host/sce_host.cpp)
#   make test   -> host builds + tests/run-tests.sh
#   make dist   -> build/dist/MediaPlayerClassicPS5-v$(VERSION).elf + mpc-ps5-v$(VERSION)-src.zip (release names)
#   make send   -> sends MediaPlayerClassicPS5.elf to $(PS5_HOST):$(PS5_PORT) with the SDK's prospero-deploy
#
# The layout is PS5SX2's (and the emulator ports'): eboot.bin is the player, a native PS5 title (the console
# gives the controller to the title in front only), and the payload is its installer and helper:
#   1. build/ps5/MediaPlayerClassicPS5-helper.elf   the helper alone (payload); the app sends it to the ELF loader
#   2. build/app/$(TITLE_ID)/                        eboot.bin = the player (player/, FFmpeg, libsmb2) + the PS5
#                                                    layer + the helper from step 1 (native title, signed);
#                                                    sce_module/libc.prx; sce_sys/
#   3. build/ps5/MediaPlayerClassicPS5.elf          installer + helper, with the app folder from step 2 built in
#
# Dependencies: the ps5-payload-dev SDK (v0.42 or newer, https://github.com/ps5-payload-dev/sdk) in
# $(PS5_PAYLOAD_SDK), with clang/lld 18 on the host, nasm, git (tools/build-deps.sh fetches FFmpeg n7.1.5 and
# libsmb2).

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_HOST ?= ps5
PS5_PORT ?= 9021
VERSION ?= 1.6.1
TITLE_ID ?= PPSA99015
# where the app looks for its updates (Help, Check for updates): this repository's GitHub releases
UPDATE_REPO ?= MisterTemaki/MediaPlayerClassicPS5
APP_OUT := build/app/$(TITLE_ID)
DEPS_PS5 ?= build/deps/prefix-ps5
DEPS_HOST ?= build/deps/prefix-host

# the app (eboot.bin): the PS5 layer and the player
EMU_SRC := coreorbis/main-boot.cpp \
	coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoNotify.cpp \
	coreorbis/orbis-shims/ProsperoVideo.cpp coreorbis/orbis-shims/ProsperoAudio.cpp \
	coreorbis/orbis-shims/ProsperoHdr.cpp \
	coreorbis/orbis-shims/ProsperoInput.cpp coreorbis/orbis-shims/ProsperoJailbreak.cpp \
	coreorbis/orbis-shims/ProsperoCrash.cpp \
	player/mpc_app.cpp player/mpc_dialogs.cpp player/mpc_font.cpp player/mpc_gdi.cpp player/mpc_menu.cpp \
	player/mpc_player.cpp player/mpc_res.cpp player/mpc_settings.cpp player/mpc_smb.cpp player/mpc_ui.cpp \
	player/mpc_console.cpp player/mpc_vdec.cpp player/mpc_tonemap.cpp player/mpc_update.cpp player/mpc_i18n.cpp \
	player/mpc_ass.cpp player/mpc_sfnt.cpp player/mpc_disc.cpp player/mpc_gpu.cpp player/mpc_bigmem.cpp \
	coreorbis/orbis-shims/ProsperoUpdate.cpp coreorbis/orbis-shims/ProsperoAppPlaceApp.cpp
# the payloads: the helper, and the installer (= helper + install). ProsperoAppPlace.cpp reads the mount table there
# (statfs / getfsstat, libkernel_sys); the app has its own build of it without (ProsperoAppPlaceApp.cpp, #3)
HELPER_SRC := coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoNotify.cpp \
	coreorbis/orbis-shims/ProsperoHelper.cpp coreorbis/orbis-shims/ProsperoCrash.cpp coreorbis/orbis-shims/ProsperoAppPlace.cpp
INSTALLER_SRC := installer/installer_main.cpp $(HELPER_SRC) coreorbis/orbis-shims/ProsperoInstall.cpp \
	coreorbis/orbis-shims/ProsperoUpdate.cpp

# Ed25519 for the release signatures (Monocypher 4.0.2, BSD-2-Clause / CC0): in the app and the installer, which both
# link ProsperoUpdate.cpp
MONO_SRC := third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c
# The release key: keys/release-ed25519.pub (64 hex digits, tools/make_release_key.sh) built in; without it the
# build checks no signature (and says so in the log)
RELEASE_KEY ?= $(shell tr -d ' \n' < keys/release-ed25519.pub 2>/dev/null)

# 1.6: the GPU renderer (ps5-slang-gpu, MIT, vendored as ps5-slang-gpu/: tools/vendor.sh of that repository; checked
# against its VENDOR.txt before anything of it is built). The video is drawn by its SetVideo pass (mpc_gpu.cpp), the
# window over it as the UI surface. The PS5 build links RADV (Mesa's AMD Vulkan driver, MIT, built against mitrt's MIT
# ps5platform headers: ../../rsdeps/radv-mit) and the platform layer RADV needs (mitrt/platform, MIT: open_memstream,
# the C locale's *_l functions, fstatat & co, emulated TLS). GPU=0: the CPU-only player of 1.5.4 (the library's stub).
GPU ?= 1
SLANG_GPU := ps5-slang-gpu
SLANG_GPU_GEN := build/gen/slang-gpu
SLANG_GPU_MITRT := mitrt
include $(SLANG_GPU)/mk/slang-gpu.mk

INCS := -Icoreorbis/include-orbis -Icoreorbis/orbis-shims -Iplayer -isystem third_party/stb -isystem third_party/nanosvg \
	$(SLANG_GPU_INC)
DEFS := -DMPCPS5_PS5_VERSION='"$(VERSION)"' -DMPCPS5_TITLE_ID='"$(TITLE_ID)"' -DMPCPS5_UPDATE_REPO='"$(UPDATE_REPO)"' -DMPC_ASSET_DIR='"$(abspath assets)"' \
	-DORBIS_ROOT_DEFAULT='"/data/mpcps5"' -DMPCPS5_RELEASE_KEY='"$(RELEASE_KEY)"' -Ithird_party/monocypher
PORT_WARN := -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers
FF_LIBS = -lavformat -lavcodec -lavfilter -lswscale -lswresample -lavutil -ldav1d -lsmb2 -lmbedtls -lmbedx509 -lmbedcrypto -lz \
	-lass -lharfbuzz -lfribidi -lfreetype -ldvdnav -ldvdread -lbluray -ludfread

# stb and NanoSVG inside mpc_res.o / mpc_font.o have warnings of their own; the resources are built in (their files:
# OBJ_DEPS below)
%/port/player/mpc_res.o: EXTRA_FLAGS = -Wno-error -Wno-sign-compare -Wno-unused-function -Wno-unused-but-set-variable \
	-Wno-implicit-fallthrough -Wno-missing-field-initializers
%/port/player/mpc_font.o: EXTRA_FLAGS = -Wno-error -Wno-sign-compare -Wno-unused-function
# the helper payload is installer_main.cpp without the install step
%/port/installer/helper_main.o: EXTRA_FLAGS = -DMPCPS5_HELPER_ONLY

# 1.5.2: what an object is built from besides its own source and the headers, as explicit targets for both builds (a
# pattern rule without a recipe adds no prerequisite -- it only cancels an implicit rule --, so up to 1.5.1 mpc_res.o
# and mpc_app.o weren't rebuilt when the fonts, the toolbar, the logo or the CA list they carry changed)
define OBJ_DEPS
build/ps5/port/$(1).o build/host/port/$(1).o: $(2)
endef
$(eval $(call OBJ_DEPS,player/mpc_res,$(wildcard assets/res/* assets/fonts/*)))
$(eval $(call OBJ_DEPS,player/mpc_app,assets/data/cacert.pem))
# 1.5.2: the flags the objects are compiled with (the version, the title, the repository, the release key...) written to
# build/<ps5|host>/flags.txt when they change, and every object depends on it: a VERSION or UPDATE_REPO given to make
# rebuilds what carries it instead of leaving the old value in the objects (up to 1.5.1 that took a make clean)
# ($(2): the name of the variable holding them -- they have commas, which call's arguments can't carry)
define FLAGS_STAMP
$$(shell mkdir -p $(1))
ifneq ($$(file < $(1)/flags.txt),$$($(2)))
$$(file > $(1)/flags.txt,$$($(2)))
endif
endef

# ---------------- PS5 ----------------
PS5_CC  := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_CXX := $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
PS5_OUT := build/ps5
# The PS5's CPU is Zen 2.
PS5_OPT := -O3 -fPIC -march=znver2 -fno-strict-aliasing
PS5_PAYLOAD_LIBS := -lkernel_sys -lSceNet -lSceNetCtl -lSceSsl -lSceHttp2 -lpthread
PS5_DEP_INCS := -isystem $(DEPS_PS5)/include
PS5_FLAGS_TEXT = $(strip GPU=$(GPU) $(PS5_CXX) $(PS5_CC) $(PS5_OPT) $(DEFS) $(INCS) $(PS5_DEP_INCS) $(PORT_WARN))
$(eval $(call FLAGS_STAMP,build/ps5,PS5_FLAGS_TEXT))

PS5_EMU_OBJ := $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(EMU_SRC)) $(patsubst %.c,$(PS5_OUT)/port/%.o,$(MONO_SRC))
PS5_HELPER_OBJ := $(PS5_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(HELPER_SRC))
PS5_INSTALLER_OBJ := $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(INSTALLER_SRC)) $(patsubst %.c,$(PS5_OUT)/port/%.o,$(MONO_SRC))

ps5: $(PS5_OUT)/MediaPlayerClassicPS5.elf

deps:
	bash tools/build-deps.sh

deps-subs:
	bash tools/build-deps.sh subs

deps-disc:
	bash tools/build-deps.sh disc

$(PS5_OUT)/port/%.o: %.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h player/*.h $(PS5_OUT)/flags.txt
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PS5_DEP_INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(PS5_OUT)/port/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=c99 $(PS5_OPT) -c -o $@ $<

$(PS5_OUT)/port/installer/helper_main.o: installer/installer_main.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h \
		$(PS5_OUT)/flags.txt
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

# 1. the helper payload (built into the app, which sends it to the ELF loader when no helper answers)
$(PS5_OUT)/MediaPlayerClassicPS5-helper.elf: $(PS5_HELPER_OBJ)
	$(PS5_CXX) -o $@ $^ $(PS5_PAYLOAD_LIBS)

$(PS5_OUT)/helper_data.o: coreorbis/orbis-shims/helper_data.cpp coreorbis/orbis-shims/ProsperoJailbreak.h $(PS5_OUT)/MediaPlayerClassicPS5-helper.elf \
		$(PS5_OUT)/flags.txt
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) -DHELPER_ELF='"$(abspath $(PS5_OUT)/MediaPlayerClassicPS5-helper.elf)"' -c -o $@ $<

# 3. the installer + helper, with the app folder built in
APP_FILES = $(APP_OUT)/eboot.bin $(APP_OUT)/sce_sys/param.json $(APP_OUT)/sce_sys/icon0.png $(APP_OUT)/sce_sys/pic0.dds \
	$(APP_OUT)/sce_sys/pic1.dds \
	$(APP_OUT)/sce_module/libc.prx
# the eboot and the backgrounds go in as zlib streams (ProsperoInstall inflates them): the payload is half the size
ZIPPED := build/zipped
$(ZIPPED)/%.z: $(APP_OUT)/%
	@mkdir -p $(dir $@)
	python3 -c "import sys, zlib; open(sys.argv[2], 'wb').write(zlib.compress(open(sys.argv[1], 'rb').read(), 9))" $< $@
APP_DATA_DEFS = -DAPP_EBOOT='"$(abspath $(ZIPPED)/eboot.bin.z)"' -DAPP_PARAM='"$(abspath $(APP_OUT)/sce_sys/param.json)"' \
	-DAPP_ICON0='"$(abspath $(APP_OUT)/sce_sys/icon0.png)"' -DAPP_PIC0='"$(abspath $(ZIPPED)/sce_sys/pic0.dds.z)"' \
	-DAPP_PIC1='"$(abspath $(ZIPPED)/sce_sys/pic1.dds.z)"' \
	-DAPP_LIBC='"$(abspath $(APP_OUT)/sce_module/libc.prx)"' \
	-DAPP_EBOOT_SIZE=$$(stat -c %s $(APP_OUT)/eboot.bin) -DAPP_PIC0_SIZE=$$(stat -c %s $(APP_OUT)/sce_sys/pic0.dds) \
	-DAPP_PIC1_SIZE=$$(stat -c %s $(APP_OUT)/sce_sys/pic1.dds)

$(PS5_OUT)/install_data.o: coreorbis/orbis-shims/install_data.cpp coreorbis/orbis-shims/ProsperoInstall.h $(APP_FILES) $(PS5_OUT)/flags.txt \
		$(ZIPPED)/eboot.bin.z $(ZIPPED)/sce_sys/pic0.dds.z $(ZIPPED)/sce_sys/pic1.dds.z
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(APP_DATA_DEFS) -c -o $@ $<

$(PS5_OUT)/port/coreorbis/orbis-shims/ProsperoInstall.o: EXTRA_FLAGS = $(PS5_DEP_INCS)
$(PS5_OUT)/MediaPlayerClassicPS5.elf: $(PS5_INSTALLER_OBJ) $(PS5_OUT)/install_data.o
	$(PS5_CXX) -o $@ $^ $(DEPS_PS5)/lib/libz.a $(PS5_PAYLOAD_LIBS)
	@echo "built $@ ($$(stat -c %s $@) bytes)"

# the release files, named with the version: build/dist/MediaPlayerClassicPS5-v$(VERSION).elf and the sources zip
DIST := build/dist
# + the app folder as a stored zip with its manifest (MediaPlayerClassicPS5-v$(VERSION)-$(TITLE_ID).zip): attach it to
# the GitHub release tagged v$(VERSION), so the app can update itself (Help, Check for updates)
dist: $(PS5_OUT)/MediaPlayerClassicPS5.elf $(APP_FILES)
	@mkdir -p $(DIST)
	cp $< $(DIST)/MediaPlayerClassicPS5-v$(VERSION).elf
	python3 tools/make_release_zip.py $(APP_OUT) $(TITLE_ID) $(DIST)/MediaPlayerClassicPS5-v$(VERSION)-$(TITLE_ID).zip
	@if [ -n "$(RELEASE_SIGNING_KEY)" ]; then bash tools/sign_release.sh $(DIST)/MediaPlayerClassicPS5-v$(VERSION)-$(TITLE_ID).zip "$(RELEASE_SIGNING_KEY)"; \
	elif [ -n "$(RELEASE_KEY)" ]; then echo "note: this build checks signatures; sign the zip (make dist RELEASE_SIGNING_KEY=<key>) or the app won't install it"; fi
	cd ../.. && rm -f $(abspath $(DIST))/mpc-ps5-v$(VERSION)-src.zip && \
		zip -qr $(abspath $(DIST))/mpc-ps5-v$(VERSION)-src.zip $(notdir $(abspath ..)) \
		-x '*/ps5/build/*' '*/proto/native/build/*' '*.o' '*/.git/*'
	@ls -l $(DIST)

send: $(PS5_OUT)/MediaPlayerClassicPS5.elf
	$(PS5_PAYLOAD_SDK)/bin/prospero-deploy -h $(PS5_HOST) -p $(PS5_PORT) $<

# ---------------- the app (eboot.bin = the player) ----------------
# 2. A native PS5 title linked the way PS5SX2 links its eboot (ps5/coreorbis/link-vk.sh): the boilerplate's
# app_crt.cpp (_start -> _init_env -> main) and app_cpp_runtime.cpp, the player's objects, FFmpeg and the
# other static libraries, a PIE laid out by ps5-pie.ld (+ PS5SX2's ehframe.ld) against the SDK's system-module
# stubs and its static libc++/libc++abi/libunwind/libc, every defined symbol local (localize.map); then
# ps5-native-tool link (SCE dynamic tables, process parameters) and self --sign.
NATIVE := proto/native
NATIVE_TOOL := $(NATIVE)/build/host/ps5-native-tool
LIBC_BUILDER := $(NATIVE)/build/host/libc-builder
LLD ?= ld.lld
NATIVE_CLANG = PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) sh $(NATIVE)/tooling/prospero-clang18
SDK_LIB := $(PS5_PAYLOAD_SDK)/target/lib
# Not libkernel_sys / libkernel_web either: a title can't load them (1.5 picked up libkernel_sys through statfs and
# crashed at launch, #3); with them left out such a symbol is a link error. tools/check_imports.py checks the result.
APP_STUBS = $(filter-out %/libScePosixForWebKit.so %/libSceGLSlimVSH.so %/libkernel_sys.so %/libkernel_web.so,\
	$(sort $(wildcard $(SDK_LIB)/*.so)))

$(NATIVE_TOOL): $(NATIVE)/tooling/native/native_app_builder.cpp $(NATIVE)/tooling/native/elf_object.cpp \
		$(NATIVE)/tooling/native/self_container.cpp $(NATIVE)/tooling/native/sce_module_writer.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX_NATIVE) -std=c++20 -O2 -o $@ $^ -lz
HOST_CXX_NATIVE ?= clang++

$(LIBC_BUILDER): $(NATIVE)/tooling/native/libc_builder.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX_NATIVE) -std=c++20 -O2 -Wall -Wextra -Werror -o $@ $<

# sce_module/libc.prx, deterministic: it must match the published digest (proto/native/libc.prx.sha256)
build/app/libc.prx: $(LIBC_BUILDER) $(NATIVE_TOOL) $(wildcard $(NATIVE)/tooling/native/runtime/*.txt)
	@mkdir -p $(dir $@)
	$(LIBC_BUILDER) $(NATIVE)/tooling/native/runtime/api-surface.txt $(NATIVE)/tooling/native/runtime/imports.txt build/app/libc.raw.elf
	$(NATIVE_TOOL) self --sign --in build/app/libc.raw.elf --out $@
	cd build/app && sha256sum --check --strict '$(abspath $(NATIVE)/libc.prx.sha256)'

$(APP_OUT)/sce_module/libc.prx: build/app/libc.prx
	@mkdir -p $(dir $@)
	cp $< $@

app: $(APP_FILES)

# app_cpp_runtime.cpp (operator new / delete) with exceptions (1.5.2): a failed allocation throws std::bad_alloc
# (1.5.1's trapped: SIGILL), caught where the code catches it; app_crt.cpp stays without
APP_CRT_EH_app_crt := -fno-exceptions
APP_CRT_EH_app_cpp_runtime := -fexceptions
build/app/app_crt.o build/app/app_cpp_runtime.o: build/app/%.o: $(NATIVE)/tooling/native/%.cpp Makefile
	@mkdir -p $(dir $@)
	$(NATIVE_CLANG) -std=c++20 -O2 -Wall -Wextra $(APP_CRT_EH_$*) -fno-rtti -ffunction-sections -fdata-sections \
		-c -o $@ $<

# 1.6: the GPU renderer's objects (the library, glslang, the tables), or its stub (GPU=0)
PS5_GPU_OBJ := $(call SLANG_GPU_OBJECTS,$(PS5_OUT)/slang-gpu,$(PS5_CXX),-O3 -fPIC -march=znver2 -fno-strict-aliasing $(SLANG_GPU_PS5_FLAGS),-O2 -fPIC -march=znver2)
PS5_GPU_STUB_OBJ := $(call SLANG_GPU_STUB_OBJECTS,$(PS5_OUT)/slang-gpu,$(PS5_CXX),-O2 -fPIC -march=znver2)
# mitrt's platform layer, without its thread wrapper (the player has its own __wrap_pthread_create: mpc_console.cpp,
# the same 2 MiB floor): open_memstream with its fclose / fflush wrappers, the C locale's *_l functions (libc++'s
# iostreams in glslang), qsort_r, fstatat & co, localtime_r & co, compiler-rt's emutls.c and x86.c (RADV's TLS)
PS5RT_PLATFORM_SRC := memstream stdio_wrap locale fs qsort_r misc
PS5RT_PLATFORM_OBJ := $(PS5RT_PLATFORM_SRC:%=build/app/ps5rt_platform/ps5rt_%.o) \
	build/app/ps5rt_platform/crt_emutls.o build/app/ps5rt_platform/crt_x86.o
build/app/ps5rt_platform/ps5rt_%.o: mitrt/platform/src/ps5rt_%.c mitrt/platform/include/ps5rt/platform.h
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -Imitrt/platform/include -c -o $@ $<
build/app/ps5rt_platform/crt_%.o: mitrt/platform/third_party/compiler-rt-18.1.8/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 -O2 -ffunction-sections -fdata-sections -c -o $@ $<
build/app/libps5rt_platform.a: $(PS5RT_PLATFORM_OBJ)
	rm -f $@
	$(PS5_PAYLOAD_SDK)/bin/llvm-ar rcsD $@ $(PS5RT_PLATFORM_OBJ)
# the C locale's plain entry points on the platform layer's table (ProsperoCtype.c): libc.a's no-locale.o, which has
# them, also has the *_l functions the platform layer gives, and both can't be linked
build/app/ProsperoCtype.o: coreorbis/orbis-shims/ProsperoCtype.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -c -o $@ $<
# libbluray and Mesa (RADV's ralloc) both export gc_free: the GPU build links a copy of libbluray with its own renamed
# (bd_gc_free, the definition and libbluray's calls alike); the deps build is left as it is
build/app/libbluray-gpu.a: $(DEPS_PS5)/lib/libbluray.a
	@mkdir -p $(dir $@)
	$(PS5_PAYLOAD_SDK)/bin/llvm-objcopy --redefine-sym gc_free=bd_gc_free $< $@
ifeq ($(GPU),1)
# RADV already embeds zlib; using a second archive would define its public symbols twice with --whole-archive.
FF_PS5_LINK_ARCHIVES = $(patsubst $(DEPS_PS5)/lib/libbluray.a,build/app/libbluray-gpu.a,$(filter-out $(DEPS_PS5)/lib/libz.a,$(FF_PS5_ARCHIVES)))
# Mesa's dispatch tables use weak references: an ordinary archive link leaves implemented Vulkan functions null.
# Keep every RADV member, as PS5_Vulkan/VLC's radv_link_recipe does.
GPU_LINK_INPUTS = $(PS5_GPU_OBJ) build/app/ProsperoCtype.o --whole-archive $(SLANG_GPU_RADV_ARCHIVE) --no-whole-archive build/app/libps5rt_platform.a
GPU_LINK_FLAGS := --wrap=fclose --wrap=fflush
GPU_IMPORT_ALLOW = $(SDK_LIB)/libSceAgc.so $(SDK_LIB)/libSceAgcDriver.so
else
FF_PS5_LINK_ARCHIVES = $(FF_PS5_ARCHIVES)
GPU_LINK_INPUTS = $(PS5_GPU_STUB_OBJ)
GPU_LINK_FLAGS :=
GPU_IMPORT_ALLOW :=
endif
$(SLANG_GPU_RADV_ARCHIVE):
	@echo "RADV built against the MIT ps5platform headers is needed: $(SLANG_GPU_RADV_ARCHIVE)"
	@echo "  (mitrt/platform/radv-shim/build-radv-mit.sh; or SLANG_GPU_RADV_MIT=<its folder>, or GPU=0 for the CPU only)"
	@exit 1

FF_PS5_ARCHIVES = $(addprefix $(DEPS_PS5)/lib/,libavformat.a libavcodec.a libavfilter.a libswscale.a libswresample.a \
	libavutil.a libdav1d.a libsmb2.a libmbedtls.a libmbedx509.a libmbedcrypto.a libz.a \
	libass.a libharfbuzz.a libfribidi.a libfreetype.a libdvdnav.a libdvdread.a libbluray.a libudfread.a)
EBOOT_OBJS = build/app/app_crt.o build/app/app_cpp_runtime.o $(PS5_EMU_OBJ) $(PS5_OUT)/helper_data.o
# the console's video decoder: the SDK has no stub for it, so one is made (stubs/videodec2_stub.c); it and
# libSceSysmodule are linked before --as-needed, since the player only refers to them weakly (mpc_vdec.cpp)
VDEC_STUB := build/app/stubs/libSceVideodec2.so
$(VDEC_STUB): stubs/videodec2_stub.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=c11 -O2 -fPIC -c -o $(dir $@)videodec2_stub.o $<
	$(PS5_PAYLOAD_SDK)/bin/prospero-lld --shared -soname libSceVideodec2.sprx -o $@ $(dir $@)videodec2_stub.o
# 1.5.2: the C++ runtime is the eboot's own, whole: libc++, libc++abi (__cxa_throw, the personality routine, the
# type_info vtables dynamic_cast walks) and libunwind (which finds the FDEs through ehframe.ld's __eh_frame_hdr_start),
# named before the system stubs. Up to 1.5.1 they came after them, and libSceLibcInternal's stub won
# __gxx_personality_v0, __cxa_get_globals and the type_info / std exception vtables while __cxa_throw and libunwind were
# static: a throw would run the system's personality routine on the static unwinder's context (Stella PS5 1.0 crashed
# that way at its first throw, starting any game). tools/check_imports.py refuses any imported C++ runtime symbol.
# 1.6: RADV, the GPU renderer and mitrt's platform layer after the player's archives (GPU=1, the default); check_imports
# takes AGC's two modules, RADV's.
# tools/check_syscalls.py (as FBNeo PS5 / Snes9x PS5): the link traces (-y) every function of the SDK's
# libc.a(syscalls.o), the payload SDK's bare system calls, and fails when any of them is linked into the app (1.5.4's
# took getfsstat from it for libbluray's mount table: a raw system call in a title, which the console may refuse);
# mitrt's check_link.py syscalls checks the same trace its own way (syscalls.o not loaded at all).
build/app/mpc-pie.elf: $(EBOOT_OBJS) $(filter-out --whole-archive --no-whole-archive,$(GPU_LINK_INPUTS)) $(FF_PS5_LINK_ARCHIVES) $(VDEC_STUB) $(NATIVE)/tooling/native/ps5-pie.ld \
		$(NATIVE)/tooling/native/ehframe.ld $(NATIVE)/tooling/native/app-symbols.map $(NATIVE)/tooling/native/localize.map \
		tools/check_imports.py tools/check_syscalls.py tools/embed_imports.py tools/check_radv_link.py
	python3 tools/check_syscalls.py rsp $(SDK_LIB)/libc.a build/app/syscalls-y.rsp
	$(LLD) -m elf_x86_64 -pie -z max-page-size=0x4000 --hash-style=gnu \
		-T $(NATIVE)/tooling/native/ps5-pie.ld -T $(NATIVE)/tooling/native/ehframe.ld --eh-frame-hdr \
		--version-script $(NATIVE)/tooling/native/app-symbols.map --version-script $(NATIVE)/tooling/native/localize.map \
		-e _start --no-dynamic-linker --wrap=getaddrinfo --wrap=pthread_create --wrap=sceVideoOutOpen --wrap=sceVideoOutClose $(GPU_LINK_FLAGS) -o $@ $(EBOOT_OBJS) \
		--start-group $(FF_PS5_LINK_ARCHIVES) --end-group $(GPU_LINK_INPUTS) \
		$(SDK_LIB)/libc++.a $(SDK_LIB)/libc++abi.a $(SDK_LIB)/libunwind.a \
		$(VDEC_STUB) $(SDK_LIB)/libSceSysmodule.so --as-needed $(filter-out %/libSceSysmodule.so,$(APP_STUBS)) -L $(SDK_LIB) \
		$(SDK_LIB)/libc.a -lpthread @build/app/syscalls-y.rsp --trace >build/app/mpc-pie.trace
	python3 tools/check_imports.py $@ $(SDK_LIB) $(VDEC_STUB) $(GPU_IMPORT_ALLOW) || { rm -f $@; exit 1; }
	python3 tools/check_syscalls.py check build/app/mpc-pie.trace || { rm -f $@; exit 1; }
	python3 tools/embed_imports.py $@ $(SDK_LIB) $(VDEC_STUB) || { rm -f $@; exit 1; }
	python3 mitrt/platform/tools/check_link.py syscalls build/app/mpc-pie.trace || { rm -f $@; exit 1; }
	@if [ "$(GPU)" = 1 ]; then python3 tools/check_radv_link.py $@ $(SLANG_GPU_RADV_ARCHIVE) || { rm -f $@; exit 1; }; fi
	@if [ "$(GPU)" = 1 ]; then grep -q '/slang-gpu/lib/gpu\.o$$' build/app/mpc-pie.trace && \
		grep -q 'libvulkan_radeon\.ps5\.a(' build/app/mpc-pie.trace || { echo "$@: the GPU renderer or RADV isn't linked"; rm -f $@; exit 1; }; \
		grep -q "MIT ps5platform headers" $(SLANG_GPU_RADV_MIT)/PROVENANCE.txt || { echo "$(SLANG_GPU_RADV_ARCHIVE): not the MIT-headers build"; rm -f $@; exit 1; }; fi

build/app/eboot.elf: build/app/mpc-pie.elf $(NATIVE_TOOL)
	$(NATIVE_TOOL) link --in $< --out $@ --stub-dir $(SDK_LIB) --stub $(VDEC_STUB) --module-sdk 0x02000009 \
		--companion-sdk 0x08050001 --file-name eboot.elf
	python3 tools/prepare_console_elf.py --input $@ --output build/app/eboot-console-ready.elf

$(APP_OUT)/eboot.bin: build/app/eboot.elf $(NATIVE_TOOL)
	@mkdir -p $(dir $@)
	$(NATIVE_TOOL) self --sign --in $< --out $@ --magic 0x1D3D154F
	$(NATIVE_TOOL) self --inspect --file $@

$(APP_OUT)/sce_sys/param.json: app/sce_sys/param.json tools/check_app_profile.py
	@mkdir -p $(dir $@)
	sed 's/PPSA99015/$(TITLE_ID)/g' $< > $@
	python3 tools/check_app_profile.py $@

$(APP_OUT)/sce_sys/%.png: app/sce_sys/%.png
	@mkdir -p $(dir $@)
	cp $< $@

# pic0.dds (the home screen's background while the icon is selected) and pic1.dds (the launch background):
# 1920x1080 BC7_UNORM DX10 DDS without mipmaps, made from app/sce_sys/background-source.png with
# tools/make_dds.py (bc7enc_rdo).
$(APP_OUT)/sce_sys/%.dds: app/sce_sys/%.dds
	@mkdir -p $(dir $@)
	cp $< $@

# ---------------- host ----------------
HOST_CC ?= gcc
HOST_CXX ?= g++
HOST_OUT := build/host
# the app's build of ProsperoAppPlace.cpp includes it (explicit targets: a recipe-less pattern rule adds nothing)
$(PS5_OUT)/port/coreorbis/orbis-shims/ProsperoAppPlaceApp.o $(HOST_OUT)/port/coreorbis/orbis-shims/ProsperoAppPlaceApp.o: \
	coreorbis/orbis-shims/ProsperoAppPlace.cpp
HOST_SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer
HOST_OPT := -O1 -g -mavx2 -fno-strict-aliasing
HOST_DEP_INCS := -isystem $(DEPS_HOST)/include
HOST_FLAGS_TEXT = $(strip $(HOST_CXX) $(HOST_CC) $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(HOST_DEP_INCS) $(PORT_WARN))
$(eval $(call FLAGS_STAMP,build/host,HOST_FLAGS_TEXT))

HOST_EMU_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(EMU_SRC) host/sce_host.cpp host/sce_host_vdec.cpp) \
	$(patsubst %.c,$(HOST_OUT)/port/%.o,$(MONO_SRC))
# 1.6: the GPU renderer on the host's Vulkan driver (lavapipe), ASan / UBSan on the library as on the player
HOST_GPU_OBJ := $(call SLANG_GPU_OBJECTS,$(HOST_OUT)/slang-gpu,$(HOST_CXX),$(HOST_OPT) $(HOST_SAN),-O2 -g1)
HOST_HELPER_OBJ := $(HOST_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(HELPER_SRC) host/sce_host.cpp)
HOST_INSTALLER_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(INSTALLER_SRC) host/sce_host.cpp) \
	$(patsubst %.c,$(HOST_OUT)/port/%.o,$(MONO_SRC))

host: $(HOST_OUT)/mpc-ps5-host $(HOST_OUT)/mpc-ps5-installer $(HOST_OUT)/mpc-ps5-helper $(HOST_OUT)/mpc-disc-test

$(HOST_OUT)/port/%.o: %.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h player/*.h $(HOST_OUT)/flags.txt
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(HOST_DEP_INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(HOST_OUT)/port/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c99 $(HOST_OPT) $(HOST_SAN) -c -o $@ $<

$(HOST_OUT)/port/installer/helper_main.o: installer/installer_main.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h \
		$(HOST_OUT)/flags.txt
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

# The host installer carries a stand-in app folder (tests/fake-eboot.bin, tests/fake-libc.prx + the real
# param/icon/pic); the host app carries the host helper, which it hands to a fake ELF loader in the tests.
# (the backgrounds compressed, as in the payload, so the tests inflate them)
HOST_APP_DEFS = -DAPP_EBOOT='"$(abspath tests/fake-eboot.bin)"' -DAPP_PARAM='"$(abspath app/sce_sys/param.json)"' \
	-DAPP_ICON0='"$(abspath app/sce_sys/icon0.png)"' -DAPP_PIC0='"$(abspath build/zipped-host/pic0.dds.z)"' \
	-DAPP_PIC1='"$(abspath build/zipped-host/pic1.dds.z)"' \
	-DAPP_LIBC='"$(abspath tests/fake-libc.prx)"' \
	-DAPP_PIC0_SIZE=$$(stat -c %s app/sce_sys/pic0.dds) -DAPP_PIC1_SIZE=$$(stat -c %s app/sce_sys/pic1.dds)
build/zipped-host/%.z: app/sce_sys/%
	@mkdir -p $(dir $@)
	python3 -c "import sys, zlib; open(sys.argv[2], 'wb').write(zlib.compress(open(sys.argv[1], 'rb').read(), 9))" $< $@
$(HOST_OUT)/port/coreorbis/orbis-shims/ProsperoInstall.o: EXTRA_FLAGS = $(HOST_DEP_INCS)

$(HOST_OUT)/install_data.o: coreorbis/orbis-shims/install_data.cpp coreorbis/orbis-shims/ProsperoInstall.h $(HOST_OUT)/flags.txt \
		tests/fake-eboot.bin tests/fake-libc.prx app/sce_sys/param.json app/sce_sys/icon0.png build/zipped-host/pic0.dds.z build/zipped-host/pic1.dds.z
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(HOST_APP_DEFS) -c -o $@ $<

$(HOST_OUT)/helper_data.o: coreorbis/orbis-shims/helper_data.cpp coreorbis/orbis-shims/ProsperoJailbreak.h $(HOST_OUT)/mpc-ps5-helper \
		$(HOST_OUT)/flags.txt
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(DEFS) $(INCS) $(PORT_WARN) -DHELPER_ELF='"$(abspath $(HOST_OUT)/mpc-ps5-helper)"' -c -o $@ $<

$(HOST_OUT)/mpc-ps5-helper: $(HOST_HELPER_OBJ)
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread

$(HOST_OUT)/mpc-ps5-installer: $(HOST_INSTALLER_OBJ) $(HOST_OUT)/install_data.o
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ $(DEPS_HOST)/lib/libz.a -lpthread

$(HOST_OUT)/mpc-ps5-host: $(HOST_EMU_OBJ) $(HOST_OUT)/helper_data.o $(HOST_GPU_OBJ)
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -L$(DEPS_HOST)/lib $(FF_LIBS) -lvulkan -lpthread -lm -ldl

$(HOST_OUT)/mpc-theme-test: tests/theme_test.cpp $(filter-out $(HOST_OUT)/port/coreorbis/main-boot.o,$(HOST_EMU_OBJ)) $(HOST_OUT)/helper_data.o $(HOST_GPU_OBJ)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(HOST_DEP_INCS) $(PORT_WARN) -o $@ $^ \
		-L$(DEPS_HOST)/lib $(FF_LIBS) -lvulkan -lpthread -lm -ldl

test-theme: $(HOST_OUT)/mpc-theme-test
	@mkdir -p build/validation/theme/root build/validation/theme/screenshots
	MPCPS5_PS5_ROOT='$(abspath build/validation/theme/root)' THEME_TEST_DUMP_DIR='$(abspath build/validation/theme/screenshots)' \
		ASAN_OPTIONS=detect_leaks=0 $(HOST_OUT)/mpc-theme-test

# player/mpc_disc.cpp on its own, with faults put into the drive (tests/disc_test.cpp)
$(HOST_OUT)/mpc-disc-test: tests/disc_test.cpp player/mpc_disc.cpp player/mpc_disc.h player/mpc_i18n.h $(HOST_OUT)/flags.txt
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(HOST_DEP_INCS) $(PORT_WARN) -o $@ $< \
		-L$(DEPS_HOST)/lib $(FF_LIBS) -lpthread -lm -ldl

test-audit:
	sh tests/run-update-transaction-tests.sh
	sh tests/run-log-password-tests.sh
	sh tests/run-install-content-tests.sh
	sh tests/run-gpu-startup-tests.sh
	sh tests/run-vdec-memory-tests.sh
	sh tests/run-console-hdr-tests.sh

$(HOST_OUT)/mpc-hdr-test: tests/hdr_output_test.cpp $(HOST_GPU_OBJ)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(SLANG_GPU_INC) -o $@ $< $(HOST_GPU_OBJ) -lvulkan -lpthread

$(HOST_OUT)/mpc-gpu-library-test: $(SLANG_GPU)/tests/gpu_test.cpp $(HOST_GPU_OBJ)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(SLANG_GPU_WARN) $(SLANG_GPU_INC) -o $@ $< $(HOST_GPU_OBJ) -lvulkan -lpthread

test: host test-audit test-theme
	bash tests/run-tests.sh

# 1.6: the CPU's work per picture, 1.5.4's path against the GPU's (tests/gpu_bench.cpp; optimised, no sanitizers):
#   make bench && build/bench/mpc-gpu-bench <clip> [pictures] [out WxH]
BENCH_OUT := build/bench
BENCH_GPU_OBJ := $(call SLANG_GPU_OBJECTS,$(BENCH_OUT)/slang-gpu,$(HOST_CXX),-O2 -g1 -mavx2,-O2 -g1)
$(BENCH_OUT)/mpc-gpu-bench: tests/gpu_bench.cpp player/mpc_tonemap.cpp player/mpc_tonemap.h $(BENCH_GPU_OBJ)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 -O2 -mavx2 $(DEFS) $(INCS) $(HOST_DEP_INCS) -o $@ tests/gpu_bench.cpp player/mpc_tonemap.cpp \
		$(BENCH_GPU_OBJ) -L$(DEPS_HOST)/lib $(FF_LIBS) -lvulkan -lpthread -lm -ldl
bench: $(BENCH_OUT)/mpc-gpu-bench

clean:
	rm -rf build/ps5 build/host build/app build/dist build/bench build/gen

.PHONY: ps5 host test test-audit test-theme send app dist clean deps deps-subs deps-disc bench
.SECONDARY:
