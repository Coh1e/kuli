# kuli 需求规格

## 1. 背景与定位

> **kuli** 是一个把**多源**与**多机**的能力**融合**进**对等**网络的工具——**感知**与**分析**为先，**IR**为骨，**编排**与**执行**可复现、可分布。

九字气质 ——「融合 / 对等 / 感知 / 分析 / IR / 编排 / 执行 / 可复现 / 分布式」。

ref/ 下四个项目（bdpcs-cpp / mozi / luban / luban-bps）的能力被 kuli **吸收**——它们的独立身份在 kuli 视野里不再存在，只作为源代码参考拆进 kuli 自己的代码树。

## 2. 干系人 / 使用者

- **MVP 单人主**：1 个开发者，自用，Windows 11 主机
- **v0.2+ 小组**：3–10 人，共享 mesh，互相 publish view / capability
- **不在 MVP 内**：多租户、企业 SSO、审计合规、付费分发

## 3. 三主业

### 3.1 主业 1 — 跨源文件聚合 + view + 双向同步

把 SSH/SFTP、FTP、WebDAV、Baidu Netdisk、LocalFS 上的文件抽象成统一可查询数据集；用户写 view，view 物化到本机或远端 peer 的一个文件夹，可正常浏览、双向同步。

**MVP backend**：BaiduNetdisk / LocalFS / SFTP / WebDAV / FTP（5 个）

**用户故事**：
- US-1.1 我想一行命令在百度网盘 + SFTP 服务器上同时搜"过去 30 天 > 1 GiB 的视频"
- US-1.2 我想把上面那条查询保存成 view，物化到 `~/views/recent-big-videos/`，里面是占位符；点开某个文件时才从源拉
- US-1.3 我想往百度网盘扔一个 8 GiB 的 ISO，kuli 自动 zstd 分卷成 4 GiB 一片上传，可断点续传，下载时自动重组校验 MD5
- US-1.4 我想把 view 发布出去，组里其他 peer 用 `view@me:recent-big-videos/foo.mp4` 引用
- US-1.5 我在 view 里改了某文件，kuli 自动同步回源；多源冗余时 fan-out

### 3.2 主业 2 — 开发环境复现

通过 Nix-flavored 内容寻址 derivation + profile + generation，在 kuli 网络任意 peer 上复现整套开发环境。

**MVP 内容**：Windows MSVC 工具链（vs_BuildTools.exe 静默装）+ mingit + cmake + ninja + vcpkg + llvm-mingw

**用户故事**：
- US-2.1 我在干净的 Win 上一行 `iwr ... | iex` 装完 kuli，再 `kuli bp apply bootstrap` 拿到能编译 C++ 的全套工具链
- US-2.2 装第二台机时同 `kuli bp apply bootstrap` → 同 store 内容相同 hash → 同步骤
- US-2.3 升级 cmake 版本后只动一处 derivation，新生成 generation；不爽就 `kuli generation rollback`
- US-2.4 我想给 Java 项目用相同框架；写一份 `jdk.luau` 派生，bootstrap 里加进去就行
- US-2.5 全程零 UAC，shim 进 `~/.local/bin/`，绝不污染 PATH 系统级

### 3.3 主业 3 — 远程能力前端 + 感知

人坐在本机，**不开新 SSH session**，直接通过 kuli 把远端的命令执行、文件浏览、状态查询融合进当前终端。

**用户故事**：
- US-3.1 我想看 prod 机器现在的负载，不起 ssh session，`kuli @prod uptime` 一行出来
- US-3.2 我想在 prod[1-10] 上同时跑 `uname -a`，结构化对比
- US-3.3 我想 `kuli '@a:cat /file | @b:gunzip | @local:save /out'`，字节流不经多余中转
- US-3.4 我有台机器只开了 8443 wss，不能 ssh；kuli 用 wss transport 同样能跑
- US-3.5 我想给目标 C 推 agent，但本机不能 cross-compile C 的平台；mesh 里 B 有工具链，kuli 自动让 B 编译后送给 C（C 不必出网）—— 这条是"能力聚合 / 黑心"的体现

## 4. 横切功能需求 R-F-XX

| ID | 需求 | 主业 |
|---|---|---|
| R-F-01 | 统一 BackendDriver 抽象：read / metadata / write / 流式上下载 | 1 |
| R-F-02 | 元数据按 backend 拉到本机 SQLite 索引，TTL + 增量刷新 | 1 |
| R-F-03 | SQL 风 + DSL 风双前端共享 `MultiSourceFileQuery` IR | 1 |
| R-F-04 | view 物化策略默认惰性，访问时才回源 | 1, 2 |
| R-F-05 | view 私有 / public 两态，public 通过 `view@<peer-id>:` URI 引用 | 1 |
| R-F-06 | view ↔ backend 双向同步 + 冲突解析 + 多源 fan-out | 1 |
| R-F-07 | 大文件 zstd 流式分卷 + sidecar manifest + 断点续传 | 1 |
| R-F-08 | Nix-flavored store / derivation hash / profile / generation 四层 | 2 |
| R-F-09 | 蓝图运行时支持 fetch / composite / withFiles 三种 builder | 2 |
| R-F-10 | scripture 包格式（basenames + adapters + resources），shim 写 `~/.local/bin/` | 2, 3 |
| R-F-11 | 一行装：Win `iwr ... \| iex`，POSIX `curl ... \| sh` | 2 |
| R-F-12 | 可插拔 transport：ssh / wss / reverse / local-subprocess | 3 |
| R-F-13 | 远端 agent 与本机 kuli 同源（同二进制 + IR + engine），按需流转 | 3 |
| R-F-14 | 跨平台 ps / ip / lsof / HostFacts native API（不解析文本） | 3 |
| R-F-15 | session-mode：多命令 / streaming 自动升 session；单命令零 bootstrap | 3 |
| R-F-16 | meshell：fan-out + 单源单汇跨 host pipe + capability 内联约束 | 3 |
| R-F-17 | Luau orchestration API（`kuli.findBuilders` / `parallel` / `exec`） | 3 |
| R-F-18 | CapabilityRecord schema + Ed25519 签名 + 单调版本 | 横切 |
| R-F-19 | Capability pairwise sync on-connect（MVP）；epidemic gossip v0.2+ | 横切 |
| R-F-20 | Capability routing L1（本地 cache）+ L2（mesh 查询）+ matcher | 横切 |
| R-F-21 | RouteJob：task → capable peer 路由 + fallback `fail`/`local`/`prompt` 三档 | 横切 |
| R-F-22 | Evidence session：每次 Run 写 `.kuli/sessions/<id>/`，支持 `kuli resume` | 横切 |
| R-F-23 | rustc 风格诊断（error[Exxx]: ...），engine 端统一渲染 | 横切 |
| R-F-24 | 22 个 MVP IR family（见设计规格） | 横切 |

## 5. 非功能需求 R-NF-XX（八条不变量）

| ID | 不变量 | 含义与判据 |
|---|---|---|
| R-NF-01 | 零 UAC / HKCU only / XDG-first | shim 进 `~/.local/bin/`，toolchain bin 绝不进系统 PATH；任何"需要管理员"路径拒绝并 surface fix-hint，不自动 elevate |
| R-NF-02 | core 静态链接 | `/MT` on Win；干净 OS 解压即跑；CI `dumpbin /DEPENDENTS` / `ldd` 拒系统运行时之外的动态依赖 |
| R-NF-03 | C++ 触磁盘，Luau 解析 | Luau 沙盒只暴露 base/math/string/table/utf8；`io`/`os`/`require`/`package`/`load*` 全 nil；C++ core 不引 `lua.h` / `luau.h`；binding 收敛到 `src/luau_frontend.cpp` 单 TU |
| R-NF-04 | DSL 纯函数化 | 蓝图 Luau 函数给同 `ctx` 必产 hash 相同的 derivation |
| R-NF-05 | 跨平台 day-1 干净 | lib 选型不绑平台（libuv / libcurl / libssh2 / OpenSSL）；Win32/POSIX API 隔离到 `src/platform/`；Linux/macOS 优先级低但实现必须可编译 |
| R-NF-06 | Lazy by default | derivation 求值、view 物化、metadata 采集、远程 agent bootstrap、scripture 加载、capability gossip 全部默认懒；eager 必须显式（`--refresh` / `--materialize` / `--prefetch`） |
| R-NF-07 | Peer-equal architecture | 任何 kuli 实例代码上对称；capability 差异来自 policy/UI 层；client/server 硬编码是 bug |
| R-NF-08 | Micro-core + tiered extension | C++ core 只做计算密集 + 平台/库抽象 + IR runtime + transport + 极简内置 executor，绝不含具体 backend / scripture / 业务逻辑；所有能力以 Luau 叠层扩展，按需流转 |

## 6. 范围外（R-OS-XX）

| ID | 不做 / 推后 | 原因 |
|---|---|---|
| R-OS-01 | DHT-based peer 发现 | < 1000 peer 是过度工程；MVP 静态 peer list |
| R-OS-02 | 多租户 / 企业 SSO | 个人 / 小组用，加不上 |
| R-OS-03 | WASM 扩展运行时 | v1+；Luau 表达力 + 第三方需求双触发器后再上 |
| R-OS-04 | Linux/macOS 发布 | 优先级低；实现必须可编译，但发布物只出 Win |
| R-OS-05 | 7z / squashfs 分卷格式 | v0.x 只 zstd 流式；留 codec 字段 |
| R-OS-06 | 多 peer 协作写 view | v0.3+；MVP 单向只读 public view |
| R-OS-07 | Native MSVC silent install 全 Win build 覆盖 | MVP 失败时 doctor 给手动装命令 |
| R-OS-08 | 自定义 scripture registry | v1+；v0.x 走 kuli-bp 内容寻址 store + GitHub source |
| R-OS-09 | mDNS / rendezvous discovery | v1+ |
| R-OS-10 | 复杂 meshell（多级 pipe / 条件 / 循环 / 子 shell） | v0.2+；MVP 子集见 R-F-16 |

## 7. 接收标准（MVP）

- 三主业每条用户故事在 conformance 测试里跑通（见 `docs/design.md` § 验证）
- 八条不变量全部由 CI 守护（静态链接 / 沙盒负向 / 跨平台编译 / hash 确定性 / lazy 默认）
- 干净 Win 11 上 `iwr ... | iex` 装完到第一个 `kuli view materialize` 成功，总耗时 < 10 分钟
