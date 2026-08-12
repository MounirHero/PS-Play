ProsperoPlayer 1.0 — Media home launcher (Prospero only)

WHAT THIS IS
  One resident payload that:
    1) installs a Media-section tile named ProsperoPlayer (title PRSP10001)
    2) keeps a loopback service on 127.0.0.1:9055
    3) launches the real player when you open the tile
    4) can fully uninstall the tile via /uninstall or UninstallTile.elf

  No third-party product names. No browser "ok" page when launch works.

CONSOLE USE
  1. Jailbreak (elfldr running)
  2. Inject ProsperoPlayer_MediaLauncher.elf
       → put on autoload so this is automatic every boot
  3. Wait for toast: "ProsperoPlayer 1.0 ready" / "Open from Media"
  4. Home → Media → ProsperoPlayer 1.0
  5. Leave the launcher payload running in the background

  After reboot: jailbreak + launcher inject (autoload recommended).
  Tapping the tile alone does nothing if the launcher is not resident.

SAFE WITHOUT TILE
  Inject the standalone player ELF only (no Media install).

UNINSTALL
  Preferred while launcher is running:
    Settings → Remove Home Tile (press X twice), or
    GET http://127.0.0.1:9055/uninstall
  Always works (even if launcher is not up):
    Inject ProsperoPlayer_UninstallTile.elf once

  Note: Sony often hides Options → Delete for Media BigApp hosts.
  If Delete works on your FW, fine; use UninstallTile if a ghost remains.

KNOWN SHELL BEHAVIOR
  - A brief "not supported" flash can still appear on some firmwares even
    when launch succeeds. Cosmetic; we answer HTTP 204 immediately and
    omit contentId on tile + host to minimize lock / unsupported.
  - No lock icon expected with current params (no contentId).
  - Launcher must be re-injected each jailbreak session.

IDENTITY
  Title ID:     PRSP10001
  Display name: ProsperoPlayer
  Port:         9055
  Runtime:      /data/homebrew/ProsperoPlayer
  Logs:         /data/prosperoplayer/media_launcher.log
                /data/prosperoplayer/player-stdio.log

BUILD
  Requires PS5 payload SDK at /opt/ps5-payload-sdk (or set SDK=).
  Embed a current player first:
    cp /path/to/PS5MediaPlayerPRO.elf assets/ProsperoPlayer.elf
  Then:
    make clean all
  Outputs:
    ProsperoPlayer_MediaLauncher.elf
    ProsperoPlayer_UninstallTile.elf

THIRD-PARTY
  core/hbldr.c, elfldr.c, pt.c — John Törnblom ps5-payload-websrv BigApp
  path, GPL-3.0-or-later (see docs/GPL-3.0.txt and docs/THIRD_PARTY.txt).
  Prospero packaging, title IDs, ports, and orchestration are original.

DO NOT SHIP
  Older experimental install ELFs (incomplete host / tile-only installers).
  They can corrupt app.db or leave lock / disc-like entries.
