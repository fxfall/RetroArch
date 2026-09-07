# RetroArch ROMX 独立组件重构开发文档

> 文档状态：开发实施基线
> 审查日期：2026-09-07
> 适用仓库：`retroarch-romx`、外部依赖 `libromx`
> 目标读者：没有看过当前 `retroarch-romx` 修改的 RetroArch/C 开发人员

## 1. 结论先行

本轮重构已经把 ROMX 运行时收敛为独立内容组件：RetroArch Host 只调用通用 component ABI，桌面构建使用独立动态库，静态平台使用同一套组件源码和 ABI；libromx 只通过公共 API 提供格式、RIDX、VFS、SAVE profile 和 mutable 能力。实际 ROM 回归覆盖了 GBA、PSP、3DS、FBNeo 私有 ZIP、Saturn 多文件、PlayStation CHD 以及普通 NDS。

当前仍有一个发布门禁：开发验证使用的是外部 `libromx` 工作树的 `f43118d` 快照，该工作树存在未提交优化，必须在 libromx 仓库完成测试、冻结并记录不可变 revision 后才能发布。macOS 当前环境还缺少 `xcrun metal`，因此强制全量 shader 重编译不是本轮 ROMX 失败；组件、Host 链接和实际核心 smoke test 已单独通过。

本文件保留原始审查项，3.2/3.3 中的“已处理”和“待发布”标签表示当前实现状态，而不是要求开发人员重复实现已经完成的工作。

本次重构的最终形态必须是：

1. ROMX 是一个独立的内容组件，RetroArch 主体只通过一个稳定的组件 ABI 调用它；
2. 桌面平台默认将 ROMX 组件编译为独立动态库，组件内部静态链接一个精确锁定、未修改的 libromx；
3. 不支持动态加载的平台使用同一 ABI 的静态注册方式，不维护第二套 ROMX 实现；
4. RetroArch 主程序、核心和数据库代码都不能直接包含 `<romx/romx.h>`；
5. 格式解析、校验、RIDX、RMBL、SAVE profile、CRC 和 mutable 提交全部调用 libromx 公共 API；
6. RetroArch 专属的目录、菜单、核心能力、缓存和写回策略只存在于 ROMX 组件，不进入 libromx；
7. 普通 ROM、ZIP、playlist、history、thumbnail 和 VFS 行为必须保持上游语义。

## 2. 名词与仓库边界

| 名词 | 本文含义 |
| --- | --- |
| ROMX 容器 | ROMX 0.2.0 文件，外层扩展名固定为 `.romx`。 |
| entrypoint | RIDX 中唯一带入口标记的虚拟文件；可能是 ROM、ISO、ZIP，也可能是 CUE/GDI/M3U 等描述文件。 |
| libromx | 只负责 ROMX wire format、校验、reader/writer、RIDX VFS、RMBL、SAVE profile 和 mutable 原子提交的 C99 库。 |
| ROMX 组件 | RetroArch 专用适配模块。它把 libromx 的通用能力转换成 RetroArch 的内容、VFS、扫描、封面、存档和菜单行为。 |
| Host | RetroArch 主程序。Host 不理解 Footer/RIDX/RMBL 字节布局。 |
| Core | Libretro 核心。Core 只能看到原始入口格式及标准 VFS，不能看到 ROMX Footer、RIDX 或 mutable 区。 |
| source path | 用户选择的 `.romx` 路径；history/playlist 必须保存该路径。 |
| core path | 提供给 Core 的逻辑路径、`romx://` VFS 路径或临时物化路径。 |

责任边界必须固定如下：

| 能力 | libromx | ROMX 组件 | RetroArch Host | Core |
| --- | --- | --- | --- | --- |
| Footer/RIDX/metadata/PNG 校验 | 是 | 调用 | 否 | 否 |
| entrypoint mapping/read/extract | 是 | 选择策略并持有生命周期 | 只接收 view | 消费 |
| 多文件虚拟路径读取 | 提供有界 cursor | 实现 Libretro VFS overlay | 发布 VFS 表 | 调用 VFS |
| SAVE profile/候选分类/slot 投影 | 是 | 调用并映射到 Host 路径 | 提供路径和用户选择 | 否 |
| Host staging、冲突检测、回滚 | 否 | 是 | 提供文件服务 | 否 |
| SAVE/CHEAT/STATS 写回策略 | 否 | 是 | 触发命令、提供当前状态 | 否 |
| RetroArch 菜单与通知 | 否 | 提供动作描述/执行入口 | 渲染和转发 | 否 |
| ROMX 格式扩展或私有解析 | 仅 libromx 项目决定 | 禁止 | 禁止 | 禁止 |

## 3. 当前代码审查结果

### 3.1 已验证能力

审查时执行结果：

- libromx 当前工作树：构建成功，`ctest` 为 3/3 通过；
- `romx/tests/romx_frontend_test.c`：137/137 检查通过；
- 同一适配测试在 AddressSanitizer + UndefinedBehaviorSanitizer 下为 137/137 通过（macOS 的 LeakSanitizer 不可用，未启用 `detect_leaks`）；
- `git diff --check` 在两个仓库中均无空白错误。

从已挂载的真实 ROMX 样本抽取的核心 smoke test 结果：

| 样本类型 | 结果 |
| --- | --- |
| GBA / mGBA | payload mmap，启动一帧成功 |
| PSP / PPSSPP | 入口物化为 ISO，进入核心成功 |
| 3DS / Azahar | 入口物化并被核心识别为 NCSD；运行阶段受 Host 未编译 Vulkan 限制 |
| FBNeo 私有 RIDX 文件格式 / ZIP | 私有 format ID 通过路径扩展名投影，进入核心成功 |
| Saturn CUE+BIN / Mednafen Saturn | `romx://` VFS、SAVE/CHEAT 恢复和 sidecar 读取路径通过；缺少 Saturn BIOS |
| PlayStation CHD / SwanStation | CHD 入口物化并进入核心；缺少 PS1 BIOS |
| 普通 NDS / DeSmuME | 未进入 ROMX 组件，普通内容路径通过 |

这些结果能证明现有单线程适配基线没有明显的内存/未定义行为回归，但不能覆盖下一节列出的发布风险。

### 3.2 发布阻断项

#### P0（待发布）：libromx 依赖冻结

审查时 `libromx` 位于提交 `f43118d`，其工作树仍包含以下未提交变化：

- bundle 新对象自动保留 25%（至少 1 KiB）增长空间；
- replacement 忽略只适用于新对象的 `data_capacity` 提示；
- `romx_mutable_copy_region_path()`；
- `ROMX_WRITER_COMPUTE_METADATA_CRC32`；
- `ROMX_WRITER_DIRECT_OUTPUT`。

这些变化没有对应的新版本号或稳定 revision。当前 RetroArch `qb/config.libs.sh` 已探测公共的 `romx_save_catalog_open_path`，而内部头文件仍只判断 `ROMX_VERSION_MAJOR == 0 && ROMX_VERSION_MINOR >= 2`。因此“头文件叫 0.2.0”仍不能证明编译期和运行期具备相同 API/行为。

处理要求：

- 先在 libromx 仓库将优化提交、测试、打 tag 或记录不可变 commit；
- RetroArch ROMX 组件只依赖该精确 revision；
- 桌面动态组件内部静态链接该 revision 的 libromx，RetroArch 主程序不再链接系统中的任意 `-lromx`；
- 不允许把 libromx 源码复制出来再打 RetroArch 私有补丁；
- 如果缺少公共 API，RetroArch 任务必须暂停并记录依赖请求，不能在 vendored libromx 上临时加函数。

#### 已处理：Host 清理不跟随符号链接

当前组件删除了重复的 `remove_tree()`，统一使用 `romx/romx_host_transaction.c`。POSIX 使用 `lstat`，Windows 使用 reparse-point 属性检查；回滚只调用这套事务工具，不把链接目标当作目录递归。适配器套件已覆盖 POSIX 符号链接删除不跟随目标；Windows reparse-point 和 staging swap 仍应在发布 CI 中补齐。

#### 已处理：3DS SAVE 由 libromx profile 驱动

`romx_save_adapter.c` 的 Host 导入、候选枚举和写回统一使用 `romx_save_catalog_*`；恢复使用 `romx_mutable_bundle_get_save_layout()`/`get_save_slot*()`。PSP marker-directory 与 3DS Title/ExtData、Gateway、SaveDataFiler、Citra/Azahar 候选由 libromx profile 判定，组件只映射 RetroArch/Azahar 目标根目录。libromx 的 save-manager 测试覆盖 3DS profile；RetroArch 适配套件已加入带 `saveData.bin` 与附加成员的 Citra/Azahar Title Save fixture，真实模拟器目录和 ExtData 映射仍需平台 CI 复核。

#### 已处理：SAVE destination plan 预检

恢复前先生成完整 destination plan，按大小写不敏感的规范化路径检查重复、祖先/子孙冲突和越界路径；冲突直接拒绝整批恢复，不再采用“第一个赢、其余静默跳过”。`romx_persistence_add_mapping()` 同时保护后续写回的 stable ID → Host 路径映射。

### 3.3 高优先级正确性问题

#### 已处理：STATS 使用 baseline + session delta

`romx_persistence_write_stats()` 在每次写回前重新读取 ROMX 最新 STATS，构造本会话 checkpoint 之后的 delta，并调用 `romx_mutable_stats_merge_session_delta()`；首次写回只提交一次 launch count，后续写回只提交新增 runtime。适配器套件已覆盖 baseline/delta 合并和 safe-integer overflow；外部 generation 变化与连续两次真实写回仍应在发布 CI 中保留回归。

实现要点：

1. 激活时记录本会话起点，不把“本地累计总数”当成 delta；
2. 每次写回前重新读取 ROMX 最新 STATS/generation；
3. 构造“上次成功提交以来”的 session delta；
4. 调用 `romx_mutable_stats_merge_session_delta(latest, delta, merged)`；
5. 写入成功后推进 session checkpoint；
6. `launch_count = 1` 每个启动会话只提交一次，后续手动写回为 0；
7. overflow、generation 变化和连续两次写回必须有测试。

#### 已处理：严格数据库扫描语义

组件扫描先使用 metadata/RIDX identity 做 CRC/size lookup；strict/ DAT-strict 只有数据库命中才添加，loose 未命中才进入 metadata fallback，且不会把外层 `.romx` 当作普通 archive 或重新计算整包 CRC。无 CRC 的容器保持为不可匹配 identity，不伪造 `00000000|crc`。

#### 已处理（实现方式为串行操作锁）：VFS 关闭与并发访问

`romx_vfs.c` 对 binding、file cursor、directory cursor 使用同一把进程级锁，并把每个 ROMX `read/seek/tell/size/flush/readdir/dirent` 操作完整包在锁内；deactivate 先持锁关闭 libromx cursor，再将 Core 可能保留的 wrapper 标记为 tombstone。目录快照在 wrapper close 前保留，避免 `dirent_get_name()` 返回悬空指针。该设计用串行 operation guard 代替显式引用计数，动态组件仍保持进程生命周期加载；发布 CI 仍应补受控并发/ThreadSanitizer 回归。

#### 已处理：单外层 ROMX session 约束

通用 Host facade 明确拒绝同时存在多个外层 `.romx` session；ROMX 内部的 M3U/CUE 多文件仍属于一个容器。失败发生在 persistence staging 之前，不会产生部分 Host 恢复。将来要支持多个容器时，必须把 persistence context 改为 session 数组，而不是放宽当前单例约束。

### 3.4 剩余维护与发布工作

- `tasks/task_content.c` 仍保留若干上游内容分支和错误提示；生命周期已经统一委托给 facade 的幂等 `abort/close`，后续可在不改变行为的前提下继续把集成压缩成更少的 hook。
- `retroarch_types.h` 不再包含 ROMX `source_path` 条件字段；history 从 active component session 的通用 accessor 读取 source path。
- core selector、scanner 和 thumbnail 路径通过通用 component facade 获取一次 logical identity；缓存键使用 `romx_cover_info_t.sha256`，缺失 hash 时才回退到路径辅助键。
- VFS 代理对普通路径委托 base VFS；当前实现用同一入口保持普通 ROM/ZIP 回归，发布前应保留 I/O 基准和受控并发测试。
- `Makefile.common` 中已有的 QuartzCore 变更与 ROMX 无关，提交时必须与 ROMX 重构拆分；当前工作树保留它是为了不覆盖用户已有修改。
- 矩阵已改为 profile-driven SAVE、动态/静态组件和实际核心条件支持；当前已新增 3DS Title Save、POSIX symlink cleanup 和 STATS merge/overflow fixture；真实 3DS/ExtData、Windows reparse swap、STATS 连续写回和并发测试仍是发布门禁。

## 4. 目标架构

```text
RetroArch Host
├── content_component_registry.c      通用注册、选择、生命周期和错误转发
├── content_component_loader.c        安全加载动态组件；静态平台走同一 ABI
├── base Libretro VFS                 普通文件的唯一真实后端
└── retroarch_romx_component
    ├── component.c                   唯一导出入口和 session 状态机
    ├── inspect.c                     一次性 Footer/RIDX/metadata identity
    ├── launch.c                      map / bounded buffer / materialize / VFS
    ├── vfs_overlay.c                 romx:// namespace 和 base VFS 委托
    ├── persistence.c                 RetroArch 会话、恢复计划、写回 checkpoint
    ├── save_adapter.c                调用 libromx SAVE catalog/slot API
    ├── host_transaction.c            symlink-safe staging/rename/rollback
    ├── scan_provider.c               RDB identity，不改变 strict/loose 规则
    ├── thumbnail_provider.c          cover SHA-256 缓存
    └── vendor/libromx                精确 revision 的只读 submodule/package
```

数据方向必须保持单向：

```text
RetroArch 状态/路径/回调
          ↓
通用 Content Component ABI
          ↓
ROMX 组件中的 RetroArch 策略
          ↓
libromx 公共 C ABI
          ↓
ROMX 文件
```

禁止从 libromx 反向包含 RetroArch 头文件，禁止 Core 直接链接 libromx，也禁止 RetroArch 主流程解析 ROMX 字节。

## 5. 独立组件 ABI

### 5.1 加载形式

桌面平台组件名称建议：

- macOS：`retroarch_romx_component.dylib`
- Linux/BSD：`retroarch_romx_component.so`
- Windows：`retroarch_romx_component.dll`

动态库只导出一个 C 符号：

```c
const struct rarch_content_component_v1 *
rarch_content_component_get_v1(
      const struct rarch_component_host_v1 *host);
```

组件 ABI 使用固定宽度整数、`struct_size` 和 `abi_version`。禁止跨模块传递 C++ 类型、RetroArch 私有结构、`FILE *` 或由另一侧直接 `free()` 的内存。组件创建的对象只能由组件 vtable 中的 close/destroy 释放。

静态平台直接调用同名 getter 并注册返回的 vtable。动态和静态模式必须运行同一测试集。

### 5.2 Host services

`rarch_component_host_v1` 至少提供：

- base Libretro VFS v4 表；
- 日志回调和消息通知回调；
- 单调时钟与 UTC 时间；
- cache/save/system/playlist/runtime-log 等目录的只读快照；
- 当前 Core 的 `need_fullpath`、VFS 能力、valid extensions；
- SAVE 文件列表的快照回调；
- 刷新 SRAM/save files 的回调；
- 保存 game-specific cheat 的回调；
- runtime/launch 的 session counters；
- Host locale；
- Host 分配器（只有确有跨边界数组需要时使用）。

ROMX 组件不得包含 `runloop_state_t`、`settings_t`、`runtime_log_t` 或 `content_file_info_t`。这些内部类型由 Host 转换为稳定的 POD snapshot。

### 5.3 Component vtable

v1 至少包含以下能力：

```c
uint32_t abi_version;
uint32_t struct_size;
const char *component_id;              /* "romx" */
const char *container_extensions;      /* "romx" */

bool (*supports_path)(const char *path);
int  (*inspect_path)(const char *path, struct rarch_component_info_v1 *out);
int  (*session_open)(const char *path, rarch_component_session_t **out);
int  (*session_prepare)(rarch_component_session_t *session,
                        const struct rarch_core_contract_v1 *core,
                        struct rarch_component_content_view_v1 *out);
int  (*session_stage_persistence)(rarch_component_session_t *session,
                                  const struct rarch_runtime_snapshot_v1 *host);
int  (*session_commit)(rarch_component_session_t *session);
void (*session_abort)(rarch_component_session_t *session);
void (*session_close)(rarch_component_session_t *session);

const struct retro_vfs_interface *(*vfs_interface)(void);
int  (*scan_identity)(const char *path, struct rarch_scan_identity_v1 *out);
int  (*thumbnail_path)(const char *path, const char *cache_root,
                       char *out, size_t out_size);
int  (*quick_menu_actions)(rarch_component_session_t *session,
                           struct rarch_component_action_list_v1 *out);
int  (*invoke_action)(rarch_component_session_t *session,
                      const char *action_id,
                      struct rarch_component_action_result_v1 *out);
```

这是 ABI 轮廓，不要求沿用当前函数名；硬性要求是外部只包含通用 component header，所有 ROMX/libromx 类型留在组件内部。

### 5.4 状态机

```text
UNLOADED
   ↓ loader/register
AVAILABLE
   ↓ session_open
OPEN
   ↓ session_prepare
PREPARED
   ↓ session_stage_persistence
STAGED
   ├── Core load 成功 → session_commit → ACTIVE
   └── 任意失败      → session_abort  → CLOSED
ACTIVE
   ├── 显式 write-back action → ACTIVE
   └── content unload → session_close → CLOSED
```

规则：

- `abort()` 和 `close()` 必须幂等；
- 只有 `ACTIVE` session 能出现在 ROMX Quick Menu；
- Core load 成功前不能发布 metadata title 到全局 runloop；
- Core load 失败时必须撤销本事务新建的 Host 文件；
- source path 始终归 session 所有，history 从 active session 读取；
- 动态组件默认保持加载到 RetroArch 进程退出，避免 VFS 回调地址失效。

## 6. 加载流程

### 6.1 Core 选择

1. 注册表将 `.romx` 作为容器扩展提供给文件浏览器；
2. 对当前 path 调用一次 `inspect_path()`，得到逻辑 entrypoint extension；
3. 将该 extension 与 Core 的 `valid_extensions` 比较；
4. 排序比较器只能读取预先缓存的结果，不能反复打开 ROMX；
5. unknown platform/launch registry ID 必须显示“不支持”，不得按文件名猜格式；生态私有 file-format ID 只有在 RIDX 路径给出安全的核心扩展名时才可接受。

### 6.2 内容准备

对唯一的外层 ROMX 内容：

1. `session_open()` 只做有界结构读取和 metadata/cover 描述读取，不扫描完整 payload；
2. 校验选中 Core 是否支持逻辑 extension；
3. 根据 Core contract 选择：
   - `need_fullpath == false`：优先 `romx_reader_map_payload()`，失败后按 entrypoint 大小 bounded read；
   - 单文件且 `need_fullpath == true`：只物化 entrypoint 到私有 session cache；
   - 多文件：必须有 VFS，entrypoint 和 sidecar 都通过 `romx://<session-id>/...`；
4. Host 将 component content view 复制到既有 `retro_game_info`/`retro_game_info_ext`；
5. metadata 指针由 session 持有到 Core unload，Host 不释放；
6. 持久化 staging 成功后再调用 Core load；
7. Core load 成功后 commit session，失败则统一 abort。

禁止：

- 把整个 `.romx` 当普通压缩包；
- 为多文件容器解压整个 payload；
- 向 Core 传 `.romx` 扩展名；
- 修改 Libretro ABI；
- 为没有实际使用 VFS 的 Core 伪装多文件支持。

### 6.3 VFS overlay

Host 在 `RETRO_ENVIRONMENT_GET_VFS_INTERFACE` 中只做一次通用选择：有组件 overlay 时返回 registry 的稳定 VFS 表，否则返回 base VFS 表。不要在 `runloop.c` 中逐字段写 18 组 `#ifdef HAVE_ROMX`。

ROMX VFS 必须满足：

- 只拦截已注册的 `romx://<session-id>/`；
- 未知或过期的 `romx://` 路径失败，绝不落到 Host 文件系统；
- 普通路径完全委托 base VFS；
- ROMX 路径只读，write/remove/rename/mkdir/truncate 全部失败；
- 每个 open 得到独立 libromx cursor；同一 cursor 不并发 seek/read；
- 支持 v1-v4 的 path/open/close/size/tell/seek/read/stat/stat64/opendir/readdir；
- session ID 使用进程内不可复用的随机/单调标识，不只对 source path 做 64 位 hash；
- session close 与异步 VFS 操作按 3.3 的串行 operation-guard 规则同步；如果未来改为细粒度并发，必须补回显式引用计数。

## 7. 适配 libromx 新优化

### 7.1 SAVE/CHEAT bundle 容量

删除以下组件内逻辑：

- 手工计算 RMBL header/table/path/alignment；
- 1 MiB/8 MiB/256 KiB 等固定默认 object capacity；
- `data_capacity` 不断减半的循环重试；
- PSP 专属 headroom 常量。

新对象写入流程：

1. 第一次以 `write_options.data_capacity = 0` 调用 bundle writer，让新 libromx 使用自身已计算的 serialized size 和默认增长空间；
2. 若返回 `ROMX_E_MUTABLE_NO_SPACE`，并且确认目标 object 不存在，则调用一次：
   - path entries：`romx_mutable_bundle_measure_path_entries()`；
   - SAVE candidate：`romx_save_catalog_measure_candidate()`；
3. 以精确 serialized size 作为 capacity 只重试一次，以允许紧凑 mutable 区放下对象；
4. 已存在 object 的 replacement 始终复用原 extent。超出 capacity 时直接返回“需要字节数/现有 capacity”的错误，不删除旧对象，不循环重试。

这保留了 libromx 的优化，又把“空间不足时是否牺牲增长余量”的产品策略留在组件。

### 7.2 SAVE 导入

所有 Host SAVE 导入均使用：

```text
romx_save_catalog_open_path
→ romx_save_catalog_get_profile
→ romx_save_catalog_get_candidate_count/get_candidate/get_file
→ 用户或自动策略选中 candidate
→ romx_save_catalog_write_candidate
```

组件可以决定扫描哪个 RetroArch save root、是否自动选择唯一 candidate、如何提示冲突，但不能重写 PSP/3DS 分类规则。

### 7.3 STATS

强制使用 `romx_mutable_stats_merge_session_delta()`。组件只负责生成当前 session delta、重新读取 latest baseline、显示冲突和推进 checkpoint。

### 7.4 mutable region copy 与 writer flags

下列新 API 对 RetroArch 的“运行时加载”没有直接用途，不要为了宣称已适配而调用：

- `romx_mutable_copy_region_path()`：用于编辑 metadata/cover 后重建容器且无损保留整个 mutable 区；
- `ROMX_WRITER_COMPUTE_METADATA_CRC32`：用于 pack/repack 时流式计算入口 CRC 并回填 metadata placeholder；
- `ROMX_WRITER_DIRECT_OUTPUT`：用于已有外层 staging transaction 的编辑器直接写临时文件。

如果未来在 RetroArch 内加入 ROMX 编辑器，它也必须位于组件中，并采用：caller-owned sibling temp → libromx direct output → mutable region copy → verify → atomic rename。当前加载/扫描/写回任务不包含容器重打包。

## 8. SAVE/CHEAT/STATS Host 策略

### 8.1 两阶段恢复

恢复必须是一个完整事务：

1. 枚举所有可操作 slot；
2. 生成 source → destination plan；
3. 规范化路径并检查重复、大小写冲突、父子路径冲突、ROMX 自身别名；
4. 检查 symlink/reparse point；
5. 将所有待恢复内容写入同父目录的私有 staging；
6. 所有 staging 成功后逐个 atomic install；
7. 记录事务新建的目标和 backup；
8. Core load 成功则提交并清理 backup；
9. Core load 失败则只回滚本事务更改。

已有本地存档默认优先，不在启动时覆盖。若 ROMX slot 与已有本地内容冲突，记录为已绑定但不恢复；写回前必须确认它仍是该 slot 的唯一映射。

### 8.2 平台目标路径

- PSP：由 libromx 认定的 PSP slot 映射到 `save_root/PSP/SAVEDATA/<validated-slot-name>`；
- 3DS Title Save：组件根据 Core/用户目录配置映射到对应 title save root；
- 3DS ExtData：读取 libromx 返回的 scope/extdata ID，再映射到 Core 的 native extdata root；
- 普通单文件：优先匹配 Core 明确公布的 savefile path；不能仅凭 basename 将多个 slot 合并；
- unknown/private/unspecified platform：容器仍可做结构检查，但默认禁止自动启动和 SAVE restore/write-back，并提示 registry 不受支持。

### 8.3 CHEAT

只恢复明确选中的 CHEAT object。若组件约定自身 key 为 `retroarch`，读取和写回都必须使用同一 key。不能“读取遇到的第一个 CHEAT、写回到另一个 key”。多个候选时要求用户选择或采用文档化的确定规则。

### 8.4 显式写回

默认仍为显式写回，不在 content unload 时静默修改 ROMX。Quick Menu 动作：

- `save`
- `cheat`
- `stats`
- `all`

动作由通用 component menu handler 转发到 active component，RetroArch 菜单代码不直接调用 `romx_persistence_*`。

## 9. 扫描、playlist、history 与封面

### 9.1 Scanner

`scan_identity()` 返回：

- logical extension；
- title/serial/platform；
- entrypoint size；
- metadata CRC（优先）；
- RIDX entrypoint CRC（fallback，只有 `HAS_CRC32` 时有效）；
- identity availability，不用 `0` 冒充有效 CRC。

同一路径在一次扫描任务中只 inspect 一次。缓存至少以 canonical path、file size、mtime 和可用的 immutable identity 失效；mutable 写入不应迫使重新扫描 payload。

### 9.2 Playlist/history

- playlist path 和 history path 永远写 source `.romx`；
- label 可以使用 metadata title，但只在 Core load 成功后发布；
- core path、materialized cache path 和 `romx://` path 永远不能写入 playlist/history；
- 不再向 `content_file_info_t` 增加 ROMX 条件字段，source path 从 active component session 查询。

### 9.3 Thumbnail

1. 只读取 cover info，不为取封面强制复制全部 metadata JSON；
2. 用 cover SHA-256 生成最终缓存名；
3. sibling temp 写入成功后 atomic rename；
4. 缺失/损坏 cover 是非致命状态，回退到 RetroArch 正常数据库封面；
5. 同一路径被编辑后，只要 cover bytes 改变，缓存名必须改变。

## 10. 源码迁移计划

| 当前文件 | 目标处理 |
| --- | --- |
| `romx/romx_frontend.*` | 拆成 component/inspect/launch；公共头不暴露 libromx。 |
| `romx/romx_vfs.*` | 迁入组件 VFS overlay，以串行 operation guard 保护 cursor 生命周期；若改为细粒度并发，再增加 session/operation 引用计数。 |
| `romx/romx_save_adapter.*` | 保留 Host transaction 和 slot view；删除 profile 重实现和手工 RMBL size。 |
| `romx/romx_persistence.*` | 使用显式单外层 session context；共享状态只在 active component 生命周期内有效；使用安全 Host transaction 和 STATS delta。 |
| `retroarch_types.h` | 撤销 ROMX `source_path` 字段。 |
| `tasks/task_content.c` | 将约 350 行 ROMX 分支替换成少量通用 begin/prepare/stage/commit/abort 调用。 |
| `tasks/task_database.c` | 用通用 scan provider；不改变 strict/loose 状态机。 |
| `core_info.c` | 用一次性通用 logical extension resolution；比较器不做 I/O。 |
| `runloop.c` | 用一处通用 component VFS getter 替代逐字段 ROMX `#ifdef`。 |
| `manual_content_scan.c` | 从 component registry 获取容器扩展名，不写 ROMX 专用 token parser。 |
| `gfx/gfx_thumbnail.c` | 调用通用 thumbnail provider。 |
| `menu/*`、`intl/*` | 使用一个通用 component submenu/action handler；动作文本由组件 action descriptor 提供。 |
| `Makefile.common`、`qb/*` | 构建/加载 component；主程序不再直接 `-lromx`。 |

侵入控制验收指标：

- 目标仍是：`romx/` 和通用 component framework 之外，非本地化新增代码不超过 200 行；
- 目标仍是：`tasks/task_content.c` 的 ROMX/组件专用改动不超过 40 行；
- 目标仍是：`tasks/task_database.c` 的组件专用改动不超过 30 行；
- `runloop.c` 的 VFS 改动目标不超过 10 行；
- `retroarch_types.h` 不含 ROMX 字段；
- 除构建注册、测试和文档外，`romx_`/`ROMX_` 标识不得出现在组件目录外；
- RetroArch 上游同步冲突应集中在少数稳定 hook，不再散布于失败分支。

本轮已经完成 ABI、loader、VFS、persistence 和普通内容隔离，但 Host 集成的当前 diff 仍大于上述目标（以 `git diff --stat` 为准）。发布前应把 task-content/task-database 的剩余流程收敛为 facade hook；在此之前不得把“侵入指标已达标”写入版本说明。

## 11. 分阶段提交顺序

每一阶段单独提交并可测试，不要做一个无法审查的大提交。

1. **冻结依赖**：libromx 优化形成干净 commit/tag；记录 revision、头文件和静态库来源。
2. **通用组件 ABI**：加入 registry、loader、host services 和 mock component 测试，不接入 ROMX。
3. **ROMX inspect/launch 组件化**：迁移 session、mapping、materialize，不接 persistence。
4. **VFS overlay**：替换 `runloop.c` 的逐字段分支，补并发/生命周期测试。
5. **content transaction**：重写 `task_content.c` 接入并删除 `retroarch_types.h` 字段。
6. **scan/thumbnail**：恢复 strict/loose 语义，加入 cover hash cache。
7. **SAVE/CHEAT/STATS**：接入新 save catalog、自动容量/单次紧凑 fallback、STATS delta 和安全事务。
8. **通用菜单动作**：替换 ROMX 专用 enum/callback 扩散。
9. **清理旧实现**：删除重复函数、旧构建链接和过时文档；保留并审查 facade 内“单次 Core load 一个外层容器”的显式生命周期状态。
10. **平台验收**：macOS/Linux/Windows 动态组件，至少一个 static-only 配置，真实 Core smoke test。

## 12. 构建与依赖要求

### 12.1 libromx 发布门禁

在锁定 revision 后执行：

```sh
cmake -S ../libromx -B /tmp/libromx-release \
  -DROMX_BUILD_TESTS=ON \
  -DROMX_CONFORMANCE_FIXTURE_DIR=/path/to/ROMX-Standard-Spec/tests/fixtures \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/libromx-release --parallel
ctest --test-dir /tmp/libromx-release --output-on-failure
git -C ../libromx status --porcelain
```

最后一条必须为空。RetroArch 构建记录中写入 libromx commit，不依赖开发机上碰巧存在的 `/usr/local/lib/libromx`。

### 12.2 动态组件安全

- 只从 RetroArch 已配置的 components 目录加载，不搜索当前工作目录；
- 使用绝对路径调用现有 `dylib_load()`；
- 校验唯一导出符号、ABI version、struct size、component ID；
- ABI 不匹配时禁用 ROMX 并给出一次明确日志，普通内容继续工作；
- 默认不在运行期卸载组件；
- 组件使用 hidden visibility，只导出 getter；
- `ROMX_COMPONENT_LIBS` 必须指向已冻结 revision 的 `libromx.a`；libromx 静态链接进组件，避免 Host 和其他组件产生符号/version 冲突。构建后用 `otool -L`/`ldd` 验证组件没有 libromx 动态依赖。

### 12.3 静态平台

`HAVE_ROMX_COMPONENT_STATIC` 只改变注册和链接方式，不改变组件源文件、状态机和测试。不得维护一个删减版“控制台 ROMX loader”。

## 13. 必须新增的测试

本轮已经落地的适配器套件位于 `romx/tests/romx_frontend_test.c`，当前为 137/137，覆盖 mapped/materialized/VFS、私有 file-format、普通 ROM/ZIP、flat/PSP/3DS Title Save、容量回滚、STATS session delta、POSIX symlink cleanup、封面和重复生命周期。下列条目中标注为发布门禁的真实 3DS/ExtData 目录、Windows reparse swap、STATS 连续写回、并发和错误 ABI loader 仍应由平台 CI 或 Host integration test 补齐；不能用适配器单元测试的通过替代它们。

### 13.1 组件加载

- 组件不存在：RetroArch 正常启动，普通内容不受影响；
- ABI version/struct size 错误：拒绝组件，不崩溃；
- 错误导出符号：拒绝组件；
- 动态/静态模式行为一致；
- 主程序退出前 VFS 回调地址始终有效。

### 13.2 内容与 VFS

- 单文件 mmap、mmap 不可用时 bounded buffer、need-fullpath materialize；
- CUE/GDI/M3U/CCD 的 entrypoint 和 sidecar；
- Core 声称 VFS 但实际不使用时给出可操作错误；
- `../`、绝对路径、双斜线、反斜杠、冒号、过期 namespace；
- 普通 ROM/ZIP 的 open/read/seek/stat/readdir 完全回归；
- 多外层 ROMX item 在任何文件变更前拒绝；
- VFS read 与 content unload 并发测试。

### 13.3 SAVE

- 普通每文件 slot；
- PSP 有效/无效 `PARAM.SFO`；
- 3DS Gateway 单文件；
- 3DS Citra/Azahar Title Save（适配器 fixture 已覆盖）；
- 3DS ExtData；
- SaveDataFiler strict shape；
- 多文件 candidate 保持为同一 object/slot；
- 新 object 使用 libromx 自动 margin；紧凑空间只做一次 exact fallback；
- replacement 超出旧 extent 时旧对象 byte-for-byte 不变；
- 两个 stable ID 映射同一 Host 路径时整批拒绝；
- symlink/reparse swap 不跟随、不越界删除（POSIX fixture 已覆盖，Windows reparse 仍需 CI）；
- staging 中途失败不留下部分目标；
- unknown namespace 在组件操作后仍保持不变。

### 13.4 STATS/CHEAT

- `romx_mutable_stats_merge_session_delta()` 的 counter/timestamp 合并和 safe-integer overflow（适配器 fixture 已覆盖）；
- 同一 session 连续写回两次不重复 launch count；
- 第二次只加入上次 checkpoint 后的 runtime；
- 外部更新 generation 后重新读取并 merge；
- safe-integer overflow 返回错误且旧 STATS 不变；
- 多 CHEAT object 不误读/误写另一个 key；
- SAVE 成功、CHEAT 失败时 `all` 动作明确报告部分结果，不谎报全成功。

### 13.5 Scanner/thumbnail

- strict 未命中不添加；loose/manual 可添加；
- metadata CRC、entry CRC、无 CRC 三种状态；
- 一次扫描每路径只 open 一次；
- 同路径替换 cover 后缓存失效；
- cover 缺失/损坏回退普通 thumbnail；
- mutable-only 写入不会改变 playlist identity。

## 14. 验收命令

完成重构后至少执行：

```sh
git diff --check
git -C ../libromx status --porcelain

rg -n '#include[[:space:]]+[<"].*romx|\bromx_|\bROMX_' \
  --glob '!romx/**' --glob '!docs/**' --glob '!tests/**'

make -C romx/tests clean
make -C romx/tests ROMX_PREFIX=/tmp/libromx-release
make -C romx/tests check
make -C romx/tests clean
make -C romx/tests ROMX_PREFIX=/tmp/libromx-release SANITIZER=address,undefined
ASAN_OPTIONS=halt_on_error=1 make -C romx/tests check

make -o config.mk -j4 \
  ROMX_LIBS=/tmp/libromx-release/libromx.a \
  ROMX_COMPONENT_LIBS=/tmp/libromx-release/libromx.a
make -o config.mk -j4 ROMX_COMPONENT_MODE=static \
  ROMX_LIBS=/tmp/libromx-release/libromx.a
make -o config.mk -j4 \
  ROMX_LIBS=/tmp/libromx-release/libromx.a \
  ROMX_COMPONENT_LIBS=/tmp/libromx-release/libromx.a

ctest --test-dir /tmp/libromx-release --output-on-failure
```

桌面动态模式再检查主程序没有直接包含 libromx：

```sh
nm -g retroarch | rg 'romx_(reader|writer|mutable|save_catalog)' && exit 1 || true
```

macOS 可补充：

```sh
otool -L retroarch
otool -L retroarch_romx_component.dylib
```

Linux 使用 `ldd`/`readelf -d`，Windows 使用 `dumpbin /DEPENDENTS`。预期是 libromx 符号只在 ROMX 组件内部，RetroArch 主程序不依赖系统 libromx。

## 15. 完成定义

同时满足以下条件才算完成：

- libromx revision 干净、可复现、测试通过；
- RetroArch 主程序与 Core 不直接调用 libromx；
- ROMX 组件可独立缺失、加载失败或 ABI 不匹配，而普通内容正常；
- current single/multi-file launch 能力无回归；
- 3DS SAVE 使用 libromx 新 profile 并有测试；
- SAVE/CHEAT 容量逻辑采用新自动 margin + 单次 exact fallback；
- STATS 使用 session delta merge；
- destination collision 和 symlink/reparse 测试通过；
- strict scan 语义恢复；
- cover cache 以内容 hash 失效；
- 外部源码侵入达到第 10 节指标；
- 文档和测试矩阵不再描述旧的“非 PSP 一律单文件”行为；
- ROMX 相关提交没有夹带 QuartzCore、shader/build artifact 等无关改动。

## 16. 可直接复制给开发人员的任务说明

```text
任务：将 retroarch-romx 的 ROMX 0.2.0 支持重构为独立内容组件，并适配已优化的 libromx。

背景：
你不需要了解当前 retroarch-romx 的历史修改。请先阅读
docs/ROMX-component-refactor-development-guide.zh-CN.md，并以其中的责任边界、
状态机、迁移顺序和验收测试为准。当前实现可作为可运行基线，但不得绕过通用 ABI
或把 RetroArch 私有策略下沉进 libromx。

硬性约束：
1. 不修改 libromx 源码，不在 vendored libromx 上增加 RetroArch 私有功能。
2. libromx 必须锁定到一个干净、不可变、测试通过的 revision。
3. ROMX 格式、RIDX、RMBL、SAVE profile、CRC、mutable commit 只调用 libromx 公共 API。
4. 桌面平台构建独立 ROMX 动态组件；静态平台使用相同 ABI 和相同组件源码注册。
5. RetroArch 主程序、Core、scanner 和 menu 不得直接包含 <romx/romx.h>。
6. 不修改 Libretro ABI，不要求普通 Core 解析 ROMX。
7. playlist/history 保存 .romx source path；Core 只看到 entrypoint 原格式。
8. 多文件 ROMX 使用标准 Libretro VFS；无真实 VFS 支持时明确失败。
9. Host restore/write-back 必须 staging、冲突预检、symlink-safe、可回滚。
10. 默认显式写回，不在 content unload 时静默改写 ROMX。

必须采用的 libromx 新能力：
- SAVE host import：romx_save_catalog_*；
- SAVE slot/layout：romx_mutable_bundle_get_save_layout/get_save_slot*；
- bundle 新对象：data_capacity=0 使用库的自动 margin；仅在空间不足且对象不存在时，
  用 measure API 做一次 exact-capacity fallback；
- STATS：romx_mutable_stats_merge_session_delta；
- replacement：复用原 fixed extent，超出时保留旧对象并返回明确错误。

必须删除：
- RetroArch 内的手工 RMBL size 计算、固定大 capacity、减半重试循环；
- PSP/其他平台二分的 SAVE 分类；
- 跨 content task 复用的 ROMX 全局 session/persistence 状态；Host facade 可以明确限制一次 Core load 一个外层容器，但状态必须在该 session 生命周期内创建、提交或回滚；
- task_content.c 各失败分支散落的 ROMX cleanup；
- retroarch_types.h 的 ROMX source_path 字段；
- runloop.c 中逐个 VFS 函数的 HAVE_ROMX 分支；
- 不安全的递归 remove_tree；
- scanner strict 模式下强行添加未命中 ROMX 的行为。

提交顺序：
依赖冻结 → 通用组件 ABI/loader → inspect/launch → VFS → content transaction →
scanner/thumbnail → SAVE/CHEAT/STATS → 通用菜单 → 清理和平台验收。
每一步单独提交，保持可构建、可测试。

完成标准：
执行文档第 14 节命令；补齐第 13 节全部测试；满足第 10 节侵入指标；
动态模式下 RetroArch 主程序不得直接导入 libromx 符号；普通 ROM/ZIP/VFS 回归必须通过。
```
