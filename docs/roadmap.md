# kuli 路线图

第一刀切：**让"一行命令把 devkit 那套开发环境复现到本机"跑通**——这条覆盖 source resolver +
内容寻址 store + derivation + Luau 蓝图 + engine + scripture/shim + HKCU env + 配置部署 +
可逆 generation 几乎所有 **主业 2** 底层，且立刻可演示、有现成的验收标的（见
[`scenarios/devkit.md`](scenarios/devkit.md)）。从 day-1 IR + Luau 蓝图形态，吸收 `ref/luban`
的成熟实现。

## M0 — 骨架 + 第一个派生跑通

- 仓库 + CMake + vcpkg + windows-msvc preset
- 核心 lib 骨架：`kuli-engine` / `kuli-ir` / `kuli-luau` / `kuli-store` / `kuli-crypto` / `kuli-bp` / `kuli-diag`
- `kuli::engine::Engine` ABI 边界完成
- 第一端到端：一份最小 Luau 蓝图取一个 GitHub release 工具 → `ApplyDerivation` IR → engine →
  realize 进内容寻址 store → 写 shim 到 `~/.local/bin/` → 工具上 PATH
- 烟测：`kuli --version` / `kuli doctor`

## M1 — devkit 开发环境复现（主业 2 首个落地）

> 完整规格见 [`scenarios/devkit.md`](scenarios/devkit.md)。

- `kuli-bp` 全套：store / derivation hash / profile / generation + `kuli.lock`（sha256 pin）
- `kuli-scripture`：registry + basename 路由 + shim 写入（含 WezTerm 智能 shim、git-lfs 真 exe twin）
- `kuli-bps` sibling repo：devkit 栈蓝图（`bootstrap` / `terminal` / `tools`）用 Luau DSL 写
- `EnvSet` IR（HKCU）+ 配置部署（content + junction 双模式）+ win-registry（右键菜单 / 字体 / 开始菜单）
  + MSVC 环境缓存 + clink 注入
- `install.ps1` / `install.sh`：一行装 → `kuli bp apply` 一条龙
- 吸收 `ref/luban` 各 TU（详见 scenarios/devkit.md「引擎来源」）
- 可逆：`kuli generation rollback` 干净回滚（对标 devkit `teardown`）

## M2 — Backend + View 第一闭环（主业 1）

- 加 BaiduNetdisk backend（拆 bdpcs-cpp 源码：crypto/range-download 进 core，rest 写 `baidu.luau`）
- 加 SFTP backend（libssh2 primitive 进 core + `sftp.luau`）
- `FileQuery` / `FileOp` 跨 backend 通用
- `kuli view create` + `ViewMaterialize` IR：query 命中 baidu + sftp 文件，物化到 `~/views/<name>/`，里面是 placeholder（**先不接 ProjFS，全物化版本验证语义**）
- evidence session 落 `.kuli/sessions/<id>/`

## M3 — 大文件 + ProjFS + LargeFileTask

- `kuli-chunker`：zstd 流式 + 4 GiB 切片 + sidecar manifest
- `LargeFileTask` IR：DAG 表达分卷上传，断点续传
- `kuli-projfs`：ProjFS provider Windows impl；FUSE provider 留 stub
- view 默认走 placeholder + on-demand fetch

## M4 — 远程能力 + Capability 层（主业 3）

- `kuli-transport`：libssh2 + local-subprocess transport；远端 kuli agent bootstrap + cache
- `kuli-sense`：跨平台 ps/ip/lsof/HostFacts native（Win API + procfs + sysctl）
- `kuli-capability`：CapabilityRecord schema + pairwise sync + L1+L2 routing
- `kuli-scheduler`：RouteJob + Luau matcher + fallback 三档
- 主业 3 端到端：`kuli @prod ps` / `kuli @group:builders uname -a` / cross-build agent

## M5 — Meshell + Public View

- `kuli-meshell`：parser → `Pipeline` IR；MVP 子集（fan-out + 单源单汇跨 host pipe）
- `PublishView` IR + `view@<peer-id>:` URI scheme + public view 单向读
- Luau orchestration API（`kuli.findBuilders` / `kuli.parallel` / `kuli.exec`）

## M6+ — Sparks

- 用户 scripture 示例集（cpp-dev-helper / ssh-devops-helper / etc）
- v0.2+：双向 view sync / epidemic capability gossip / wss + reverse transport / TextReplace / 多级 pipe
- v1+：WASM 二级运行时 / mDNS / rendezvous / Linux/macOS 发布

## 验收准则

每个 M 的"完成"定义：
1. 该里程碑用户故事在 conformance 里全绿（M1 即 `scenarios/devkit.md` §接收标准）
2. 八条不变量（R-NF-01..08）由 CI 守护，无新违反
3. `docs/upstream.md` / `docs/sparks.md` 同步更新当周期遇到的新坑 / 新点子
