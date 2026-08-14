PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := PS5MediaPlayerPRO.elf

# Progressive 4K product stages: 0=SAFE 1=G_VO 2=G_V8 3=G_AUDIO 4=H_FULL
# Default 4 after G-H stage ladder PASS (2026-07-21). Rollback: STAGE=0
STAGE ?= 4

CFLAGS := -Wall -g -O2 \
    -Ipp/include \
    -Icjson \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include/SDL2 \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include \
    -DPP_4K_PRODUCT_STAGE=$(STAGE)

LIBS := \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libSDL2.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavformat.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavcodec.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libswresample.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavutil.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libswscale.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libssl.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libcrypto.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libiconv.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libz.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libbz2.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/liblzma.a \
    -lSceNotification \
    -lSceSystemService \
    -lSceUserService \
    -lScePad \
    -lSceAudioOut \
    -lSceVideoOut \
    -lSceKeyboard \
    -lSceImeDialog \
    -lc++ -lc++abi -lpthread

PP_SRCS := \
    pp/src/pp_videoout.c \
    pp/src/pp_converter.c \
    pp/src/pp_converter_parallel.c \
    pp/src/pp_converter_fused.c \
    pp/src/pp_audio_clock.c \
    pp/src/pp_stage_breadcrumb.c \
    pp/src/pp_clock.c \
    pp/src/pp_playback.c \
    pp/src/tile_copy.c \
    cjson/cJSON.c \
    net/pp_http.c \
    net/pp_dlna.c \
    net/pp_iptv.c \
    net/pp_dmr.c

all: $(ELF)

$(ELF): main.c $(PP_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "STAGE=$(STAGE)"
	@sha256sum $@
	@ls -la $@

# Build all progressive stage ELFs into artifacts/
stage_all:
	@mkdir -p artifacts/4k_stages
	$(MAKE) STAGE=0 ELF=artifacts/4k_stages/PP_4K_STAGE0_SAFE.elf
	$(MAKE) STAGE=1 ELF=artifacts/4k_stages/PP_4K_STAGE1_G_VO.elf
	$(MAKE) STAGE=2 ELF=artifacts/4k_stages/PP_4K_STAGE2_G_V8.elf
	$(MAKE) STAGE=3 ELF=artifacts/4k_stages/PP_4K_STAGE3_G_AUDIO.elf
	$(MAKE) STAGE=4 ELF=artifacts/4k_stages/PP_4K_STAGE4_H_FULL.elf

clean:
	rm -f $(ELF)
	rm -rf artifacts/4k_stages
