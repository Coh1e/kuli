# kuli CLI 谓语名词设计

> kuli 的 CLI 表面 = **resource-style 子命令**（git 风：`kuli <noun> <verb>`）+ **basename sugar**（`find` / `ndisk` / `kuli-bp` 等直接走 scripture）+ **@peer prefix**（`kuli @prod ps` exec at peer）+ **meshell 一行式**（`kuli '@a:... | @b:...'`）。
>
> 设计原则：
> 1. **每个 noun 一个一致动词集**（ls / show / create / rm / publish 等）；不为某 noun 发明独有谓语
> 2. **@peer 是 sugar，不是新语法层**：底层都是 `Exec` / `RemoteSession` IR
> 3. **basename sugar 由 scripture 决定**，不在 kuli 主表里硬编码
> 4. **全局 flag 处处可用**（`--dry-run` / `--json` / `--refresh`）
> 5. **dangerous 操作要显式确认**（删 view、rollback、scripture uninstall），可用 `--yes` 跳

## 1. 顶层入口

```
kuli <subcommand> [args...]      # 标准子命令
kuli <basename> [args...]        # 等价 kuli --basename <basename> -- args...
kuli @<target> <cmd>             # 等价 kuli exec --at @<target> -- cmd
kuli '<meshell pipeline>'        # 一行 meshell
kuli -c '<meshell>'              # 同上显式
```

basename sugar 触发条件：argv[1] 不是已知子命令、不以 `@` / `-` / `'` 开头、且 scripture-registry 里有此 basename。

## 2. 名词 × 动词矩阵

✅ MVP，🌱 v0.2+，🌿 v1+

### 2.1 view

| 动词 | 干什么 | MVP |
|---|---|---|
| `view create <name> --query <q> --at <uri>` | 新建 view | ✅ |
| `view ls` | 列所有 view | ✅ |
| `view show <name>` | 显示 view 定义 + 状态 | ✅ |
| `view rm <name>` | 删 view（连同 vault；需 `--yes`） | ✅ |
| `view materialize <name> [path-glob]` | 主动 hydrate | ✅ |
| `view unmaterialize <name> [path-glob]` | 把 hydrated 退回 placeholder | ✅ |
| `view sync <name> [--push\|--pull\|--both]` | 与 backends 同步 | ✅ |
| `view diff <name>` | 与 backends 差异预览 | ✅ |
| `view publish <name> --policy <p>` | 标 public/mutual-trust/shared | ✅ |
| `view unpublish <name>` | 撤回 publish | ✅ |
| `view share <name> --to <peer-id>` | shared policy 下加白名单 | 🌱 |
| `view mv <old> <new>` | 改名 | ✅ |

### 2.2 bp（blueprint）

| 动词 | 干什么 | MVP |
|---|---|---|
| `bp apply <name>` | realize + 切 profile | ✅ |
| `bp apply <name> --dry-run` | 预览，不写盘 | ✅ |
| `bp explain <name>` | 展开为人类可读派生树 | ✅ |
| `bp doctor` | 检查 source / lock / 已装派生健康 | ✅ |
| `bp src add <url\|path> [--name <n>]` | 注册 bp source | ✅ |
| `bp src rm <name>` | 撤销注册 | ✅ |
| `bp src update <name>` | 拉最新 | ✅ |
| `bp src ls` | 列所有 source + 信任态 | ✅ |
| `bp describe <name>` | 跟 luban `describe tool:` 同型 | ✅ |

### 2.3 scripture

| 动词 | 干什么 | MVP |
|---|---|---|
| `scripture install <pkg>` | realize derivation + 写 shim | ✅ |
| `scripture uninstall <name>` | 删 shim + GC 候选 | ✅ |
| `scripture ls` | 列已装 | ✅ |
| `scripture show <name>` | 显示 manifest + basename 列表 | ✅ |
| `scripture search <q>` | 在已配 bp source 里搜 | ✅ |
| `scripture publish <path>` | 打包 + push 到自己拥有的 source | 🌿 |

### 2.4 peer

| 动词 | 干什么 | MVP |
|---|---|---|
| `peer add <addr> [--name <n>]` | 添加 + 交换公钥 (handshake) | ✅ |
| `peer rm <id\|name>` | 撤销信任 | ✅ |
| `peer ls` | 列已知 peer | ✅ |
| `peer show <id\|name>` | 详细：endpoint、capability、信任态 | ✅ |
| `peer trust <id> --policy <...>` | 调整信任策略 | ✅ |
| `peer ping <id>` | RTT + capability hit | ✅ |
| `peer group <name> [--add <ids>]\|[--rm <ids>]` | 命名组（meshell `@group:` 用） | ✅ |

### 2.5 capability

| 动词 | 干什么 | MVP |
|---|---|---|
| `capability ls [--peer <id>]` | 列本机 / 某 peer 的 capability | ✅ |
| `capability query <constraints>` | mesh 中查谁满足 | ✅ |
| `capability refresh [--peer <id>]` | 主动重拉 | ✅ |
| `capability gossip` | 触发一轮 gossip（v0.2+） | 🌱 |

### 2.6 session（peer transport）

| 动词 | 干什么 | MVP |
|---|---|---|
| `session ls` | 当前活跃 peer session | ✅ |
| `session show <id>` | endpoint / channels / 流量 | ✅ |
| `session kill <id>` | 立即关闭 | ✅ |

### 2.7 backend

| 动词 | 干什么 | MVP |
|---|---|---|
| `backend ls` | 列已装 backend driver | ✅ |
| `backend show <name>` | 协议 / scripture / 配置示例 | ✅ |
| `backend configure <name>` | 交互式建配置（如百度 BDUSS） | ✅ |
| `backend rm <name>` | 卸载（其实卸 scripture，sugar 而已） | ✅ |

### 2.8 store / derivation / generation / profile

| 动词 | 干什么 | MVP |
|---|---|---|
| `store ls` | 列所有 store path | ✅ |
| `store describe <hash\|name>` | 哪个 derivation、谁引用 | ✅ |
| `store gc` | 清未被任何 profile generation 引用的 | ✅ |
| `derivation ls` | 列所有 derivation | ✅ |
| `derivation realize <hash>` | 强制重 realize | ✅ |
| `generation ls [--profile <p>]` | 列当前 profile 的 generation | ✅ |
| `generation diff <a> <b>` | 两代差异 | ✅ |
| `generation rollback [--steps N]` | 回滚（默认 1） | ✅ |
| `generation switch <id>` | 切到指定 generation | ✅ |
| `profile ls` | 列所有 profile | ✅ |
| `profile current` | 当前激活 profile | ✅ |
| `profile create <name>` | 新建 | 🌱 |
| `profile switch <name>` | 切到指定 profile | 🌱 |

### 2.9 env（环境变量）

| 动词 | 干什么 | MVP |
|---|---|---|
| `env get <key>` | 读 | ✅ |
| `env set <key>=<value> [--at <peer>]` | 写（HKCU on Win / shell-rc on POSIX） | ✅ |
| `env unset <key>` | 删 | ✅ |
| `env ls [--at <peer>]` | 列 | ✅ |

### 2.10 顶层动词（无 noun）

| 动词 | 干什么 | MVP |
|---|---|---|
| `doctor` | 全面健康检查（PATH / shim / store / peer / 信任 / 跨平台） | ✅ |
| `version` | 版本号 + 构建信息 | ✅ |
| `resume <session-id>` | 接续 evidence session | ✅ |
| `exec --at <target> -- <cmd>` | 在 target 跑 cmd（`kuli @target cmd` sugar 此条） | ✅ |
| `ls <at-uri>` | sugar：等价 `FileQuery` at uri | ✅ |
| `daemon [--listen <addr>]` | 主动 listen（headless peer 启动方式） | ✅ |
| `init` | 在 cwd 建 `.kuli/`，把当前目录视作 kuli workspace | ✅ |

## 3. Basename sugar

由 scripture 决定。**kuli 主表不硬编码任何 basename**。MVP 时 kuli 安装包内置一套 scripture（`share/kuli/scriptures/`），它们装上后自然有以下 basename 可用：

| 来源 scripture | basename |
|---|---|
| `cloud-storage-mgr` | `ndisk` / `nput` / `nget` / `nls` / `nrm` / `nmv` |
| `bp` | `kuli-bp` |
| `ssh-devops-helper` | `nssh` / `nupload` / `ndownload`（建议名） |
| `find` | `find`（dangerous-off default） |
| `grep` | `grep` |

用户可以选择**不装**内置 scripture，则上述 basename 不可用。也可装第三方 scripture 引入新 basename。

## 4. @target 语法

`@target` 出现在两个位置：
1. argv 第一位（顶层）：`kuli @<target> <cmd>` → sugar for `kuli exec --at @<target> -- <cmd>`
2. meshell 内（一行式或 `-c`）：每段 `@<target>: <cmd>` 描述 pipe 的一节

target 形式：

| 形式 | 含义 |
|---|---|
| `@prod` | peer 名 `prod` |
| `@<peer-id-hex>` | 直接 peer id |
| `@prod[1-10]` | fan-out 到 prod1..prod10 |
| `@prod[a,b,c]` | 显式列举 |
| `@group:builders` | 命名组（用 `peer group` 建） |
| `@'tools.cmake>=3.20'` | capability 内联约束，让 scheduler 选 peer |
| `@route(target=linux-aarch64)` | 路由委派，scheduler 显式决定 |
| `@local` | 本机（peer-equal 视角下也是个 peer） |

## 5. at-URI（IR 层 `at:` 字段）

CLI 上偶尔直接出现（如 `kuli view create --at <uri>`、`kuli ls <uri>`）。

| URI | 含义 |
|---|---|
| `local:<path>` | 本机文件系统 |
| `view:<name>[/sub]` | 本机一个 view 的内部路径 |
| `view@<peer-id>:<name>[/sub]` | 远端 peer 的 public view |
| `baidu:<path>` | 百度网盘路径 |
| `sftp://<user>@<host>:<port>/<path>` | SFTP |
| `webdav://<host>/<path>` | WebDAV |
| `ftp://<user>@<host>/<path>` | FTP |
| `@<peer>` | 远端 peer（exec / capability 语境） |
| `@<peer>:<path>` | 远端 peer 的 local path（exec / 文件语境复合） |

## 6. 全局 flag

| flag | 适用 | 行为 |
|---|---|---|
| `--dry-run` | 所有 mutation | 走完 plan 不执行；输出预览 |
| `--json` | 所有 | 结构化输出（机器读） |
| `--explain` | 所有 | 显示 IR + descent_trace |
| `--refresh` | 含 metadata 读 | 绕过 cache 重拉 |
| `--no-color` | 所有 | 关 ANSI |
| `--log-level <off\|err\|warn\|info\|debug\|trace>` | 所有 | 默认 info |
| `--session <id>` | mutation | 把记录写进已有 session（resume 配套） |
| `--yes` | dangerous | 跳过确认 |
| `--at <uri>` | 一些子命令 | 显式 at（覆盖 cwd 默认） |
| `--basename <name>` | 顶层 | 强制走 scripture basename routing |
| `--profile <name>` | bp / generation / profile | 操作指定 profile |
| `--peer <id>` | capability / env / 部分查询 | 操作指定 peer |
| `--prefetch` | 含 LazyRef 读 | 提前 hydrate 候选 stub |

## 7. 退出码

| 码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 一般失败（解析、参数、业务） |
| 2 | IR 校验失败 |
| 3 | 沙盒违规（Luau 越界）|
| 4 | 远端 peer 错误 / 不可达 |
| 5 | scheduler 找不到候选 peer（且 fallback=fail） |
| 6 | 用户取消（dangerous 操作） |
| 7 | session 中断（可 resume） |
| 70+ | 内部 bug（应 report） |

## 8. 帮助系统

- `kuli help` / `kuli --help` —— 顶层 noun 列表 + 全局 flag
- `kuli <noun> help` / `kuli <noun> --help` —— 该 noun 的所有 verb
- `kuli <noun> <verb> --help` —— 该 verb 的具体参数
- `kuli help concepts` —— 跳到 `docs/concepts.md` 的关键摘要
- `kuli help cli` —— 本文件浓缩版

## 9. 输出风格

- 默认人类可读，带 ANSI 颜色（除非 stdout 非 tty 或 `--no-color`）
- `--json` 严格 schema 输出（同 `kuli/ir/1.0` 的 RawResult 形态）
- 诊断走 stderr，rustc 风格：`error[E0042]: <msg>` + 多行 span + `help:` hint
- 不写无意义"start..."、"done."；只写发生了什么 + 接下来该怎么做

## 10. 反例（不要做）

- **不要发明 noun 独有动词**：`view materialize` ✅，`view hydrate` ❌（语义重复）
- **不要 nest 三层**：`kuli bp src` 已经两层，到 `bp src cache prune` 就该改成 `kuli cache prune --bp-src`
- **不要默认 dangerous**：删 / rollback / unpublish 不带 `--yes` 必须交互确认
- **不要把 `--peer` 跟 `@<peer>` 混用**：`@target` 表"在这里执行"；`--peer` 表"对它操作"——比如 `kuli capability ls --peer prod` 读 prod 的 capability，跟 exec 无关
- **不要 silent 修改用户文件**：写 `~/.bashrc` / WT settings 必须 `post_install` 显式 + 可关
- **不要把 vault 暴露到 CLI**：用户面只有 view；vault 是内部词
