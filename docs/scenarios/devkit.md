# 场景一：复刻 devkit 开发环境（主业 2 首个落地）

> 本文是 kuli **第一个落地场景**的规格。它把 `D:\devkit`（一套命令式 `.cmd` 的便携 Windows
> 开发环境安装器）逐项映射到 kuli 的 **blueprint / store / profile / scripture / env** 机制上，
> 给出 kuli-bps 蓝图分解、超出现有前作的设计决断、以及引擎从 `ref/luban` 的吸收来源。
>
> 这件事就是 `requirements.md` §3.2 **主业 2 —— 开发环境复现**。devkit 是它的命令式原型，
> `ref/luban` + `ref/luban-bps` 是同目标的声明式 C++ 前作（已发布 v1.1）。kuli 的目标不是"再写一遍
> devkit"，而是用 kuli 的一等机制**声明式、可复现、可回滚**地达到 devkit 的同一终态。
>
> 概念词汇见 [`../concepts.md`](../concepts.md) / [`../glossary.md`](../glossary.md)；机制细节见
> [`../design.md`](../design.md)；CLI 见 [`../cli.md`](../cli.md)。

---

## 1. 目标终态（从 devkit 反推）

一行 `iwr … | iex` 装完 kuli，再 `kuli bp apply <name>`，即得到与 devkit 等价的环境，且
**可逆、零 UAC、HKCU-only、绿色便携、不装系统**。下表是 devkit 的 `00`–`05` 脚本 + `.devkit-manifest`
的全部产出，也就是本场景的验收清单。

### 1.1 工具（15 个 + 字体）

| 工具 | devkit 来源 | shim / 上 PATH 方式 | 备注 |
|---|---|---|---|
| wezterm | nightly **固定 URL**（非 release） | **智能 shim**：裸→GUI，带子命令→CLI | 捆绑 Maple Mono 字体 |
| clink | `chrisant996/clink` `.zip`（排除 setup） | shim `clink_x64.exe` → `clink` | cmd 行编辑 / Lua 钩子 |
| starship | `starship/starship` msvc.zip | shim | 提示符 |
| fzf | `junegunn/fzf` windows_amd64.zip | shim | + clink-fzf `fzf.lua`（raw URL） |
| zoxide | `ajeetdsouza/zoxide` msvc.zip | shim | + clink-zoxide `zoxide.lua`（raw URL） |
| fd | `sharkdp/fd` msvc.zip | shim | |
| ripgrep | `BurntSushi/ripgrep` msvc.zip | shim `rg.exe` → `rg` | |
| neovim | `neovim/neovim` nvim-win64.zip | shim | |
| git (MinGit) | `git-for-windows/git` `-64-bit.zip`（排除 busybox） | **真实目录上 PATH**（`opt\git\cmd`），**不 shim** | GCM / `git <子命令>` 需真 exe |
| git-lfs | `git-lfs/git-lfs` windows-amd64 | **真 exe 拷贝**进 `.local\bin` | git 不能调 `.cmd` 子命令 |
| git-credential-manager | `git-ecosystem/…` gcm-win-x64（排除 symbols） | shim + `gcm configure` | framework-dependent，自写绝对路径 helper |
| cmake | `Kitware/CMake` windows-x86_64.zip | shim `cmake`/`ctest`/`cpack` | |
| ninja | `ninja-build/ninja` ninja-win.zip | shim | |
| yazi | `sxyazi/yazi` msvc.zip | shim `yazi`/`ya` + `y.cmd` wrapper | TUI 文件管理器 |
| fastfetch | `fastfetch-cli/fastfetch` windows-amd64.zip | shim | |
| uv | `astral-sh/uv` msvc.zip | shim `uv`/`uvx` | Python 包管理 |
| rclone | `rclone/rclone` windows-amd64.zip | shim（解到版本子目录，递归找 exe） | |
| Maple Mono NF CN | `subframe7536/maple-font` `MapleMono-NF-CN.zip` | **不装系统**，捆进 wezterm `font_dirs` | wezterm 只在启动时扫字体目录 |

**vcpkg 故意不打包**（沿用 devkit 的明确立场）：由各 C/C++ 项目在首次 `cmake` 配置时自举，
共享二进制缓存 `%LOCALAPPDATA%\vcpkg\archives`。本场景文档附 devkit README 的 `vcpkg-bootstrap.cmake` 模板。

### 1.2 环境变量（HKCU，幂等：已存在则保留）

`HOME`（默认 `%USERPROFILE%`）、`XDG_CONFIG_HOME`（`~/.config`）、`XDG_DATA_HOME`（`~/.local/share`）、
`XDG_STATE_HOME`（`~/.local/state`）、`XDG_CACHE_HOME`（`~/.cache`）、`YAZI_CONFIG_HOME`（`$XDG_CONFIG_HOME/yazi`）；
PATH 追加 `~/.local/bin` 与 `~/.local/opt/git/cmd`（去重，已在则跳过）。

### 1.3 配置（devkit 用 junction —— 配置住仓库、链接到位、改一处处处生效）

| 目标位置 | 链接到 | 备注 |
|---|---|---|
| `$XDG_CONFIG_HOME/{nvim,wezterm,yazi,fastfetch,starship}` | `config\*` | 遵循 XDG 的工具 |
| `%LOCALAPPDATA%\clink` | `config\clink` | clink 默认 profile |
| `%APPDATA%\rclone` | `config\rclone` | `rclone.conf` 含密钥，目录持久但不随仓库分发 |
| `%APPDATA%\mpv.net` | `config\mpv.net` | 运行时拉 `thumbfast.lua` + uosc（只取 `scripts/`+`fonts/`） |
| `%APPDATA%\Everything` | `config\everything` | 先把既存 `Everything.ini` 迁进仓库再链接 |

外加运行时从 raw URL 拉取的：`clink-fzf/fzf.lua`、`clink-zoxide/zoxide.lua`、`thumbfast.lua`、uosc。
动既存真实文件夹前必须询问（Backup / Skip / Overwrite），绝不静默删除。

### 1.4 接线（04-wire）

- clink 设置：`fzf.default_bindings true`、`fzf.exe_location`、`autosuggest.enable true` +
  `strategy "match_prev_cmd history completion"`、`history.shared true`、`history.max_lines 10000`、
  `clink.autoupdate off`；clink 运行态（历史/日志/生成设置）落 `$XDG_STATE_HOME/clink`（`CLINK_PROFILE`），
  不污染 `config\clink`。
- MSVC 环境缓存：`vswhere` 定位 VS → 跑 `vcvars64.bat` → 把 `BASE_PATH/VSDEV_PATH/INCLUDE/LIB/LIBPATH`
  缓存进 `config\clink\vsenv.generated.env`，由 `vs-inject.lua` 注入每个 clink 会话。无 VS 则空操作。
- `git-lfs install` + `git-credential-manager configure`。

### 1.5 集成（05-context-menu，HKCU）

"在此打开 WezTerm"右键菜单（`HKCU\…\Directory[\Background]\shell\WezTermHere`，命令
`wezterm-gui.exe start --cwd "%V"`）+ 开始菜单 `WezTerm.lnk` 快捷方式（指向 `wezterm-gui.exe`，
windows 子系统，无多余控制台窗口）。

### 1.6 横切不变量（与 kuli R-NF-01..08 对齐）

幂等（重跑报 already OK）、可逆（manifest + teardown）、零 UAC / HKCU only、不污染系统 PATH /
Program Files、不装系统字体、绿色便携。

---

## 2. devkit → kuli 机制映射

> 左列是 devkit 的命令式机制，右列是 kuli 的一等机制（词汇取自 `design.md` / `cli.md`）。

| devkit 机制 | kuli 机制 |
|---|---|
| `lib\gh-latest.cmd` + `gh-pick.js` 解析 GitHub latest release 资产 | **bp source resolver**（`github:` scheme，匹配 host triplet 资产） |
| `lib\fetch-one.cmd` 下载 + `tar` 解压到 `.local\opt\<tool>` | derivation **`fetch` builder** → 校验 sha256 → realize 进**内容寻址 store** `~/.local/share/kuli/store/<hash>-<name>/` |
| `lib\make-shim.cmd` 写 `.local\bin\<tool>.cmd`（带 `:: devkit-managed` 标记 + manifest 记录） | **scripture / shim 机制**：shim 写 `~/.local/bin/<basename>`，登记进 profile generation |
| `01-setup-env.cmd` 设 HKCU 变量 + PATH | **`EnvSet` IR**（HKCU on Win，§design 4.4）+ profile 的 PATH 编排 |
| `03-link-configs.cmd` junction 配置目录 | **配置部署**：`lib.withFiles` / config renderer + **junction-deploy 模式**（见 §4.1） |
| `04-wire.cmd` clink 设置 + MSVC 缓存 | derivation 的 **`post_install` step** + 生成态资源部署（vsenv.env + 注入 lua） |
| `05-context-menu.cmd` reg + `.lnk` | **win-registry** 原语 + 快捷方式生成（post_install） |
| `.devkit-manifest` + `teardown.cmd` | **profile / generation** + `kuli generation rollback`（每次 apply 是一代，append-only，可回滚） |
| 手动按序跑 `00`–`05` | `install.ps1`（一行装）+ `kuli bp apply <name>` 一条龙 |
| `choice` 逐工具确认 / dry-run 预览 | apply 前打印 **trust summary** + `--dry-run`（走完 plan 不写盘，见 cli.md §6） |

整条链落进 kuli 的 IR：一份 Luau 蓝图被 adapter 解析为 **`ApplyDerivation` IR**（含 `EnvSet` / `FileOp`
子节点），engine plan → execute → 写 evidence session（`.kuli/sessions/<id>/`，可 `kuli resume`）。

---

## 3. kuli-bps 蓝图分解

照 `ref/luban-bps` 的多图纸风格（每个 `.lua` 返回一份蓝图，见 `ref/luban-bps/blueprints/*.lua`），
但把栈换成 devkit 的栈。三份蓝图，按依赖顺序：

### 3.1 `bootstrap` —— 编译 / 构建基座

git（MinGit，`bin = "cmd/git.exe"`，真实目录上 PATH）+ git-lfs（真 exe）+ git-credential-manager
+ cmake（`shim_dir = "bin"` 自动 shim cmake/ctest/cpack）+ ninja + **MSVC 环境缓存**。
**遵循 devkit：不打包 vcpkg**（cmake 自举，附 `vcpkg-bootstrap.cmake` 模板）。
`configs.git` 走 config renderer 写 LFS filter + `credential.helper = manager`（对标 luban-bps bootstrap）。

### 3.2 `terminal` —— 终端体验（`requires = { bootstrap }`）

wezterm（**显式 URL source**，nightly）+ clink + starship + fzf + zoxide + fastfetch
+ Maple Mono NF CN 字体（`no_shim = true`，post_install 注册到 `HKCU\…\Fonts` 并 `AddFontResourceEx` 广播）
+ clink lua 配置（fzf.lua / zoxide.lua / starship.lua / vs-inject.lua / clink_start.cmd）
+ clink 设置（post_install 跑 `clink set …`）+ MSVC 注入资源
+ 右键菜单 / 开始菜单（win-registry post_install）+ wezterm/clink/starship/fastfetch 配置部署。

### 3.3 `tools` —— 趁手 CLI（`requires = { bootstrap }`）

fd + ripgrep + neovim + yazi（`shims = { yazi, ya }` + `y` wrapper）+ uv（`shims = { uv, uvx }`）+ rclone，
及各自配置部署（nvim init.lua、yazi、ripgreprc、fd ignore 等，参考 luban-bps onboarding 的 `files` 块）。

> 每个工具 / 配置 / env / shim 的 IR 形态用 cli.md 现有的 `bp` / `scripture` / `view` 词汇描述。
> 蓝图本身遵守 R-NF-03/04：Luau 沙盒、纯函数化（同 `ctx` 必产相同 hash 派生）。

---

## 4. 照搬 devkit 暴露的、超出 luban 现有能力的设计点（须明确决断）

这是本规格的真正价值——devkit 比 luban-bps 的 onboarding 多出的硬骨头。每条给出**首版决断**。

1. **junction 配置模型。** devkit 让配置"住仓库、链接到位、改一处处处生效"；luban 是"bp 拥有并写入文件
   内容"。kuli 本就有 view + ProjFS，junction 是天然能力。**决断**：为配置部署引入 `mode = "junction"`，
   与现有 `mode = "replace"/"patch"` 并列——**可编辑配置**（wezterm.lua / nvim / clink lua）走 junction-deploy
   指向 store 或一个 kuli 管理的可写配置根；**只读 / 渲染生成**配置（ripgreprc / starship.toml）走 content-deploy。
   junction 关系登记进 generation，回滚时 `rmdir` 链接、还原备份（对标 devkit `.devkit-bak`）。

2. **WezTerm 智能 shim。** 裸 `wezterm` → `start "" wezterm-gui.exe`（windows 子系统，无控制台窗口）；
   `wezterm <子命令>` → `wezterm.exe`（控制台 CLI）。**决断**：scripture shim 支持 `shim_kind = "smart"`，
   生成的 shim 按 argv 分流（不是纯转发）。

3. **git-lfs 必须是真 `.exe`。** git 不能把 `.cmd`/`.sh` shim 当子命令调。**决断**：shim 机制增加
   `deploy = "real_exe"` 模式（把真 exe 拷进 `~/.local/bin/`），直接吸收 luban 的 `luban-shim.exe`
   真 exe twin 思路。

4. **git 以真实目录上 PATH（不 shim）。** GCM 与 `git <子命令>` 需要 git.exe 与同级目录在一起。
   **决断**：profile 支持"真实 bin 目录入 PATH"条目（`~/.local/opt/git/cmd`），与 shim 目录并列——luban 已有此能力。

5. **非 release 来源。** wezterm-nightly 固定 URL、clink-fzf/zoxide/thumbfast 的 raw URL。
   **决断**：source resolver 增加 `url:`（显式 artifact + sha256/无 sha256 TOFU）与 `raw:`（单文件抓到资源目录）两个 scheme，与 `github:` 并列。

6. **选择性解压。** uosc 只取压缩包里的 `scripts/` + `fonts/`。**决断**：`fetch` builder 支持
   `extract = { paths = {...} }` 部分解压（扩展 luban `archive.cpp`）。

7. **MSVC 环境缓存 + clink 注入。** `vsenv-gen` 探测 VS、缓存环境、由 lua 注入。**决断**：kuli 平台层提供
   msvc-env 探测原语（吸收 luban `msvc_env.cpp`），post_install 生成 `vsenv.generated.env` 并部署注入 lua；
   无 VS 时静默空操作。

---

## 5. 引擎来源：吸收 / 移植 `ref/luban`（本场景实现期，非本次）

luban 已把本场景 ~90% 的机制做成成熟 C++（v1.1）。下表指明每个 kuli lib 从哪个 luban TU 移植
（路径以 `ref/luban/src/` 为准）。

| kuli lib | 吸收自 luban TU |
|---|---|
| kuli-bp source resolver | `source_resolver.cpp` / `source_resolver_github.cpp`（+ 新增 `url:` / `raw:` scheme，见 §4.5） |
| kuli-store | `store.cpp` / `store_fetch.cpp` / `archive.cpp`（+ 部分解压，§4.6）/ `hash.cpp` |
| kuli-bp 蓝图运行时 | `blueprint.hpp` / `blueprint_lua.cpp` / `blueprint_apply.cpp` / `blueprint_lock.cpp` |
| kuli 文件 / 配置部署 | `file_deploy.cpp` / `config_renderer.cpp` / `renderer_registry.cpp`（+ junction 模式，§4.1） |
| kuli win 平台 | `win_registry.cpp`（env / 字体 / 右键）/ `msvc_env.cpp` / `win_path.cpp` |
| kuli scripture / shim | `shim.cpp` + `luban-shim.exe` twin（+ 智能 shim §4.2 / 真 exe §4.3） |
| kuli-luau 沙盒 | `lua_engine.cpp` / `lua_frontend.cpp`（lua C API 收敛到单 TU，正合 R-NF-03）/ `lua_json.cpp` / `lua_pattern.cpp` |
| kuli-transport（http 原语） | `libcurl_backend.cpp` / `download.cpp` |
| kuli paths / 可逆 | `paths.cpp` / `xdg_shim.cpp` / `applied_db.cpp`（→ 收敛进 kuli 的 profile/generation 模型） |
| kuli util / diag | `progress.cpp` / `log.cpp` / `proc.cpp` |

**关键调和点。** luban 不是 IR 架构，kuli「IR 为骨」。吸收方式 = luban 的模块成为 kuli
`ApplyDerivation` / `EnvSet` / `FileOp` IR 背后的 **executor primitive**；由 Luau 蓝图产出 IR、engine 派发。
这样既满足"完全吸收 luban"，又不破坏 R-NF-03（C++ core 不引 `lua.h`，binding 收敛 `kuli_luau` 单 TU）与
R-NF-08（micro-core，业务走 Luau 叠层）。luban 已自带的不变量（XDG-first / 零 UAC / HKCU-only /
静态链接 / core 不引 lua.h）与 kuli R-NF-01..08 同源，迁移阻力小。

---

## 6. 接收标准（对标 devkit §13）

干净 Win 11 上：

1. 一行 `iwr … | iex` 装完 kuli 自身（< 1 分钟）。
2. `kuli bp apply terminal`（自动先 apply 依赖 `bootstrap`）+ `kuli bp apply tools` 后：
   §1.1 的 15 个工具全部 `where`/`Get-Command` 命中并可运行；git-lfs 以 `git lfs version` 验证（真 exe 生效）；
   `cmake`/`ninja`/MSVC 环境可编译一个 `find_package(fmt)` 的 vcpkg manifest 项目。
3. §1.3 配置就位（wezterm 加载 Maple Mono 字体、starship 提示符出现、clink 注入 fzf/zoxide 绑定）。
4. §1.5 右键菜单"在此打开 WezTerm"与开始菜单快捷方式出现且能开 GUI（无多余控制台窗口）。
5. `kuli bp apply <name> --dry-run` 走完 plan 且不写任何盘 / HKCU。
6. `kuli generation rollback` 干净回滚：shim / junction / env / PATH / 字体注册项被精确移除，
   还原备份，`~/.local/bin` 既存的他方文件（如 `claude.exe`）与用户数据原样保留（对标 devkit teardown）。
7. 全程零 UAC、HKCU only；`dumpbin /DEPENDENTS kuli.exe` 拒系统运行时之外的动态依赖（R-NF-02）。
8. Luau 沙盒负向测试：蓝图里调 `io.open` / `os.execute` / `require` 必须求值失败（R-NF-03）。

> 以上每条进 kuli 的 conformance 测试集（roadmap M1 完成判据）。
