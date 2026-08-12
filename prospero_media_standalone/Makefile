# ProsperoPlayer Media home launcher
# core/ = websrv BigApp path (John Törnblom, GPL-3.0+)
# orchestration = ProsperoPlayer original

SDK ?= /opt/ps5-payload-sdk
CC := $(SDK)/bin/prospero-clang
STRIP := $(SDK)/bin/prospero-strip

ELF := ProsperoPlayer_MediaLauncher.elf
UNINSTALL_ELF := ProsperoPlayer_UninstallTile.elf

SRCS := \
	prospero_media_launcher.c \
	core/hbldr.c \
	core/elfldr.c \
	core/pt.c

CFLAGS := -Os -Wall -Wno-unused-parameter \
	-I. -Icore \
	-I$(SDK)/target/include

LIBS := \
	-lSceIpmi \
	-lSceUserService \
	-lSceAppInstUtil \
	-lSceSystemService \
	-lSceNet \
	-lpthread

all: $(ELF) $(UNINSTALL_ELF)

$(ELF): $(SRCS) assets/ProsperoPlayer.elf assets/param.json assets/icon0.png
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS)
	$(STRIP) $@
	@sha256sum $@
	@ls -lh $@

$(UNINSTALL_ELF): prospero_uninstall_tile.c
	$(CC) $(CFLAGS) -o $@ prospero_uninstall_tile.c $(LIBS)
	$(STRIP) $@
	@sha256sum $@
	@ls -lh $@

clean:
	rm -f $(ELF) $(UNINSTALL_ELF)

.PHONY: all clean
