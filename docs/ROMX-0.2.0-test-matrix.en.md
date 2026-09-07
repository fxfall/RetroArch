# ROMX 0.2.0 Test and Core Modification Matrix (English)

> Current-state matrix for the ROMX 0.2.0 specification. Concrete game names, serials, and image filenames are replaced with `***`. Test rounds and absolute local paths are intentionally omitted.
>
> 中文版: [ROMX-0.2.0-test-matrix.md](ROMX-0.2.0-test-matrix.md)

## 1. Status definitions

| Status | Meaning |
| --- | --- |
| Supported | ROMX recognition, entry preparation, and the core-facing path pass. |
| Conditionally supported | The ROMX path passes, but complete execution needs BIOS, core system files, or other environment resources. |
| Not supported | The core does not declare the entry extension or does not provide the required VFS; the frontend rejects it explicitly. |
| Not tested | There is not enough real media or runtime environment to infer support. |

## 2. Automated and container tests

| Test layer | Coverage | Status |
| --- | --- | --- |
| libromx 0.2.0 baseline | C/C++ phases 1–8, payload view, payload file, and v2 reader/VFS | Supported, exercised by the current libromx test project |
| libromx release build | reader/VFS; writer, mutable bundle, STATS, commit, and probe | Supported, 3/3 passed; uses the current development snapshot's public API; freeze a clean revision before release |
| libromx no-mmap build | writer/mutable paths | Supported, 1/1 passed |
| romx-gui | streamed multi-entry `pack-set`, descriptor/sidecar paths, and entry offsets | Supported, 42 passed, 1 ignored, 0 failed; clippy passed |
| ROMX conversion and structural verification | 3 Saturn CUE+BIN, 2 PlayStation CHD, 1 CCD+IMG+SUB, 3 PBP, 2 GameCube ISO, and 2 GCM containers | Supported, 13/13 passed `verify` |
| Mutable write verification | Fake SAVE/CHEAT/STATS writes, rereads, and generation increments | Supported, all 13 containers succeeded |
| RetroArch macOS arm64 | Dynamic component and static ABI builds; real-ROMX startup regression | Supported |
| RetroArch ROMX adapter suite | mapped/materialized/VFS, flat/PSP/3DS Title Save/ExtData, STATS delta/overflow, POSIX symlink cleanup, ordinary ROM/ZIP | Supported, 145/145; ASan/UBSan 145/145 |
| Real ROMX sample smoke tests | GBA, PSP, 3DS, FBNeo private ZIP, Saturn CUE+BIN, PlayStation CHD; ordinary NDS regression | Supported (BIOS/Vulkan conditions are listed in the core matrix) |
| Yabause/YabaSanshiro macOS arm64 | ROMX VFS adapter dynamic-library builds | Supported |

## 3. RetroArch ROMX adapter

| Capability | Verification | Status |
| --- | --- | --- |
| Standard recognition and identity | Only `.romx`; Footer/RIDX/metadata project platform, title, serial, and entry extension | Supported |
| Malformed input | Invalid Footer, invalid RIDX, and missing entrypoint | Supported (correctly rejected) |
| Single-file entry | Bounded reads, payload mapping, and entry-only materialization when a path is required | Supported |
| Multi-file entry | CUE virtual paths, RIDX sidecar reads, `stat/size/readdir`, and preserved relative paths | Supported |
| Path safety | `../`, absolute paths, backslashes, and stale namespaces after session teardown | Supported (correctly rejected) |
| Cover/metadata | Embedded PNG extraction, deterministic cache, and normal database fallback when metadata is missing | Supported |
| Ordinary-file regression | Ordinary ROMs stay on transparent upstream VFS; ZIP uses the existing archive parser | Supported |
| Profile-driven saves | libromx SAVE catalog/profile selects single-file, PSP marker-directory, or 3DS directory-per-save grouping; no directory guessing | Supported |
| PSP/3DS saves | PSP uses validated `DISC_ID`/directory rules; 3DS covers Title/ExtData, Gateway, SaveDataFiler, and Citra/Azahar directory candidates | Supported |
| SAVE/CHEAT/STATS | Enumerate, inspect, import, replace, export, delete, actual paths, rescans, and stable IDs; 3DS Title Save/ExtData and STATS session-delta/overflow fixtures | Supported |
| Atomic rollback | Capacity or mid-write failure restores the pre-write state | Supported |
| Repeated lifecycle | Repeated mapped, materialized, VFS, and mutable session open/close with resource cleanup | Supported |

## 4. Content and core support matrix

`mmap/data` means a single-file payload is provided through `retro_game_info.data`; `materialized` means only the entry file is created; `VFS` means the core reads RIDX virtual files through `romx://<stable-id>/***`.

| Platform / entry format | Core | Entry strategy | Current status | Core modified |
| --- | --- | --- | --- | --- |
| Game Boy single file | Gambatte | Payload mmap/data | Supported | No |
| Game Boy Advance single file | mGBA | Payload mmap/data | Supported | No |
| Nintendo DS single file | DeSmuME | Payload mmap/data; materialize entry if a path is required | Supported | No |
| PSP single file | PPSSPP | `VFS + libromx mmap` | Supported | No |
| 3DS single file | Azahar | `VFS + libromx mmap` | Supported | Yes |
| Arcade private RIDX ZIP | FBNeo (reference core) | Single-file entry materialized as `***.zip` | Supported: private file-format IDs are accepted only with a validated RIDX path extension | Yes (separate reference project) |
| PlayStation 2 single image | Play! | `VFS + libromx mmap` | Supported | Yes |
| Saturn CUE+BIN | Beetle Saturn | VFS V4 with RIDX sidecars | Conditionally supported: Saturn BIOS required | No |
| Saturn CUE+BIN | Mednafen Saturn | VFS V4 with RIDX sidecars | Conditionally supported: Saturn BIOS required | No |
| Saturn CUE+BIN | Yabause `romx0.2.0` | VFS V4 with `RFILE/filestream` | Conditionally supported: Saturn BIOS required | Yes |
| Saturn CUE+BIN | YabaSanshiro `romx0.2.0` | VFS V4 with ROMX namespace | Conditionally supported: Saturn BIOS required | Yes |
| PlayStation CHD | PCSX-ReARMed | Materialize only `***.chd` | Supported | No |
| PlayStation CHD | SwanStation | Materialize only `***.chd` | Supported | No |
| PlayStation PBP | PCSX-ReARMed | Materialize only `***.pbp` | Supported | No |
| PlayStation CCD+IMG+SUB | Beetle PSX HW | VFS V4; RIDX provides all three files | Conditionally supported: PS1 BIOS required | No |
| PlayStation CCD+IMG+SUB | PCSX-ReARMed / SwanStation | Runtime extension check | Not supported: current runtime does not declare `.ccd` | No |
| GameCube ISO | Dolphin | Materialize only `***.iso` | Conditionally supported: core system files required | No |
| GameCube GCM | Dolphin | Materialize only `***.gcm` | Conditionally supported: core system files required | No |

### 4.1 Formats not tested

| Platform / format | Status |
| --- | --- |
| Dreamcast GDI multi-file | Not tested |
| Dreamcast / Saturn / PlayStation M3U multi-disc | Not tested |
| GameCube RVZ and WBFS | Not tested |

## 5. Core source modification summary

GBAStation entries are independent reference projects and must not be confused with RetroArch's bundled cores or the local Azahar/Play! forks. No entry modifies the Libretro ABI.

| Core / project | Branch or source | Modified | ROMX-related change |
| --- | --- | --- | --- |
| Gambatte, mGBA, DeSmuME, PPSSPP | RetroArch bundled cores | No | Handled by the RetroArch ROMX frontend/session, payload mmap, and standard VFS |
| Beetle Saturn, Mednafen Saturn | Upstream cores | No | Use frontend VFS directly; do not parse ROMX |
| Beetle PSX HW, PCSX-ReARMed, SwanStation | Upstream cores | No | CCD uses frontend VFS; CHD/PBP use entry materialization |
| Dolphin | Upstream core | No | ISO/GCM materialize only the entry; the core reads the original format |
| Azahar | `ns` | Yes | `file_util.cpp/.h`: safe `Swap/Close/GetFd` handling for opaque VFS handles; virtual paths are not forced into host `FILE*` |
| Play! | `vfs-support` | Yes | Added `RomxVfsAdapter.cpp/.h`; reuses `CLibretroVfsStream`, restricts the ROMX namespace, handles load/unload cleanup, and reports missing VFS |
| Yabause | `romx0.2.0` | Yes | Added `romx_vfs_adapter.c/.h`; registered it in the Makefile; initializes it from `libretro.c`, uses `RFILE/filestream_*` for M3U, and keeps VFS lifetime stable |
| YabaSanshiro | `romx0.2.0` | Yes | Added `romx_vfs_adapter.c/.h`; registered it in the Makefile; initializes the adapter and keeps VFS lifetime stable |
| GBAStation 3DS | `romx-0.2.0` reference project | Yes | `romx_io_file`, loader, libromx integration, and platform build logic; not RetroArch's bundled Azahar |
| GBAStation FBNeo | `romx-0.2.0` reference project | Yes | `FbneoRomxArchive` and FBNeo archive/libretro integration; not upstream FBNeo |
| GBAStation PPSSPP | `romx-0.2.0` reference project | Yes | `RomxFileLoader`, loader integration, and the libromx submodule; not RetroArch's bundled PPSSPP |

## 6. RetroArch-side change scope

| Layer | Responsibility | Main modules |
| --- | --- | --- |
| ROMX session | `.romx` allowlist, Footer/RIDX/metadata/cover/entrypoint, mapping/materialization/VFS, cleanup | `romx_frontend.c/.h` |
| ROMX VFS | RIDX virtual-file read/seek/tell/stat/size/readdir and namespace restrictions | `romx_vfs.c/.h` |
| Mutable adapter | Platform save identification, import/replace/export/delete, and atomic rollback | `romx_save_adapter.c/.h`, `romx_persistence.c/.h` |
| Content flow | Only `.romx` enters the ROMX session; ordinary ROMs, ZIPs, and history keep the upstream flow | `task_content.c`, `runloop.c`, `core_info.c` |
| Scan/database/UI | Extension filtering, metadata/RDB, cover cache, and ROMX save menu | `manual_content_scan.c`, `task_database.c`, `gfx_thumbnail.c`, `menu/*`, `intl/*` |

The RetroArch worktree change size is recorded by `git diff --stat`, plus the independent `romx/` component and tests. `libretro.h` is unchanged.

## 7. Specification conclusions

1. `.romx` is the only container entry extension. Do not infer ROMX from arbitrary `x` suffixes or ordinary archive paths.
2. Single-file entries should use payload mapping first; materialize only the entry file when a host path is required.
3. Multi-file entries must use Libretro VFS. A core without VFS is explicitly rejected unless a separately reviewed core adapter is provided.
4. A core sees the original entry extension, never `.romx`, Footer, RIDX, or mutable data.
5. Save-slot grouping is defined by the libromx platform profile. Ordinary platforms must not use “one top-level directory equals one save”; PSP/3DS directory candidates must be validated by libromx.
6. The frontend calls only the ROMX adapter API; parsing, VFS boundaries, atomic mutable commits, and lifetimes belong to the ROMX/libromx layer.
7. Core adapters must not modify the Libretro ABI. Ordinary ROMs, ZIPs, playlist/history, savefile paths, and existing behavior must remain unchanged.
