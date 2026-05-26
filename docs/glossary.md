# kuli 术语表

> 单词 ↔ 定义查询。要看实体关系与生命周期，去 `docs/concepts.md`。按字母（中文按拼音）排序。

## A

**Adapter**
Luau 模块，把某个 basename / 某条 CLI 解析成 kuli IR document。每个 scripture 提供一组 adapter；backend 也以 adapter 形态接入。

**Agent**
远端 peer 上跑的 kuli 进程。与本机 kuli **同二进制 + 同 IR + 同 engine**，只是 basename / 启动模式不同。

**at-URI**
IR 节点上的 `at:` 字段值，描述 IR 操作的位置。scheme 来自字面前缀：`local:` / `view:` / `view@<peer-id>:` / `baidu:` / `sftp:` / `webdav:` / `ftp:` / `@<host>`。read / mutation 都统一用它，不再分 `at` / `target` / `path` 几套字段。

## B

**Backend / BackendDriver**
对外部数据源（baidu / sftp / webdav / ftp / local）的统一抽象。read / metadata / write / 流式上下载五类操作。MVP 5 个。

**Basename**
被 OS 当成入口名的文件名（去掉路径与扩展）。kuli 把 basename 当成路由 key：`find`、`grep`、`ndisk`、`kuli-bp` 都是 basename。

**Basename routing**
scripture 在 `~/.local/bin/` 放一组 shim；执行 `<basename> ...` 时 shim 把控制权交给 kuli，kuli 按 basename 找 adapter。

**Blueprint / bp**
一份 Luau 脚本，产 derivation；可组合（composite / withFiles）。`kuli bp apply <name>` realize 它并切换 profile。luban 的 `bp` 概念被 kuli 完全吸收。

**bp source**
托管一组 blueprint 的仓库（GitHub repo 或本地路径）。`kuli bp src add` 注册。

## C

**Capability**
peer 自报的能力清单：arch / os / tools / backends / views / load / tags。

**CapabilityRecord**
签名 + 单调版本化的 capability 序列化形式。Ed25519 签名，单调 version。

**CapabilityBid**
peer 对一条 task 的应答：能 / 不能、估值如何。

**CapabilityQuery**
mesh 中发起的 capability 查询 IR，带 constraints + TTL。

**Composite (派生)**
`lib.composite` 产的 derivation，把多个 component derivation 捆成一个，realize 时 symlinkJoin。

**Conformance (测试)**
端到端验证 kuli 业务行为的测试集，对应每条用户故事。区别于 unit test。

## D

**Daemon**
kuli 二进制以 `kuli daemon` 子模式运行；listen / accept / initiate peer 连接。所有 kuli 实例都自带这个能力，不分 client/server。

**Derivation**
内容寻址的构建单元。hash = 输入闭包 sha256；realize 一次后存 store。

**Descent trace**
adapter 在构造 IR 时记的"为什么走到这步"轨迹；engine 在 dry-run / 诊断时回显。

## E

**Engine**
C++ 一等 ABI：`kuli::engine::Engine::execute(AdapterCall) → RawResult`。一切前端（CLI / meshell / Luau orchestrator / 远端 agent）只通过它发 IR。

**Evidence session**
每次 Run 模式调用写的 `.kuli/sessions/<id>/`，含 input/ir/plan/steps/summary。崩了可 `kuli resume`。

## F

**fan-out**
同一 IR 并发发到多个 peer。meshell 写 `@prod[1-10] uptime` 是 fan-out 例。`Parallel` IR 表达。

## G

**Generation**
profile 的一个时间切片。Nix-flavored。`kuli generation rollback` 回上一代。

## H

**HostFacts**
host 摘要 IR：os / arch / disk / uptime / load。

## I

**IR / IR document**
kuli 内部数据交换格式。schema = `kuli/ir/1.0`。envelope + kind + node + at + descent_trace。

**IR family**
一类 IR，对应一类 OP。MVP 22 个。

## L

**Lazy by default**
不变量之一。derivation / view / metadata / agent / scripture / gossip 默认按需触发；eager 必须显式。

**LazyRef / Stub**
IR 节点里的延迟引用原语，避免一次性把整棵远端目录 / 整个 query 结果加载进来。带 `fetch_on` 字段。

## M

**Materialize**
view 从 stub 状态变成可读字节流的过程。默认按 placeholder 触发；`kuli view materialize` 主动批量化。

**Matcher**
调度器用来从 CapabilityBid 候选里选 peer 的函数。默认 Luau impl；可被覆盖。

**Mesh / Mesh DSL / Meshell**
对等 peer 网络 / 用来在 mesh 上写"跨机 bash"的 DSL / kuli 自带的内置实现。

**Micro-core**
不变量之一。C++ 只做计算密集 + 平台/库抽象 + IR runtime + transport + 极简内置 executor，所有业务 / scripture / backend 协议走 Luau。

## P

**Peer**
mesh 中的一个 kuli 实例。每个 peer 都有 Ed25519 keypair + peer_id。

**Peer-equal**
不变量之一。代码上无 client/server 概念；差异来自 policy / UI 层。

**Placeholder**
ProjFS 五状态之一：metadata 已缓存，内容未拉。

**Plumtree**
EBT (Epidemic Broadcast Tree) 算法；kuli capability gossip 的参考。

**Profile**
一组 store path 在 PATH 上的具体编排。Nix-flavored。

**ProjFS**
Windows Projected File System；kuli 用它把 view 物化成"占位符 + 按需 hydrate"。POSIX 对应 FUSE。

**Pure DSL**
不变量之一。蓝图 Luau 函数给同 ctx 必产 hash 相同的 derivation。

## R

**Realize**
执行一个 derivation 的 builder，产出 store path 的过程。一次成功后短路。

**RouteJob**
一类 coordinator IR：task → capable peer 路由 + fallback。

**RPC framing**
length-prefixed frame，多 channel；可插拔 transport 之上的统一协议。

## S

**Sandbox (Luau)**
nil 掉 `io` / `os` / `require` / `package` / `load*` / `debug`，只留 `base` / `math` / `string` / `table` / `utf8`。

**Scripture**
用户扩展包；含一组 basename → adapter 映射 + resources。本身是个 derivation。

**Session (peer)**
两个 peer 之间的持久 transport channel + 远端 kuli daemon 进程。

**Shim**
`~/.local/bin/<basename>` 下的小可执行；把控制权转给 `kuli --basename <basename>`。

**Slime mold mesh**
"黏菌网络"——kuli peer 网络的别称，强调每节点对等、task 在网中传播。

**Store**
内容寻址的本地存储根目录（`~/.local/share/kuli/store/`）。每条 derivation 一个子目录，名 `<hash[:16]>-<name>`。

**Stub**
见 LazyRef。

**Super-peer**
人在前的 host。policy 上比 headless peer 多承担 UI / 调度发起，但代码对称。

## V

**Vault**
view 物化文件夹内部的 `.kuli/` 子目录的别名——含 manifest / index / content cache。**只在设计规格与代码出现**，不进用户面。

**View**
用户面唯一一等概念：query + 物化位置（可选 publish policy）。

## W

**WASM (v1+)**
Luau 之外的二级扩展运行时，与 Luau 并存。v0.x 不实现。
