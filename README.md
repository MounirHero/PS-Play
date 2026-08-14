# PS Play

**All-in-one media hub for jailbroken PlayStation 5** — DLNA receiver, DLNA/UPnP browser, native Stremio client, IPTV/Live TV and USB playback in a single native app with a minimal AMOLED-black UI.

> **Credits:** InsideMatrixDev / MounirHero  
> Based on [Prospero Player 1.0](https://github.com/KINGDKAK/ProsperoPlayer) by KINGDKAK (GPL-3.0)  
> Stremio integration ported from **Stremio Portable** by **stuey-81**

---

## Features

### Four ways to watch
- **DLNA Hub** — UPnP MediaRenderer:1 cast target (find "PS Play" in BubbleUPnP or any DLNA controller) plus automatic discovery and browsing of LAN media servers, all in one menu.
- **IPTV / Live TV** — reads `.m3u` / `.m3u8` from USB (all ports usb0-usb3 in one list) and `/data/PS Play`, custom URL entry, built-in public HLS test channels. Dedicated channel list with TRIANGLE search and SQUARE reload (up to 2500 channels).
- **Stremio** — native addon client (Nuvio-style), no web engine. Movies & series with disk-cached posters and episode lists. Load addons via `Stremio_addons.txt` on USB or `/data/PS Play`, or enter manifests manually. Catalogs come from catalog-capable addons; streams are aggregated from **all** installed stream addons.
- **USB & Local** — browse all USB ports in a single list, recent files with resume position and restart prompt.

### Built for the couch
- **AMOLED pure-black UI** — horizontal icon-row menu, grayscale palette (black / gray / white), selected tile grows with a bright white outline.
- **Native PS5 system keyboard** — sceImeDialog for every text field; full symbols and all languages. Auto-reopens on invalid input; OPTIONS reopens manually.
- **Left analog stick navigation** — works like the d-pad with continuous auto-scroll when held.
- **Clean notifications** — only essential toasts ("PS Play READY", "CASTING", errors). Debug spam removed.
- **Accidental-press-proof** — during playback only X, D-pad LEFT/RIGHT, OPTIONS and CIRCLE are active; everything else lives inside the OPTIONS dialog.

### Playback engineering
- **FFmpeg-powered** — H.264, HEVC, AV1, VP9, MPEG-2/4, AAC, AC3, E-AC3, DTS, TrueHD, ALAC, FLAC, Opus, Vorbis, WMA, PCM. Protocols: HTTP/S, HLS, RTSP, RTP, UDP, RTMP, MMSH.
- **Network anti-freeze** — 8 MB socket buffer, 384-packet demux queues, PTS discontinuity re-anchoring (HLS restarts / source switches), auto-resync on >3s desync, deadlock prevention (stalled audio dropped to keep video flowing), 1.5s post-underrun buffer, auto-reconnect on 4xx/5xx.
- **Fast seeking** — 16 MB short-seek cache serves ±10s seeks from RAM without reopening the stream. Decoder threads park before touching codec state; the last frame stays visible instead of flashing black.
- **Subtitles & audio** — external SRT, embedded tracks, per-track selection, ±100ms delay nudging. Multiple audio tracks, aspect ratio (Fit / Fill / Stretch), media info panel, optional stats overlay.
- **Software volume** — 0-150%, also driven by DLNA SetVolume.

---

## Menu layout (7 entries)

**DLNA · IPTV / LIVE TV · WEB BROWSER · BROWSE USB · RECENT FILES · SETTINGS · ABOUT**

Navigate with **LEFT/RIGHT** or the **left analog stick**. **X** to open, **CIRCLE** back.  
CIRCLE on the main menu asks for exit confirmation (X quits, O cancels).

---

## Controls

| Context | Input | Action |
|---|---|---|
| Menu | D-pad / Left stick | Navigate |
| Menu | X / CIRCLE | Select / Back |
| Playback | X | Play / Pause |
| Playback | LEFT / RIGHT | Seek ±10s (hold to accelerate, X confirms) |
| Playback | OPTIONS | Playback settings dialog |
| Playback | CIRCLE | Stop and return (with "EXIT PLAYBACK?" confirmation) |
| OPTIONS dialog | UP / DOWN | Navigate rows |
| OPTIONS dialog | X, LEFT / RIGHT | Activate / adjust value |
| Text fields | — | PS5 system keyboard |

**OPTIONS playback dialog:** subtitles on/off, subtitle track, subtitle delay, audio track, software volume, aspect ratio, media info, statistics.

---

## Requirements

- An active jailbreak and a payload sender (port 9021)
- Like all homebrew on this firmware, **the ELF must be injected again after each reboot**

---

## Installation

1. Jailbreak your PS5 and start the payload listener.
2. Inject **`PSPlay.elf`** (the installer).
3. Watch the notifications: `step 3 ok` → `step 4 ok` → `step 6 ok` → `PS Play ready — Open from Media`.
4. Open **PS Play** from the Media section of the home screen.

> `PSPlay.elf` is the standalone player (useful for quick tests) — it does **not** install the home-screen tile. Use the MediaLauncher for installation.

---

## Building from source

You need the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) with its pacbrew packages (SDL2, FFmpeg, OpenSSL, libiconv, zlib, bzip2, xz) and a host LLVM/clang toolchain (15+).

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
export PATH=$PS5_PAYLOAD_SDK/bin:$PATH

# 1. Player
make                        # produces PS5MediaPlayerPRO.elf
llvm-strip -o PSPlay.elf PS5MediaPlayerPRO.elf

# 2. Media launcher (embeds the player + assets)
cp PSPlay.elf prospero_media_standalone/assets/ProsperoPlayer.elf
cd prospero_media_standalone
make SDK=$PS5_PAYLOAD_SDK   # produces ProsperoPlayer_MediaLauncher.elf
```

---

## Tidier `/data`

All app data now lives in `/data/PS Play/` (log, resume state, web history, Stremio addons, poster cache) instead of flooding the system `/data` folder. Files from 2.0 are migrated automatically.

---

## License

GNU General Public License v3.0 (or later) — see [LICENSE](LICENSE).  
Based on [Prospero Player 1.0](https://github.com/KINGDKAK/ProsperoPlayer) by KINGDKAK (GPL-3.0) — see [NOTICE](NOTICE).

## Disclaimer

This is unofficial homebrew software, not affiliated with Sony or PlayStation. Use at your own risk on a console you own.
