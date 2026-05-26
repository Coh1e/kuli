# kuli 概念表

> 列出 kuli 里**一等**的实体、定义者、消费者、生命周期、与其它实体的关系。要单词 ↔ 定义，去 `docs/glossary.md`；要怎么操作它们，去 `docs/cli.md`。

## 实体清单

| 概念 | 是什么 | 谁定义 | 谁消费 | 生命周期 | 持久位置 |
|---|---|---|---|---|---|
| **Peer** | mesh 中一个 kuli 实例 | kuli daemon 启动时生成 | scheduler / transport / capability layer | 进程生命周期 + Ed25519 keypair 持久 | `~/.local/share/kuli/identity/` |
| **Capability Record** | peer 自报的能力清单 | peer 自身 | mesh 中所有 peer（按信任） | 单调版本递增 | 本地 SQLite cache + gossip 传播 |
| **Backend (driver)** | 对一类数据源的统一抽象 | kuli 内置 / scripture | 主业 1 / view materialize / sync | scripture 安装生命周期 | `share/kuli/backends/<name>/` |
| **Backend config** | 某 backend 的连接参数 | 用户 | backend driver | 用户显式管理 | `~/.config/kuli/backends/<name>.toml` |
| **IR Document** | engine 入参 | adapter（Luau 或 C++） | engine | per invocation | evidence session 里持久一份 |
| **Evidence Session** | 一次 Run 调用全记录 | engine | `kuli resume` / 诊断 / 审计 | 持久直到 GC | `.kuli/sessions/<id>/` |
| **View** | query + 物化位置 + policy | 用户 | vault / projfs / sync | 用户显式 create / rm | `~/.config/kuli/views/<name>.toml` + 物化目录 |
| **Vault** | view 物化目录内部存储 | bp runtime + projfs | view 用户、projfs provider | 跟 view 同生死 | view 目录的 `.kuli/` |
| **Derivation** | 内容寻址构建单元 | 蓝图 Luau 函数 | bp runtime | hash 不变即永生 | `~/.local/share/kuli/store/<hash>-<name>/` |
| **Store** | 所有 derivation realize 出的根 | bp runtime | profile / scripture-registry | 全局，可 GC | `~/.local/share/kuli/store/` |
| **Profile** | 一组 store path 在 PATH 上的编排 | bp apply | 用户 PATH / shim | 当前一份 + 历代 generation | `~/.local/share/kuli/profiles/<name>/` |
| **Generation** | profile 一个时间切片 | bp apply | rollback / diff | append-only，不删 | profile 下符号链接链 |
| **Scripture** | 用户扩展包 | scripture 作者 | scripture-registry / basename router | derivation 生命周期 | store 一条 derivation |
| **Shim** | `~/.local/bin/<basename>` 小可执行 | scripture install | OS exec | scripture 安装即写、卸载即删 | `~/.local/bin/<basename>` |
| **Session (peer transport)** | 两 peer 间持久通道 | transport layer on demand | engine RPC | TTL 或显式 kill | 进程内 + 连接池 |
| **bp source** | 一组 blueprint 的仓库 | 用户 `bp src add` | `bp apply` | 用户显式管理 | `~/.config/kuli/bp-sources/<name>.toml` |

## 关系图（文字版）

```
User
 ├─ creates → View ────────┐
 ├─ installs → Scripture ──┤
 ├─ applies → Blueprint ───┤
 └─ adds → Peer            │
                           ▼
View ── materializes-via ── ProjFS provider
     ── stores-internal ─── Vault (.kuli/ in view dir)
     ── queries ─────────── BackendDriver(s) via IR FileQuery
     ── publishes-via ───── CapabilityRecord.views[]

Scripture ── packaged-as ── Derivation
          ── registers ──── basenames → Shim(s) in ~/.local/bin/
          ── contains ───── Adapter(s) (Luau)

Adapter ── parses ──────── CLI argv → IR Document
        ── calls ───────── Engine.execute
        ── routes-via ──── at-URI scheme

Blueprint ── evaluates-to ── Derivation
          ── composes ───── via lib.fetchGitHubRelease / composite / withFiles

Derivation ── realized-into ── Store
           ── activated-as ── Profile generation

Peer ── exchanges ─────── CapabilityRecord via Capability layer
     ── connects-via ──── Transport (ssh / wss / reverse / local-subprocess)
     ── runs ──────────── Engine (same binary as super-peer)
     ── may-host ──────── View(s) referenced as view@<peer-id>:<name>

Engine ── reads ────────── IR Document
       ── writes ───────── Evidence Session
       ── dispatches-to ── built-in executor or Luau adapter
       ── stays-up-via ─── Session (when in agent mode)

Scheduler ── consumes ──── CapabilityRecord cache
          ── emits ─────── RouteJob IR
          ── fallback ──── fail | local | prompt
```

## 不混淆的几对

- **View ≠ Vault**：view 是用户面概念；vault 是 view 物化的内部存储格式。用户从不操作 vault，只操作 view。
- **Adapter ≠ Backend**：adapter 是"CLI → IR"的 Luau 模块（含 scripture adapter）；backend 是"IR → 远端数据源"的 driver（亦实现为 Luau adapter，但语义是数据源接入）。Backend 是 adapter 的一种特化用途。
- **Derivation ≠ Store path**：derivation 是描述（hash + builder + inputs），store path 是 realize 后的产物目录。一个 derivation 一对一对应一个 store path（hash 决定）。
- **Profile ≠ Generation**：profile 是命名空间（如 `default`），generation 是该 profile 的某次状态切片。`kuli generation rollback` 是把 profile 的当前指针指回旧 generation。
- **Peer ≠ Host**：peer 是 kuli 概念（带 Ed25519 身份）；host 是 OS 概念（一台机器）。一个 host 上可跑多个 peer（开发 / 测试用），但常态 1:1。
- **Session (peer) ≠ Session (evidence)**：transport channel + 远端 daemon vs. 一次 Run 调用的目录记录。完全不同的东西，命名不幸碰撞——代码里用 `PeerSession` / `EvidenceSession` 区分，文档与用户面看上下文。
