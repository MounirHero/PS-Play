# ProsperoPlayer Media home launcher

## Goal
A **Prospero-only** Media-section app you install by injecting one resident
payload after jailbreak (autoload recommended). No Payload Manager browser
“ok” page. No third-party product names or title IDs.

## Technique (what we do — not whose brand)

| Step | What | Why |
|------|------|-----|
| 1 | Write player to `/data/homebrew/ProsperoPlayer/eboot.elf` | Runtime image |
| 2 | Build system host `/system_ex/app/PRSP10001/` | Shell can start a BigApp |
| 3 | Host `eboot.bin` = copy of **NPXS40106** (system media host) | Valid signed host process |
| 4 | Raise AppInst authid `0x4801000000000013` | AppInstUtil accepts the install |
| 5 | Register `/user/app/PRSP10001` with Media category **65536** | Shows under **Media** |
| 6 | Tile param: category + attributes + deeplink — **no contentId** | Avoids lock / “not supported” |
| 7 | Host param also **no contentId** | Fake contentId triggers shell license check / toast |
| 8 | Resident HTTP on `127.0.0.1:9055` | Tile deeplink target |
| 9 | `/launch` → **204 first**, then **hbldr** on a worker thread | Fast deeplink; less “not supported” flash |
| 10 | HTTP **204** empty body | No browser document left open |
| 11 | `/uninstall` + `ProsperoPlayer_UninstallTile.elf` | Remove when XMB Delete is missing or incomplete |

## Prospero identity

| Item | Value |
|------|--------|
| Title ID | `PRSP10001` |
| Display name | ProsperoPlayer |
| Loopback port | **9055** |
| Runtime dir | `/data/homebrew/ProsperoPlayer` |
| Logs | `/data/prosperoplayer/media_launcher.log` |
| ELF | `ProsperoPlayer_MediaLauncher.elf` |

Legacy experimental IDs cleaned on install: `PRSP00001`, `PSMC00002`.

## Why earlier builds broke the UI

1. AppInst without authid → errors / partial registration  
2. `contentId` on the **tile** → lock + unsupported  
3. Incomplete system host → disc-like ghost apps  
4. `InstallAll` / bad uninstall → app.db stress  
5. Launch via raw elfldr only → wrong process model  

## User steps

1. Jailbreak (elfldr up)  
2. Inject **`ProsperoPlayer_MediaLauncher.elf`** (autoload each boot)  
3. Wait for notification: ProsperoPlayer ready in Media  
4. Home → **Media** → ProsperoPlayer  
5. Keep the launcher payload running  

Safe without any install: inject `RUN_ME_PLAYER.elf` only.

## Third-party code

`core/hbldr.c`, `elfldr.c`, `pt.c` implement the BigApp process transition and
are derived from John Törnblom’s **ps5-payload-websrv** (GPL-3.0+).  
ProsperoPlayer packaging, title IDs, ports, and launcher orchestration are
original to this project.
