# kuli 设计规格

> 本文是 `docs/requirements.md` 的实现指导。如果你只想了解 kuli 是什么，先读 `docs/concepts.md` 和 `docs/glossary.md`。
>
> 大节顺序：架构总览 → C++/Luau 边界 → 仓库布局 → IR 体系 → View 系统 → Capability 层 → Transport 层 → Luau DSL → Scripture → Meshell + Orchestration → Security → 跨平台策略 → 验证

## 1. 架构总览

### 1.1 黏菌网络（slime mold mesh）

kuli 网络上**每个实例都是 peer**。能力差异来自环境（人在 / headless / 资源极限），不来自架构。

| 旧框架（fat client + thin agent） | 黏菌框架 |
|---|---|
| 本机 kuli 是 fat client | 人在的 host 是 super-peer |
| 远端 agent on host C | peer C（按 capability 自适应） |
| transport client → server | peer 间双向通道，任何 peer 都可起也可被起 |
| scheduler 看 inventory 找 builder | task 在 mesh 里**传播**到 capable peer |
| 感知 HostFacts | peer 间 capability lazy query；v0.2+ gossip |
| hosts.toml | peer list（Ed25519 公钥识别） |

代码里**不允许**硬编码"我是 client / 我是 server"。每个 kuli 二进制启动就有 `kuli daemon` 子能力 listen / accept / initiate。MVP 静态 peer list + on-demand capability query；v0.2+ 上 epidemic gossip；v0.3+ 上 rendezvous / mDNS。

### 1.2 能力聚合（黑心内核）

三主业到齐后涌现的底座属性：网络上不同 peer 提供不同能力，kuli 把它们当一个**能力池**自动路由 task。

参考：Nix remote builders / distcc / Bazel remote execution。kuli 把它做成底座一等公民——因为感知 + 派生 + 传输 + view 四件零件本来就齐。

MVP day-1 就基于 capability cache + 用户 matcher（Luau 函数）选 builder；不是手工配。fallback 三档：`fail` / `local` / `prompt`。

### 1.3 分层

```
┌─────────────────────────────────────────────────────────┐
│ 用户面：CLI (basename shim) / meshell / Luau orchestrator │
├─────────────────────────────────────────────────────────┤
│ Scripture 层：basename → adapter 映射                     │
├─────────────────────────────────────────────────────────┤
│ Adapter 层（Luau）：CLI 解析 → IR；backend 协议；matcher    │
├─────────────────────────────────────────────────────────┤
│ Engine（C++）：IR 校验 → plan → execute → 渲染 diagnostic   │
├─────────────────────────────────────────────────────────┤
│ Runtime primitives（C++）：transport / sense / store /     │
│   vault / bp / capability / scheduler / projfs / crypto    │
├─────────────────────────────────────────────────────────┤
│ Platform 抽象（C++）：libuv / libcurl / libssh2 / OpenSSL  │
└─────────────────────────────────────────────────────────┘
```

## 2. C++ ↔ Luau 边界

### 2.1 C++ core（必须）

- IR 协议（schema 校验 + 序列化）
- Luau runtime + sandbox + binding（单 TU）
- Transport（libuv-based：TCP/QUIC/SSH/WSS）
- Crypto primitives（sha256/md5/ed25519/TLS via OpenSSL）
- HTTP/SSH/SFTP transport primitives（libcurl / libssh2）
- ProjFS/FUSE provider 抽象
- Content-addressed store FS 层
- SQLite（metadata + capability cache）
- 压缩 primitive（zstd）
- 系统感知 primitive（跨平台 ps/ip/lsof/host facts native API）
- 极简内置 IR executor（基础 op 不依赖 Luau 也能跑）

### 2.2 Luau 外围（按需流转）

- 所有 backend driver 协议胶水（baidu / sftp / webdav / ftp）
- 所有 scripture（cloud-storage-mgr / find / grep / cpp-dev-helper / ssh-devops-helper / ...）
- view 高层语义
- blueprint stdlib（`lib.fetchGitHubRelease` / `composite` / `withFiles` / `merge`）
- 感知收集策略
- 调度器 matcher

### 2.3 切分原则

**C++ 做计算密集 + 平台/库抽象；Luau 做协议胶水 + 业务逻辑 + 用户扩展**。bdpcs-cpp 代码相应拆：crypto/range-download primitive 进 core；endpoint 构造 / 签名 / 错误解释 走 `baidu.luau` adapter。其它 backend 同型。

### 2.4 ABI 边界

```cpp
namespace kuli::engine {
    struct AdapterCall {
        std::string tool_name;
        std::vector<std::string> argv;
        std::filesystem::path cwd;
        nlohmann::json ir_doc;   // kuli/ir/1.0
    };
    struct RawResult {
        int exit_code = 0;
        std::vector<std::string> lines;
        std::string raw_stderr;
        std::optional<nlohmann::json> structured;
    };
    class Engine {
        explicit Engine(Mode mode = Mode::Run);
        RawResult execute(const AdapterCall& call);
        nlohmann::json host_facts() const;
        void emit_diag(const nlohmann::json& d) const;
        void set_current_tool(std::string tool) noexcept;
    };
}
```

前端（Luau adapter 或 C++ 内置 executor）只 link `kuli::engine`，不引 `kuli::ir::*` 或 `kuli::host::*`。

## 3. 仓库布局

```
kuli/
├── CMakeLists.txt          # static-link, /MT on Win, vcpkg manifest, C++23
├── CMakePresets.json       # windows-msvc / linux-clang / macos / default
├── vcpkg.json              # libcurl[ssl,http2] / libssh2 / libuv / openssl /
│                           #  nlohmann-json / cli11 / spdlog / zstd / sqlite3 / doctest
│                           # Luau 不走 vcpkg，FetchContent vendored
├── install.ps1             # Win 一行装：iwr ... | iex
├── install.sh              # Linux/macOS 一行装
├── apps/
│   └── kuli/main.cpp      # CLI 路由 + scripture 分发 + meshell 入口
├── libs/
│   ├── kuli-engine/       # ABI 边界，公开 nlohmann_json + kuli-ir
│   ├── kuli-ir/           # 22 IR family + envelope + Stub/LazyRef
│   ├── kuli-luau/         # Luau 沙盒 + binding（lua/luau C API 单 TU 隔离）
│   ├── kuli-transport/    # libuv RPC + libssh2 / libcurl / reverse / local-subprocess
│   ├── kuli-crypto/       # 跨平台 sha256/md5/ed25519/TLS（OpenSSL 包装）
│   ├── kuli-store/        # 内容寻址 store FS 层
│   ├── kuli-vault/        # view 物化的内部存储格式（stub/manifest/index/cache）
│   ├── kuli-projfs/       # ProjFS/FUSE provider 抽象 + native impl
│   ├── kuli-sense/        # 跨平台 ps/ip/lsof/host facts native API
│   ├── kuli-bp/           # 蓝图运行时（store/derivation/profile/generation）
│   ├── kuli-scripture/    # scripture registry、basename 路由、shim 写入
│   ├── kuli-capability/   # capability record/gossip/routing
│   ├── kuli-scheduler/    # RouteJob + matcher + fallback
│   ├── kuli-meshell/      # mesh DSL parser → Pipeline IR
│   ├── kuli-diag/         # rustc 风格 diag
│   ├── kuli-memory/       # .kuli/sessions/<id>/ evidence + 断点恢复
│   └── platform/
│       ├── win/            # Win32 API 集中地（HKCU、ProjFS native、bcrypt 包装）
│       └── posix/          # POSIX-only（FUSE native、xattr 等）
├── share/kuli/
│   ├── scriptures/
│   │   ├── cloud-storage-mgr/   # basename = ndisk / nput / nget / nls / nrm / nmv
│   │   ├── bp/                  # basename = kuli-bp
│   │   ├── ssh-devops-helper/
│   │   ├── find/                # 内置 find，dangerous-off default
│   │   └── grep/
│   ├── backends/                # backend driver 的 Luau adapter
│   │   ├── baidu/
│   │   ├── sftp/
│   │   ├── webdav/
│   │   ├── ftp/
│   │   └── local/
│   └── builtin-bps/             # 最小内置蓝图（kuli-deps）
├── scripts/
│   ├── dev.ps1
│   └── dev.sh
├── docs/                        # 见本规划六份文件
└── tests/
    ├── unit/                    # doctest，每 leaf 模块一文件
    └── conformance/
```

## 4. IR 体系

### 4.1 协议

- `kuli/ir/1.0` 独立 schema；不复用任何外部 schema
- envelope：`{ schema, kind, node, descent_trace, expansion_trace, sources }`
- 每节点带 `at: "<URI>"` 字段（read / mutation 统一），值取自 URI scheme：`local:` / `view:` / `view@<peer-id>:` / `baidu:` / `sftp:` / `webdav:` / `ftp:` / `@<host>` / 等

### 4.2 双层

多步 mutation（`LargeFileTask` / `ViewMaterialize` / `BidirectionalSync` / `ApplyDerivation` / `Pipeline` / `RouteJob`）都是 L1 意图层 + L2 plan 层。flat IR（`FileQuery` / `ProcessQuery`）单层。

### 4.3 Stub / LazyRef

跨域统一的"延迟引用"原语——`NetdiskOp.List` 返回 stub 数组、`ViewMaterialize` 计划生成 stub 树、`RemoteHostFacts` 第一次给 host 摘要 stub。带 `fetch_on: access | refresh | explicit` 字段。

### 4.4 22 个 MVP IR family

| 类 | IR | 干什么 |
|---|---|---|
| read | `FileQuery` | find/fd 风：按名/类型/mtime/size 查文件 |
| read | `TextSearch` | grep/rg 风：内容搜索 |
| read | `ProcessQuery` | ps 风：进程列举 |
| read | `NetworkQuery` | ip/netstat 风：网络状态（只读） |
| read | `HandleQuery` | lsof 风：打开 fd |
| read | `HostFacts` | host 摘要（os/arch/disk/uptime/load） |
| read | `PackageQuery` | 已装包（winget/apt/brew list） |
| read | `EnvQuery` | 环境变量读取 |
| mutation | `FileOp` | read/write/copy/move/del/mkdir/chmod，跨 backend 通用 |
| mutation | `LargeFileTask` | DAG：compress → split → upload-chunk* → manifest → verify |
| mutation | `ViewMaterialize` | query → view 物化（stub 树 + ProjFS/FUSE 计划） |
| mutation | `BidirectionalSync` | view ↔ backends 同步 + 冲突解析 |
| mutation | `EnvSet` | 写环境变量（HKCU on Win / shell-rc on POSIX） |
| mutation | `ApplyDerivation` | 蓝图派生 realize + profile 切换 |
| mutation | `PublishView` | 把 local view 标为 public 并分配 ID + policy |
| mutation | `Exec` | 在 `at:` 跑 shell 命令，stdout/stderr 流回 |
| mutation | `CapabilitySync` | 跟某 peer pairwise 同步 capability bundle |
| coordinator | `Pipeline` | 顺序 + 流式 pipe（mesh-native） |
| coordinator | `Parallel` | fan-out 同 IR 到多 peer |
| coordinator | `RouteJob` | task → capable peer 路由 + fallback 策略 |
| coordinator | `CapabilityQuery` | mesh 中发起 capability 查询（带 constraints + TTL） |
| transport | `RemoteSession` | session 模式下任意 IR 包进 envelope over transport |
| data | `CapabilityRecord` | 签名 + 单调版本化的 peer capability |
| data | `CapabilityBid` | peer 应答"我能接这个 task" |

v0.2+ 候选：`TextReplace` / `PackageOp` / `NetworkOp` / `Conditional` / `CapabilityBroadcast`。

### 4.5 幂等

每个 mutation 节点带 `idempotency: {key, on_conflict: "skip"|"fail"|"overwrite"}`。engine 维护已成功 op cache（基于 evidence session），重复 op 直接命中。

### 4.6 Evidence session

每次 Run 模式 invocation 写 `<cwd>/.kuli/sessions/<id>/`，含：

```
.kuli/sessions/<id>/
├── input.json          # AdapterCall.argv + cwd + env 摘要
├── ir.json             # 序列化的 IR document
├── plan.json           # L1 → L2 展开后的 plan
├── steps/
│   ├── 0.json
│   ├── 1.json
│   └── ...
└── summary.md          # 人类可读总结
```

**承担断点恢复职责**——中途崩 `kuli resume <session-id>` 直接接续。

## 5. View 系统

### 5.1 用户面唯一一等概念

view = 一个 query + 物化位置。用户写 view，不知道也不需要知道"vault"或别的内部存储术语。

```
kuli view create my-files --query "..." --at local:~/views/my-files
kuli view create archive  --query "..." --at @prod-a:/data/archive
kuli view publish my-files --policy public
```

### 5.2 物化目录的内部结构（vault，实现细节）

```
~/views/my-files/
├── <用户可见的文件树，大部分是 ProjFS 占位符>
└── .kuli/
    ├── manifest.luau    # view 元数据 + stub 清单
    ├── index.db         # SQLite metadata 索引
    └── content/         # 内容寻址 cache（hydrated 后）
```

"vault" 一词只出现在内部代码与本设计规格里；**不进** requirements / glossary / CLI 等用户面。

### 5.3 私有 / public 两种

- **私有 view**：纯本机使用；mesh 上其他 peer 不可见
- **public view**：广播到网络；peer 通过 `view@<peer-id>:<view-name>/<path>` 访问
- 三档 policy：`public`（任何 peer）/ `mutual-trust`（握过手的 peer）/ `shared`（公钥白名单）
- MVP 单向只读；v0.2+ 双向同步；v0.3+ 多 peer 协作写

### 5.4 物化策略（ProjFS 五状态）

| 状态 | 含义 |
|---|---|
| `virtual` | 仅 enumeration 时合成，磁盘上不存在 |
| `placeholder` | metadata 已缓存，内容未拉 |
| `hydrated placeholder` | metadata + 内容都缓存（被访问过） |
| `modified` | 本地修改过，待 sync 回源 |
| `tombstone` | 本地删了，待 sync 转 remote-delete |

ProjFS 是 Windows 上的实现路径；POSIX 用 FUSE；远端 peer 没 ProjFS / FUSE 时 fallback 全物化。

## 6. Capability 层

### 6.1 三件

**(1) Capability Record**——每 peer 自报家门，Ed25519 签名 + 单调版本：

```json
{
  "peer_id": "<pubkey-hash>", "version": 42, "signed_at": ..., "signature": "...",
  "capabilities": {
    "arch": "x86_64", "os": "linux",
    "tools":    { "cmake": "3.27", "msvc": null, ... },
    "internet": true, "reachable": true,
    "tags":     ["build-server"],
    "views":    [{ "id": "...", "policy": "public" }],
    "backends": ["sftp", "baidu"],
    "load":     { "cpu_pct": 12, "mem_pct": 34 }
  }
}
```

**(2) Capability Gossip**——参考 Plumtree / Secure Scuttlebutt EBT：peer 间互换"我知道哪些 peers 的最新版本"vector → 按差求/推 delta。

MVP 简化：只做 pairwise on-connect sync（A 连 B 时双方互换一次 bundle）；v0.2+ 上 epidemic gossip。

**(3) Capability Routing**——混合：

- **L1**：本地 cache hit → 直接选 candidate
- **L2**：cache 不足 → 发 `CapabilityQuery` 到 known peers → 收 `CapabilityBid` → matcher 选最优
- **L3 DHT-based** → MVP 不做，> 1000 peer 才评估

### 6.2 信任 / 防中毒

- Capability records 必须 peer_id 对应私钥签名
- 只接受已 mutual handshake 信任 peer 转发的 records（防 Sybil）
- v0.2+ 加 web-of-trust（attestation chain）

### 6.3 调度器（Scheduler）

`RouteJob` 调度流程：

```
task → 提取 constraints → CapabilityQuery (L1 → L2) →
       Bid 候选 → matcher(Luau, 默认 fn) 评分 → 选最优 peer →
       Exec/RemoteSession → 异常 fallback (fail|local|prompt)
```

matcher 入参：`{ task, bids: [{peer, capability, load, latency_estimate}] }` → 返回 `peer_id`。默认 matcher：满足约束 + 最低 `load.cpu_pct`。

## 7. Transport 层

可插拔 transport，pipeline 一致：

```
Agent runtime（远端 kuli core，agent 模式）
   ↑ IR 协议层（kuli/ir/1.0 JSON）
   ↑ RPC framing（length-prefixed frames，多 channel）
   ↑ Transport（可插拔）
       ├── ssh (libssh2)
       ├── ssh-through-bastion
       ├── wss (HTTPS-only 网络场景)
       ├── reverse (NAT 后 peer 主动外连)
       └── local-subprocess (开发 / docker exec / kubectl exec)
```

MVP 默认 ssh + local-subprocess；wss / reverse 在 v0.2+ 上线。

### 7.1 Agent bootstrap

- 单命令零 bootstrap：在 transport 层直接跑 IR executor 内联 IR（无需远端 kuli 二进制）
- 多命令 / streaming 自动升 session：检测到第二条 IR 时，先 transport.scp 本机 kuli 二进制到远端 cache（`~/.cache/kuli/agent-<sha>`），exec 成 daemon，后续 RPC 走它
- agent 二进制按 `arch + os` cache；存在且 sha256 对就跳过

### 7.2 Session 模式

- session = 一条 transport channel + 一个远端 kuli daemon 进程
- kuli 内部维护 SSH/wss/reverse-tunnel **连接池**（不依赖 OpenSSH ControlMaster）
- TTL 后空闲 session 自动关；显式 `kuli session kill <id>` 立刻关

## 8. Luau DSL（蓝图 + 规格）

### 8.1 蓝图（形状）

一份派生 = 一个返回函数的 Luau 模块：

```luau
-- kuli-bps/blueprints/<name>.luau
return function(ctx)
    return ctx.pkgs.fetchGitHubRelease {
        name    = "<name>",
        owner   = "<gh-owner>",
        repo    = "<gh-repo>",
        version = "<v>",
        sha256  = "<hex>",
        bin     = "<rel-path-to-exe>",
    }
end
```

`ctx` 是唯一参数：

```luau
type Ctx = {
    pkgs:   PackageSet,    -- 懒 proxy：ctx.pkgs.foo 触发加载 blueprints/foo.luau
    lib:    StdLib,        -- 派生组合器
    system: SystemInfo,    -- { os, arch, winVersion }
    source: SourceCtx,     -- { name, root, lockedAt }
}
```

stdlib 首发四件套：

| API | 干什么 |
|---|---|
| `lib.fetchGitHubRelease(args)` | 抓 GitHub release artifact，产 derivation |
| `lib.composite { components }` | 把多个 derivation 捆成一个 |
| `lib.withFiles(drv, files)` | 给 derivation 附加文件部署 step |
| `lib.merge(a, b)` | attrset 浅合并（取代 Nix `//`） |

派生本体：

```luau
type Derivation = {
    kind:      "derivation",
    hash:      string,       -- 输入闭包的 sha256
    name:      string,
    storePath: string,       -- "<hash[:16]>-<name>"
    args:      { [string]: any },
}
```

蓝图源仓库布局：

```
<bp-source>/
├── source.toml          -- 源元数据 + kuli_min 版本
├── blueprints/
│   ├── mingit.luau      -- 每个文件 = 一个返回派生的函数
│   ├── cmake.luau
│   ├── bootstrap.luau   -- 组合派生
│   └── ...
└── resources/           -- 静态资源（dotfiles、profile 等），lib.readResource 读
```

**三条硬规则**（违反则求值失败）：
1. 顶层 `return` 必须是 `function(ctx) -> Derivation`
2. 函数体不准调 `require` / `io.*` / `os.*` / `package.*` / `load*` / `_G`
3. 派生求值必须纯——同 `ctx` 必产 hash 相同的派生

### 8.2 规格（细节）

#### 8.2.1 派生 hash 算法

- 输入：canonical JSON({source_url, source_sha256, builder_cmd, env, name, system_target})
- canonical 规则：key 字典序、UTF-8 NFC、数字标准化、布尔小写、字符串 ECMA-404 转义、无尾随逗号
- 算法：`sha256(canonical_json_bytes)` → 64 hex；storePath 取前 16 字符 prefix
- 明确不进 hash：kuli core 版本、求值时本地时间、`source.lockedAt`
- 同 hash 已存在 store → 短路；不重复 realize

#### 8.2.2 PackageSet 懒解析（`__index` 语义）

- `ctx.pkgs` 是 metatable proxy；`pkgs.foo` 触发 `__index("foo")`
- 解析顺序：(1) 当前 source 的 `blueprints/foo.luau`；(2) 已注册依赖 source；(3) 找不到 → 报错 + "did you mean X?" hint
- 求值缓存：同一次 `apply` 内 `pkgs.foo` 只 load + eval 一次；跨 apply 不缓存
- 循环依赖：求值栈记录访问中 derivation；发现回环报错并打印路径

#### 8.2.3 Luau 沙盒禁用清单

- 全局 nil 化：`require`, `io`, `os`, `package`, `loadstring`, `loadfile`, `dofile`, `getfenv`, `setfenv`, `_G`, `collectgarbage`, `debug`
- 保留：`base`（`pairs`/`ipairs`/`tostring`/`type`/`select`/...）、`math`、`string`、`table`、`utf8`
- 替代渠道：读资源走 `ctx.lib.readResource(path)`；做 shell 副作用走派生的 builder

#### 8.2.4 类型注解策略

- stdlib：强制 `--!strict`，lib API 类型完整
- 用户蓝图：默认 `--!nonstrict`，不强制注解
- 编辑器：Luau LSP 通过 stdlib 的 strict 类型给用户蓝图补全 + 错误提示

#### 8.2.5 lib API 完整签名

```luau
--!strict
type FetchGitHubReleaseArgs = {
    name:         string,
    owner:        string,
    repo:         string,
    version:      string,
    assetPattern: string,    -- Lua pattern
    sha256:       string,
    bin:          string?,
    shimDir:      string?,
    postInstall:  string?,
}
type CompositeArgs = {
    name:        string,
    description: string,
    components:  { Derivation },
    requires:    { Derivation }?,
}
type FileEntry = { mode: "replace" | "patch", content: string }

function lib.fetchGitHubRelease(args: FetchGitHubReleaseArgs): Derivation
function lib.composite(args: CompositeArgs): Derivation
function lib.withFiles(base: Derivation, files: { [string]: FileEntry }): Derivation
function lib.merge<A, B>(a: A, b: B): A & B
function lib.readResource(rel: string): string
```

#### 8.2.6 派生组合器语义契约

- `composite`：新派生，hash 含所有 components + requires + name。realize 时按 `requires → components` 串行
- `withFiles`：新派生，hash = base.hash + files 内容 hash。realize 时先 realize base，再按 entry 写文件**到 profile**（不是 store——文件是 per-user 配置，不内容寻址）
- `merge`：纯 attrset 浅合并，**不产派生**；右覆左；类型 intersect

#### 8.2.7 Realize 阶段 builder 协议

- 一个派生 realize = 临时目录跑 builder → 校验输出 sha256 → 原子重命名进 store
- builder 类型：
  - `fetch`：下载 URL，校验 sha256，解压（zip/tar.zst/tar.gz）到 store path
  - `composite`：先 realize 所有 inputs，符号链接进 store path 子目录（仿 Nix `symlinkJoin`）
  - `script`（v0.2+）：跑 `cmd.exe /c <postInstall>`（Win）或 `sh -c`（POSIX），仅新提取时触发
- 失败：临时目录残骸标 `.failed`，不污染 store；下次同 hash 重试覆盖

#### 8.2.8 冲突与覆盖

- 两个 source 都提供 `foo.luau` → 默认报错；用户可在 `kuli.lock` 显式 pin
- `composite` components 同名 binary → 报错；解决：`lib.withRename(drv, { from = to })`（v0.2+）
- `withFiles` 目标路径已存在且非 kuli 管理 → `mode="replace"` 拒绝、`mode="patch"` 尝试合并

#### 8.2.9 `kuli.lock` 格式

- 每源一段：`[sources.<name>]`，记 `url` / `commit` / 最后求值的派生集 hash
- 每 realize 过的派生一行：`<hash> = { name, source_url, source_sha256, components = [...] }`
- 用途：(1) 复现"上次到底装了什么"；(2) `kuli profile diff` 比较两代

#### 8.2.10 派生与 scripture 的派生接口统一

- `lib.mkScripture { basenames, adapters, resources }` 产 `kind = "derivation"` 派生
- realize 出的 store path 含 `scripture/manifest.luau` + `scripture/adapters/*.luau`
- scripture-registry 引用 store path；basename shim 落到 `~/.local/bin/`——跟工具 shim 同一套机制

## 9. Scripture 机制

### 9.1 包格式

一个 scripture 是个 derivation；realize 后的 store path 长这样：

```
<store>/<hash[:16]>-<name>-scripture/
├── manifest.luau            # { name, version, basenames, adapters, resources }
├── adapters/
│   ├── <basename>.luau      # CLI parse → IR
│   └── ...
└── resources/               # 静态文件（模板、locale 等）
```

`manifest.luau` 示例：

```luau
return {
    name      = "cloud-storage-mgr",
    version   = "0.1.0",
    kuli_min = "0.1.0",
    basenames = {
        ndisk = "adapters/ndisk.luau",
        nput  = "adapters/nput.luau",
        nget  = "adapters/nget.luau",
        nls   = "adapters/nls.luau",
        nrm   = "adapters/nrm.luau",
        nmv   = "adapters/nmv.luau",
    },
    backends_required = { "baidu" },
    capabilities      = { internet = true },
}
```

### 9.2 Basename routing

- `kuli scripture install <pkg>` → realize derivation + 在 scripture-registry 登记
- 对 `manifest.basenames` 每条，写 `~/.local/bin/<basename>` shim：调用 `kuli --basename <basename> -- <args>`
- 也可在系统 PATH 上直接 `<basename> args...` 走 shim，shim 反查 registry 找到 store path → load `adapters/<basename>.luau` → 调 adapter
- adapter 返回 IR → engine.execute → 渲染

### 9.3 Shim

shim 内容（Win，PowerShell 风）：

```powershell
#!/usr/bin/env pwsh
& "<kuli-exe>" --basename "<basename>" -- @args
exit $LASTEXITCODE
```

POSIX：

```sh
#!/bin/sh
exec "<kuli-exe>" --basename "<basename>" -- "$@"
```

shim 由 kuli core 写，**不依赖 PowerShell / sh 是否在 PATH**——shim 文件内首行只是 hint；实际入口由 OS exec 处理。

## 10. Meshell + Luau Orchestration

### 10.1 Meshell（bash-flavor 一行式）

shell-like 骨架，mesh-native 语义：

```
@prod ps -ef                                  # 单 host
@prod[1-10] uptime                            # fan-out
@group:builders uname -a                      # 命名组
@a:cat /file | @b:gunzip | @c:save /file      # 跨 host pipe（直传，不经本机）
@'tools.cmake>=3.20' cmake --version          # capability inline 约束
@route(target=linux-aarch64) kuli-agent      # 路由委派
```

kuli 内置 hand-written recursive-descent parser → `Pipeline` IR → engine 选最优 transit 路径。

### 10.2 Luau orchestration（脚本）

```luau
local builders = kuli.findBuilders { target = "linux-aarch64", needs = { cmake = ">=3.20" } }
for _, b in ipairs(builders) do
    kuli.exec { at = b, cmd = "kuli-build-agent" }
end
local results = kuli.parallel({ "@prod[1-10]", function(h) return kuli.exec { at = h, cmd = "uptime" } end })
```

Luau 在 core 已经存在，复用零成本。

### 10.3 MVP 子集

- 单 host + fan-out + 单源单汇跨 host pipe + Luau 编排（这四件）
- v0.2+：多级 pipe 路径 / 条件 / 循环 / 子 shell

## 11. Security / 沙盒 / 信任 / 权限

### 11.1 Luau 沙盒

见 §8.2.3 禁用清单。负向测试是 conformance 的硬要求。

### 11.2 Peer 信任

- Mutual handshake：A B 互换 Ed25519 公钥并签名 challenge；本机存 `peers/<id>.pub`
- CapabilityRecord 必须 peer_id 对应私钥签名
- 转发：只接受 mutual handshake 信任 peer 转发的 records（防 Sybil）
- v0.2+：web-of-trust（attestation chain）

### 11.3 视图发布权限

- `public`：mesh 任意 peer 可读（但 peer 必须能 reach 本机）
- `mutual-trust`：仅 mutual handshake peer 可读
- `shared`：公钥白名单（`kuli view share <view> --to <peer-id>`）
- 写权限：MVP 全 view 只读 public；v0.2+ 显式 grant

### 11.4 Privilege / OS 权限

- 永远不 elevate；任何"需要管理员"路径打印命令让用户手动做
- HKCU only，绝不写 HKLM
- `~/.local/bin/` shim；从不修改系统 PATH 或 `Program Files`
- 进 windefender 例外建议在 doctor 里以提示形式出现，不自动加

### 11.5 远程 exec 信任边界

- `kuli @<peer> <cmd>`：cmd 由发起方组装，签名后随 RemoteSession 发到 peer
- peer 校验签名 + 检查 sender 是否在白名单（默认：mutual-trust）
- denied 时返 `error[E0301]: peer not in trust list`，**不**给 hint 列出名单

## 12. 跨平台策略

### 12.1 lib 选型不绑平台

- 进程：`libuv` / `boost.process`（之一，倾向 libuv 因 RPC 也用）
- HTTP：`libcurl`
- SSH/SFTP：`libssh2`
- TLS / crypto：OpenSSL
- 文件系统：`std::filesystem`（C++23）
- 压缩：`zstd`
- DB：`sqlite3`

### 12.2 平台隔离

- `libs/platform/win/` 收 Win32 API（ProjFS native、HKCU、bcrypt 包装、`vs_BuildTools.exe` 静默装）
- `libs/platform/posix/` 收 POSIX-only（FUSE native、xattr 等）
- 业务 lib 只依赖 platform 抽象接口，不直接 `#include <windows.h>` 或 `<unistd.h>`

### 12.3 优先级

- MVP 全力推 Win 11；CI 主路径 Win
- Linux/macOS：CI 必须**编译过**，conformance 跑 platform-neutral 子集（仍要绿）
- 任何让 Linux/macOS **不可能**编译的选择都不接受（违反 R-NF-05）

## 13. 验证

### 13.1 构建

- `pwsh scripts/dev.ps1`（Win）/ `bash scripts/dev.sh`（POSIX）一把梭
- presets：`windows-msvc` / `linux-clang` / `macos` / `default`

### 13.2 单元

- doctest，每 leaf 模块一文件
- `ctest --preset windows-msvc -R <name>`

### 13.3 Conformance

- 单 host：`kuli ls @local:/` → `kuli view create` → `kuli view ls` 闭环
- 多 backend：从 baidu + sftp 各拉一些文件聚合到同一 view
- 6 GiB 假文件分卷上传 → 删本地 → `kuli view materialize` 重组 → MD5 一致
- `kuli bp apply main/bootstrap --dry-run` 跑通且不写任何东西
- Nix 风格 store/profile 性质测试：同 derivation 重复 apply 是 no-op；不同 derivation 是新 generation；rollback 后 PATH 上 shim 立刻指向旧 store path
- Luau 沙盒负向测试：蓝图 / scripture 里调 `io.open` / `os.execute` / `require` 必须求值失败、错误指向沙盒边界
- 远程能力：`kuli @local-docker-alpine ps` 走 local-subprocess transport；`kuli @prod uname -a` 走 ssh
- 跨 host pipe：`kuli '@a:cat /file | @b:gunzip | @local:save /out'` 字节流不经多余中转
- Capability gossip：A 连 B 后双方 capability cache 互相包含彼此；matcher 求值正确
- 调度器自动匹配：构造一个 RouteJob 要求 `arch=linux-aarch64`，让 mesh 里只有 B 满足，验证 task 落到 B
- Scripture 装卸闭环：装 cloud-storage-mgr → `~/.local/bin/` 出现 ndisk/nput/nget shim → uninstall 清理干净

### 13.4 静态链接

- CI `dumpbin /DEPENDENTS kuli.exe`（Win）/ `ldd kuli`（Linux）拒系统运行时之外动态依赖

### 13.5 真账号烟测

- 用真 BDUSS / 真 SSH key 跑 UPSTREAM 怪癖表每条；新怪癖加一行带日期/HTTP-status/workaround/commit 的记录
