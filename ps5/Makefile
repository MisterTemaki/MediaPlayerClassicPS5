# Media Player Classic PS5
#   make deps   -> build/deps/prefix-{ps5,host}   FFmpeg, libsmb2, Mbed TLS, zlib (tools/build-deps.sh; once)
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
# $(PS5_PAYLOAD_SDK), with clang/lld 18 on the host, nasm, git (tools/build-deps.sh fetches FFmpeg n7.1.1 and
# libsmb2).

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_HOST ?= ps5
PS5_PORT ?= 9021
VERSION ?= 1.1.1
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
	coreorbis/orbis-shims/ProsperoInput.cpp coreorbis/orbis-shims/ProsperoJailbreak.cpp \
	coreorbis/orbis-shims/ProsperoCrash.cpp coreorbis/orbis-shims/ProsperoCrt.cpp \
	player/mpc_app.cpp player/mpc_dialogs.cpp player/mpc_font.cpp player/mpc_gdi.cpp player/mpc_menu.cpp \
	player/mpc_player.cpp player/mpc_res.cpp player/mpc_settings.cpp player/mpc_smb.cpp player/mpc_ui.cpp \
	player/mpc_console.cpp player/mpc_vdec.cpp player/mpc_tonemap.cpp player/mpc_update.cpp player/mpc_i18n.cpp \
	coreorbis/orbis-shims/ProsperoUpdate.cpp
# the payloads: the helper, and the installer (= helper + install)
HELPER_SRC := coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoNotify.cpp \
	coreorbis/orbis-shims/ProsperoHelper.cpp coreorbis/orbis-shims/ProsperoCrash.cpp
INSTALLER_SRC := installer/installer_main.cpp $(HELPER_SRC) coreorbis/orbis-shims/ProsperoInstall.cpp \
	coreorbis/orbis-shims/ProsperoUpdate.cpp

INCS := -Icoreorbis/include-orbis -Icoreorbis/orbis-shims -Iplayer -isystem third_party/stb -isystem third_party/nanosvg
DEFS := -DMPCPS5_PS5_VERSION='"$(VERSION)"' -DMPCPS5_TITLE_ID='"$(TITLE_ID)"' -DMPCPS5_UPDATE_REPO='"$(UPDATE_REPO)"' -DMPC_ASSET_DIR='"$(abspath assets)"' \
	-DORBIS_ROOT_DEFAULT='"/data/mpcps5"'
PORT_WARN := -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers
FF_LIBS = -lavformat -lavcodec -lavfilter -lswscale -lswresample -lavutil -ldav1d -lsmb2 -lmbedtls -lmbedx509 -lmbedcrypto -lz

# stb and NanoSVG inside mpc_res.o / mpc_font.o have warnings of their own; the resources are built in
%/port/player/mpc_res.o: EXTRA_FLAGS = -Wno-error -Wno-sign-compare -Wno-unused-function -Wno-unused-but-set-variable \
	-Wno-implicit-fallthrough -Wno-missing-field-initializers
%/port/player/mpc_res.o: $(wildcard assets/res/* assets/fonts/*)
%/port/player/mpc_font.o: EXTRA_FLAGS = -Wno-error -Wno-sign-compare -Wno-unused-function
%/port/player/mpc_app.o: assets/data/cacert.pem
# the helper payload is installer_main.cpp without the install step
%/port/installer/helper_main.o: EXTRA_FLAGS = -DMPCPS5_HELPER_ONLY

# ---------------- PS5 ----------------
PS5_CC  := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_CXX := $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
PS5_OUT := build/ps5
# The PS5's CPU is Zen 2.
PS5_OPT := -O3 -fPIC -march=znver2 -fno-strict-aliasing
PS5_PAYLOAD_LIBS := -lkernel_sys -lSceNet -lSceNetCtl -lSceSsl -lSceHttp2 -lpthread
PS5_DEP_INCS := -isystem $(DEPS_PS5)/include

PS5_EMU_OBJ := $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(EMU_SRC))
PS5_HELPER_OBJ := $(PS5_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(HELPER_SRC))
PS5_INSTALLER_OBJ := $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(INSTALLER_SRC))

ps5: $(PS5_OUT)/MediaPlayerClassicPS5.elf

deps:
	bash tools/build-deps.sh

$(PS5_OUT)/port/%.o: %.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h player/*.h
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PS5_DEP_INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(PS5_OUT)/port/installer/helper_main.o: installer/installer_main.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

# 1. the helper payload (built into the app, which sends it to the ELF loader when no helper answers)
$(PS5_OUT)/MediaPlayerClassicPS5-helper.elf: $(PS5_HELPER_OBJ)
	$(PS5_CXX) -o $@ $^ $(PS5_PAYLOAD_LIBS)

$(PS5_OUT)/helper_data.o: coreorbis/orbis-shims/helper_data.cpp coreorbis/orbis-shims/ProsperoJailbreak.h $(PS5_OUT)/MediaPlayerClassicPS5-helper.elf
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

$(PS5_OUT)/install_data.o: coreorbis/orbis-shims/install_data.cpp coreorbis/orbis-shims/ProsperoInstall.h $(APP_FILES) \
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
APP_STUBS = $(filter-out %/libScePosixForWebKit.so %/libSceGLSlimVSH.so,$(sort $(wildcard $(SDK_LIB)/*.so)))

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
	cd build/app && sha256sum --check --strict $(abspath $(NATIVE)/libc.prx.sha256)

$(APP_OUT)/sce_module/libc.prx: build/app/libc.prx
	@mkdir -p $(dir $@)
	cp $< $@

app: $(APP_FILES)

build/app/app_crt.o build/app/app_cpp_runtime.o: build/app/%.o: $(NATIVE)/tooling/native/%.cpp
	@mkdir -p $(dir $@)
	$(NATIVE_CLANG) -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
		-c -o $@ $<

FF_PS5_ARCHIVES = $(addprefix $(DEPS_PS5)/lib/,libavformat.a libavcodec.a libavfilter.a libswscale.a libswresample.a \
	libavutil.a libdav1d.a libsmb2.a libmbedtls.a libmbedx509.a libmbedcrypto.a libz.a)
EBOOT_OBJS = build/app/app_crt.o build/app/app_cpp_runtime.o $(PS5_EMU_OBJ) $(PS5_OUT)/helper_data.o
# the console's video decoder: the SDK has no stub for it, so one is made (stubs/videodec2_stub.c); it and
# libSceSysmodule are linked before --as-needed, since the player only refers to them weakly (mpc_vdec.cpp)
VDEC_STUB := build/app/stubs/libSceVideodec2.so
$(VDEC_STUB): stubs/videodec2_stub.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=c11 -O2 -fPIC -c -o $(dir $@)videodec2_stub.o $<
	$(PS5_PAYLOAD_SDK)/bin/prospero-lld --shared -soname libSceVideodec2.sprx -o $@ $(dir $@)videodec2_stub.o
build/app/mpc-pie.elf: $(EBOOT_OBJS) $(FF_PS5_ARCHIVES) $(VDEC_STUB) $(NATIVE)/tooling/native/ps5-pie.ld $(NATIVE)/tooling/native/ehframe.ld \
		$(NATIVE)/tooling/native/app-symbols.map $(NATIVE)/tooling/native/localize.map
	$(LLD) -m elf_x86_64 -pie -z max-page-size=0x4000 --hash-style=gnu \
		-T $(NATIVE)/tooling/native/ps5-pie.ld -T $(NATIVE)/tooling/native/ehframe.ld --eh-frame-hdr \
		--version-script $(NATIVE)/tooling/native/app-symbols.map --version-script $(NATIVE)/tooling/native/localize.map \
		-e _start --no-dynamic-linker --wrap=getaddrinfo --wrap=pthread_create -o $@ $(EBOOT_OBJS) \
		--start-group $(FF_PS5_ARCHIVES) --end-group \
		$(VDEC_STUB) $(SDK_LIB)/libSceSysmodule.so --as-needed $(filter-out %/libSceSysmodule.so,$(APP_STUBS)) -L $(SDK_LIB) \
		$(SDK_LIB)/libc++.a $(SDK_LIB)/libc++abi.a $(SDK_LIB)/libunwind.a $(SDK_LIB)/libc.a -lpthread

build/app/eboot.elf: build/app/mpc-pie.elf $(NATIVE_TOOL)
	$(NATIVE_TOOL) link --in $< --out $@ --stub-dir $(SDK_LIB) --stub $(VDEC_STUB) --module-sdk 0x02000009 \
		--companion-sdk 0x08050001 --file-name eboot.elf
	python3 tools/prepare_console_elf.py --input $@ --output build/app/eboot-console-ready.elf

$(APP_OUT)/eboot.bin: build/app/eboot.elf $(NATIVE_TOOL)
	@mkdir -p $(dir $@)
	$(NATIVE_TOOL) self --sign --in $< --out $@ --magic 0x1D3D154F
	$(NATIVE_TOOL) self --inspect --file $@

$(APP_OUT)/sce_sys/param.json: app/sce_sys/param.json
	@mkdir -p $(dir $@)
	sed 's/PPSA99015/$(TITLE_ID)/g' $< > $@

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
HOST_SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer
HOST_OPT := -O1 -g -mavx2 -fno-strict-aliasing
HOST_DEP_INCS := -isystem $(DEPS_HOST)/include

HOST_EMU_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(EMU_SRC) host/sce_host.cpp host/sce_host_vdec.cpp)
HOST_HELPER_OBJ := $(HOST_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(HELPER_SRC) host/sce_host.cpp)
HOST_INSTALLER_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(INSTALLER_SRC) host/sce_host.cpp)

host: $(HOST_OUT)/mpc-ps5-host $(HOST_OUT)/mpc-ps5-installer $(HOST_OUT)/mpc-ps5-helper

$(HOST_OUT)/port/%.o: %.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h player/*.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(HOST_DEP_INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(HOST_OUT)/port/installer/helper_main.o: installer/installer_main.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h
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

$(HOST_OUT)/install_data.o: coreorbis/orbis-shims/install_data.cpp coreorbis/orbis-shims/ProsperoInstall.h \
		tests/fake-eboot.bin tests/fake-libc.prx app/sce_sys/param.json app/sce_sys/icon0.png build/zipped-host/pic0.dds.z build/zipped-host/pic1.dds.z
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(HOST_APP_DEFS) -c -o $@ $<

$(HOST_OUT)/helper_data.o: coreorbis/orbis-shims/helper_data.cpp coreorbis/orbis-shims/ProsperoJailbreak.h $(HOST_OUT)/mpc-ps5-helper
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(DEFS) $(INCS) $(PORT_WARN) -DHELPER_ELF='"$(abspath $(HOST_OUT)/mpc-ps5-helper)"' -c -o $@ $<

$(HOST_OUT)/mpc-ps5-helper: $(HOST_HELPER_OBJ)
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread

$(HOST_OUT)/mpc-ps5-installer: $(HOST_INSTALLER_OBJ) $(HOST_OUT)/install_data.o
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ $(DEPS_HOST)/lib/libz.a -lpthread

$(HOST_OUT)/mpc-ps5-host: $(HOST_EMU_OBJ) $(HOST_OUT)/helper_data.o
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -L$(DEPS_HOST)/lib $(FF_LIBS) -lpthread -lm -ldl

test: host
	bash tests/run-tests.sh

clean:
	rm -rf build/ps5 build/host build/app build/dist

.PHONY: ps5 host test send app dist clean deps
.SECONDARY:
