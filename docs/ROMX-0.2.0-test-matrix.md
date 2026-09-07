# ROMX 0.2.0 测试与核心改动矩阵（中文）

> 当前状态矩阵，用于整理 ROMX 0.2.0 规范。具体游戏名、序列号和镜像文件名统一写作 `***`。文档不记录测试轮次或本地绝对路径。
>
> English version: [ROMX-0.2.0-test-matrix.en.md](ROMX-0.2.0-test-matrix.en.md)

## 1. 状态定义

| 状态 | 含义 |
| --- | --- |
| 支持 | ROMX 识别、入口准备和核心调用路径通过。 |
| 支持（有条件） | ROMX 路径通过，但完整运行需要 BIOS、核心系统文件或其他环境资源。 |
| 不支持 | 核心实际不声明入口扩展名或不提供所需 VFS；前端会明确拒绝。 |
| 未测试 | 没有足够的实镜像或运行环境，不能推断支持。 |

## 2. 自动化与容器测试

| 测试层 | 覆盖内容 | 状态 |
| --- | --- | --- |
| libromx 0.2.0 基线 | phase 1–8（C/C++）、payload view、payload file、v2 reader/VFS | 支持，按当前 libromx 测试工程执行 |
| libromx 发布构建 | reader/VFS；writer、mutable bundle、STATS、commit、probe | 支持，3/3 通过；使用当前开发快照的公共 API，发布前需冻结干净 revision |
| libromx 无 mmap 构建 | writer/mutable 路径 | 支持，1/1 通过 |
| romx-gui | `pack-set` 多入口流式写入、descriptor/旁车路径、入口偏移 | 支持，42 通过、1 忽略、0 失败；clippy 通过 |
| ROMX 转换与结构校验 | Saturn CUE+BIN 3 个、PlayStation CHD 2 个、CCD+IMG+SUB 1 个、PBP 3 个、GameCube ISO 2 个、GCM 2 个 | 支持，13/13 通过 `verify` |
| mutable 写入校验 | 虚假 SAVE/CHEAT/STATS 写入、重新读取、generation 递增 | 支持，13/13 容器成功 |
| RetroArch macOS arm64 | 动态组件与静态 ABI 两种构建；实际 ROMX 启动回归 | 支持 |
| RetroArch ROMX adapter suite | mapped/materialized/VFS、flat/PSP/3DS Title Save、STATS delta/overflow、POSIX symlink cleanup、普通 ROM/ZIP | 支持，137/137；ASan/UBSan 137/137 |
| 真实 ROMX 样本 smoke test | GBA、PSP、3DS、FBNeo 私有 ZIP、Saturn CUE+BIN、PlayStation CHD；普通 NDS 回归 | 支持（BIOS/Vulkan 条件见核心矩阵） |
| Yabause/YabaSanshiro macOS arm64 | ROMX VFS adapter 动态库构建 | 支持 |

## 3. RetroArch ROMX 适配层

| 能力 | 验证内容 | 状态 |
| --- | --- | --- |
| 标准识别与身份 | 只接受 `.romx`；Footer/RIDX/metadata 投影平台、标题、序列号、入口扩展名 | 支持 |
| 损坏输入 | 无效 Footer、无效 RIDX、缺少入口点 | 支持（正确拒绝） |
| 单文件入口 | bounded read、payload mapping、需要路径时只物化入口文件 | 支持 |
| 多文件入口 | CUE 虚拟路径、RIDX 旁车读取、`stat/size/readdir`、相对路径保持有效 | 支持 |
| 路径安全 | `../`、绝对路径、反斜杠、已结束 session 的旧 namespace | 支持（正确拒绝） |
| Cover/metadata | 内嵌 PNG 提取、确定性缓存、metadata 缺失时普通数据库回退 | 支持 |
| 普通文件回归 | 普通 ROM 继续使用上游透明 VFS；ZIP 使用现有 archive parser | 支持 |
| profile 驱动存档 | 由 libromx SAVE catalog/profile 决定单文件、PSP marker-directory 或 3DS directory-per-save；不会按目录猜测 | 支持 |
| PSP/3DS 存档 | PSP 经过有效 `DISC_ID`/目录规则验证；3DS 支持 Title/ExtData、Gateway、SaveDataFiler、Citra/Azahar 目录候选 | 支持 |
| SAVE/CHEAT/STATS | enumerate、inspect、import、replace、export、delete、实际路径、重新扫描、stable id；3DS Title Save 与 STATS session delta/overflow fixture | 支持 |
| 原子回滚 | 容量不足或中途失败时恢复写入前状态 | 支持 |
| 重复生命周期 | mapped、materialized、VFS、mutable session 的反复打开关闭和资源释放 | 支持 |

## 4. 内容与核心支持矩阵

`mmap/data` 表示单文件 payload 通过 `retro_game_info.data` 提供；`物化`表示只生成入口文件；`VFS`表示核心通过 `romx://<stable-id>/***` 读取 RIDX 虚拟文件。

| 平台 / 入口格式 | 核心 | 入口策略 | 当前状态 | 核心改动 |
| --- | --- | --- | --- | --- |
| Game Boy 单文件 | Gambatte | payload mmap/data | 支持 | 否 |
| Game Boy Advance 单文件 | mGBA | payload mmap/data | 支持 | 否 |
| Nintendo DS 单文件 | DeSmuME | payload mmap/data；需要路径时物化入口 | 支持 | 否 |
| PSP 单文件 | PPSSPP | `VFS + libromx mmap` | 支持 | 否 |
| 3DS 单文件 | Azahar | `VFS + libromx mmap` | 支持 | 是 |
| Arcade 私有 RIDX ZIP | FBNeo（参考核心） | 单文件入口物化为 `***.zip` | 支持：私有 file-format ID 仅使用已验证 RIDX 路径扩展名 | 是（独立参考项目） |
| PlayStation 2 单文件镜像 | Play! | `VFS + libromx mmap` | 支持 | 是 |
| Saturn CUE+BIN | Beetle Saturn | VFS V4、RIDX 旁车 | 支持（有条件：需要 Saturn BIOS） | 否 |
| Saturn CUE+BIN | Mednafen Saturn | VFS V4、RIDX 旁车 | 支持（有条件：需要 Saturn BIOS） | 否 |
| Saturn CUE+BIN | Yabause `romx0.2.0` | VFS V4、`RFILE/filestream` | 支持（有条件：需要 Saturn BIOS） | 是 |
| Saturn CUE+BIN | YabaSanshiro `romx0.2.0` | VFS V4、ROMX namespace | 支持（有条件：需要 Saturn BIOS） | 是 |
| PlayStation CHD | PCSX-ReARMed | 单文件入口物化为 `***.chd` | 支持 | 否 |
| PlayStation CHD | SwanStation | 单文件入口物化为 `***.chd` | 支持 | 否 |
| PlayStation PBP | PCSX-ReARMed | 单文件入口物化为 `***.pbp` | 支持 | 否 |
| PlayStation CCD+IMG+SUB | Beetle PSX HW | VFS V4，三个文件分别由 RIDX 提供 | 支持（有条件：需要 PS1 BIOS） | 否 |
| PlayStation CCD+IMG+SUB | PCSX-ReARMed / SwanStation | 运行时扩展名检查 | 不支持：当前运行时不声明 `.ccd` | 否 |
| GameCube ISO | Dolphin | 单文件入口物化为 `***.iso` | 支持（有条件：需要核心系统文件） | 否 |
| GameCube GCM | Dolphin | 单文件入口物化为 `***.gcm` | 支持（有条件：需要核心系统文件） | 否 |

### 4.1 未测试格式

| 平台 / 格式 | 状态 |
| --- | --- |
| Dreamcast GDI 多文件 | 未测试 |
| Dreamcast / Saturn / PlayStation M3U 多碟 | 未测试 |
| GameCube RVZ、WBFS | 未测试 |

## 5. 核心源码改动统计

GBAStation 条目是独立参考项目，不与 RetroArch 自带核心或 Azahar/Play! 本地 fork 混用。所有条目均未修改 Libretro ABI。

| 核心 / 项目 | 分支或来源 | 是否改动 | 改动内容 |
| --- | --- | --- | --- |
| Gambatte、mGBA、DeSmuME、PPSSPP | RetroArch 自带核心 | 否 | 由 RetroArch ROMX frontend/session、payload mmap 和标准 VFS 处理 |
| Beetle Saturn、Mednafen Saturn | 上游核心 | 否 | 直接使用前端 VFS，不解析 ROMX |
| Beetle PSX HW、PCSX-ReARMed、SwanStation | 上游核心 | 否 | CCD 使用前端 VFS；CHD/PBP 使用入口物化 |
| Dolphin | 上游核心 | 否 | ISO/GCM 只物化入口，核心仍按原格式读取 |
| Azahar | `ns` | 是 | `file_util.cpp/.h`：opaque VFS handle 的 `Swap/Close/GetFd` 安全处理；虚拟路径不再强行解释为宿主 `FILE*` |
| Play! | `vfs-support` | 是 | 新增 `RomxVfsAdapter.cpp/.h`；复用已有 `CLibretroVfsStream`，限制 ROMX namespace，处理加载/卸载清理和无 VFS 错误 |
| Yabause | `romx0.2.0` | 是 | 新增 `romx_vfs_adapter.c/.h`；注册 Makefile；`libretro.c` 初始化 adapter，M3U 使用 `RFILE/filestream_*`，保持 VFS 表生命周期稳定 |
| YabaSanshiro | `romx0.2.0` | 是 | 新增 `romx_vfs_adapter.c/.h`；注册 Makefile；初始化 adapter，保持 VFS 表生命周期稳定 |
| GBAStation 3DS | `romx-0.2.0` 参考项目 | 是 | `romx_io_file`、loader、libromx 集成及平台构建逻辑；不属于 RetroArch 自带 Azahar |
| GBAStation FBNeo | `romx-0.2.0` 参考项目 | 是 | `FbneoRomxArchive` 与 FBNeo archive/libretro 接入；不属于上游 FBNeo |
| GBAStation PPSSPP | `romx-0.2.0` 参考项目 | 是 | `RomxFileLoader`、loader 接入及 libromx 子模块；不属于 RetroArch 自带 PPSSPP |

## 6. RetroArch 侧改动范围

| 层 | 职责 | 主要模块 |
| --- | --- | --- |
| ROMX session | `.romx` 白名单、Footer/RIDX/metadata/cover/entrypoint、mapping/物化/VFS、清理 | `romx_frontend.c/.h` |
| ROMX VFS | RIDX 虚拟文件的 read/seek/tell/stat/size/readdir 和 namespace 限制 | `romx_vfs.c/.h` |
| mutable 适配 | 平台存档识别、导入/替换/导出/删除、原子回滚 | `romx_save_adapter.c/.h`、`romx_persistence.c/.h` |
| 内容流程 | 仅 `.romx` 进入 ROMX session；普通 ROM、ZIP、history 保持原流程 | `task_content.c`、`runloop.c`、`core_info.c` |
| 扫描/数据库/UI | 扩展过滤、metadata/RDB、cover 缓存、ROMX 存档菜单 | `manual_content_scan.c`、`task_database.c`、`gfx_thumbnail.c`、`menu/*`、`intl/*` |

RetroArch 相对上游的跟踪文件改动规模以 `git diff --stat` 为准，另有独立 `romx/` 组件和测试文件。没有修改 `libretro.h`。

## 7. 规范结论

1. `.romx` 是唯一容器入口扩展名；不能用任意 `x` 后缀或普通 archive 路径猜测。
2. 单文件入口优先使用 payload mapping；需要宿主路径时只物化入口文件。
3. 多文件入口必须通过 Libretro VFS；无 VFS 的核心明确拒绝，或由单独审查的核心 adapter 支持。
4. 核心只能看到入口文件的原始扩展名，不能看到 `.romx`、Footer、RIDX 或 mutable 区域。
5. 存档 slot 由 libromx 平台 profile 决定；普通平台不得使用“一层目录就是一个存档”，PSP/3DS 目录候选必须由 libromx 验证。
6. 前端只调用 ROMX 适配接口；解析、VFS 边界、mutable 原子提交和生命周期由 ROMX/libromx 层负责。
7. 核心适配不得修改 Libretro ABI，普通 ROM、ZIP、playlist/history、savefile 路径和原有行为必须保持不变。
