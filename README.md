# PS Play 1.5

**All-in-one media hub for jailbroken PlayStation 5** — DLNA receiver (cast target), DLNA/UPnP browser, IPTV/Live TV and USB playback in a single native app with a minimal AMOLED-black UI.

PS Play is based on the open-source **[Prospero Player 1.0](https://github.com/KINGDKAK/ProsperoPlayer)** project by **KINGDKAK**, rebuilt and expanded with a new interface, a UPnP MediaRenderer, native system keyboard support and heavy network-streaming optimizations.

> **Credits:** InsideMatrixDev / MounirHero — based on [Prospero Player 1.0](https://github.com/KINGDKAK/ProsperoPlayer) by KINGDKAK (GPL-3.0)

---

## Features

### Four ways to watch, one app
- **DLNA Receiver** — turns your PS5 into a Chromecast-like target: full UPnP MediaRenderer:1 with play / pause / stop / seek / volume control, GENA eventing and position reporting. Find "PS Play" in BubbleUPnP or any DLNA controller and cast wirelessly.
- **DLNA / UPnP Browser** — automatic discovery of media servers on your LAN (NAS, PCs, routers) with full ContentDirectory browsing: folders, videos, music and metadata.
- **IPTV / Live TV** — reads `.m3u` / `.m3u8` playlists from USB (root, `/IPTV`, `/PLAYLISTS`), custom URL entry (http / HLS / rtmp / udp), built-in public HLS test channels.
- **USB Browser** — local playback from USB storage, with recent files list and resume playback.

### Built for the couch
- **AMOLED pure-black theme** across the whole interface — zero distractions, designed for TV.
- **Horizontal tile menu**: DLNA Receiver · DLNA/UPnP · IPTV/Live TV · Browse USB · Recent Files · Settings (with Developer Tools inside) · About.
- **Transparent playback overlay**: just the video, a single slim progress bar, the title and two hints — no panels, no backgrounds, no icon clutter.
- **Accidental-press-proof controls**: during playback only X, D-pad LEFT/RIGHT, OPTIONS and CIRCLE are active. Everything else lives inside the OPTIONS dialog.
- **Native PS5 system keyboard** (sceImeDialog) for all text fields — full symbols, all input languages. Opens automatically; built-in keyboard as fallback.

### Serious playback engineering
- FFmpeg-powered decoding: H.264 / HEVC up to 4K with adaptive multithreaded decoding tuned to the PS5 CPU.
- **Network streaming optimizations**: 4 MB socket receive buffer, 16 MB short-seek cache (±10 s seeks served from buffered data instead of reopening the HTTP stream), deep 256-packet demux-ahead queues that absorb Wi-Fi jitter, reduced probe window for fast stream startup.
- **Full subtitle support**: external SRT, embedded tracks, per-track selection, ±100 ms delay nudging.
- Multiple audio tracks, aspect ratio control (Fit / Fill / Stretch), media info panel, optional stats overlay.

## Requirements

- PlayStation 5 on firmware **4.00** (other umtx-compatible firmwares may work)
- An active jailbreak (umtx) and a payload sender (port 9021)
- Like all homebrew on this firmware, **the ELF must be injected again after each reboot**

## Installation

1. Jailbreak your PS5 and start the payload listener.
2. Inject **`PSPlay-1.5-MediaLauncher.elf`** (the installer).
3. Watch the notifications: `step 3 ok` → `step 4 ok` → `step 6 ok` → `PS Play 1.5 ready — Open from Media`.
4. Open **PS Play** from the Media section of the home screen.

> `PSPlay-1.5.elf` is the standalone player (useful for quick tests) — it does **not** install the home-screen tile. Use the MediaLauncher for installation.

## Controls

| Context | Input | Action |
|---|---|---|
| Menus | D-pad | Navigate |
| Menus | X / CIRCLE | Select / Back |
| Playback | X | Play / Pause |
| Playback | LEFT / RIGHT | Seek ±10 s (hold to accelerate, X confirms) |
| Playback | OPTIONS | Playback settings dialog |
| Playback | CIRCLE | Stop and go back |
| OPTIONS dialog | UP / DOWN | Navigate rows |
| OPTIONS dialog | X, LEFT / RIGHT | Activate / adjust value |
| Text fields | — | PS5 system keyboard (OPTIONS reopens) |

The **OPTIONS playback dialog** groups every secondary action: subtitles on/off, subtitle track, subtitle delay, audio track, aspect ratio, media info and statistics.

## Building from source

You need the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) with its
pacbrew packages (SDL2, FFmpeg, OpenSSL, libiconv, zlib, bzip2, xz) and a host
LLVM/clang toolchain (15+).

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
export PATH=$PS5_PAYLOAD_SDK/bin:$PATH

# 1. Player
make                        # produces PS5MediaPlayerPRO.elf
llvm-strip -o PSPlay-1.5.elf PS5MediaPlayerPRO.elf

# 2. Media launcher (embeds the player + assets)
cp PSPlay-1.5.elf prospero_media_standalone/assets/ProsperoPlayer.elf
cd prospero_media_standalone
make SDK=$PS5_PAYLOAD_SDK   # produces ProsperoPlayer_MediaLauncher.elf
```

## Project structure

```
main.c                        player core: UI, playback engine, input, OSD
pp/                           video output / conversion backend
net/                          HTTP, DLNA browser, DMR (cast receiver), IPTV
playback/                     playback profiles
storage/                      recent files, favorites
metadata/                     media metadata helpers
ui/                           about / developer screens
assets/                       fonts, icons, UI bitmaps
prospero_media_standalone/    media-tile launcher, installer and assets
test/                         DMR host-side tests
docs/                         launcher internals
```

## License

GNU General Public License v3.0 (or later) — see [LICENSE](LICENSE).
Based on **[Prospero Player 1.0](https://github.com/KINGDKAK/ProsperoPlayer)** by KINGDKAK (GPL-3.0) — see [NOTICE](NOTICE).

## Disclaimer

This is unofficial homebrew software, not affiliated with Sony or PlayStation.
Use at your own risk on a console you own.
