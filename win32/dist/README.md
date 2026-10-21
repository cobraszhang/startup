# 开机自动加载应用启动器（boot-time application launcher）

按配置文件中的 **依赖关系（deps）** 决定应用的启动顺序：先做拓扑排序，再按序启动。
服务进程常驻运行，并对登录后的用户进程提供 **控制接口（IPC）**：启动/中止/重启/状态
（见“进程模型与控制接口”）。提供两个版本，共享同一套纯 **C89** 核心代码：

| 版本 | 入口 | 开机自启方式 |
|---|---|---|
| Win32 | `win32/win32_service.c`（服务/CLI 入口）+ `win32/win32_gui.c`（无参数时的配置/状态窗口）+ 共享 `src/cli.c` | **Windows 系统服务**（`SERVICE_AUTO_START`，由 SCM 开机拉起；supervisor 常驻） |
| POSIX | `posix/posix_main.c`（开机/supervisor/CLI 入口）+ 共享 `src/cli.c` | systemd unit（`serve` 常驻 supervisor）/ SysV / rc.local；一次性开机模式仍保留 |

## 目录结构

```
README.md .gitignore .gitattributes      根目录：说明与 git 文件
src/                仅放跨平台共享源文件（纯 C89）
  cfg.[ch]            配置解析 + 共享模型编辑/保存（add/rm/rename/set/deps、cfg_save）
  topo.[ch]           Kahn 拓扑排序 + 环检测 / 未知依赖检测
  log.[ch]            带时间戳的日志（可重定向到 GUI）
  cli.[ch]            共享命令行管理接口（Win32 与 POSIX 同一个 cli_main）
  version.h           发布版本号单一来源（当前 0.1.1）
  ipc.[ch]            服务进程↔用户进程控制协议（共享语法与客户端调用）

win32/              Win32 平台源文件与工程文件
  win32_service.c     服务/CLI 入口（SCM 注册、CreateProcess 启动、supervisor 常驻）
  ipc_win32.c         控制接口命名管道传输（服务侧 + CLI 客户端）
  win32_gui.c         结构化 GUI 配置/状态窗口（原生 API，GBK 源码）
  win32_app.h         服务与 GUI 共用的内部接口
  build_win32.cmd     MinGW-w64 与 VC6 双编译器构建脚本（debug/release 四份）
  appboot.rc          Windows 文件版本资源（VERSIONINFO，需与 version.h 同步）
  package_win32.cmd   生成发布包到 dist\
  conf\startup.conf   Windows 样例配置（也是开发构建的默认 ../conf 目标）
  test\               Win32 测试配置与 svc_lifecycle.cmd
  build\debug\        调试构建（gcc+cl，含 .pdb 调试信息，不入库）
  build\release\      发布构建（gcc+cl，package_win32.cmd 取这里，不入库）
  dist\bin\           发布执行文件（入库）

posix/              *nix 平台源文件与工程文件
  posix_main.c        开机/supervisor/CLI 入口（fork + /bin/sh -c，可 -d 后台化）
  ipc_posix.c         控制接口 AF_UNIX 套接字传输（含 FD_CLOEXEC，防 fd 泄入子应用）
  gui_gtk.c           GTK+3 图形配置窗口（有桌面会话时无参数自动打开）
  posix_app.h         入口与 GUI 共用的内部接口
  Makefile            构建（自动探测 pkg-config gtk+-3.0，有则编入 GUI）
  conf\startup.conf   POSIX 样例配置（make install 装到 /usr/local/conf）
  contrib\            systemd unit 与 SysV init 脚本样例
  test\               POSIX 测试配置与 run_posix_tests.sh
  build/debug/        调试构建（-g -O0，不入库）
  build/release/      发布构建（-O2，make install/package 取这里，不入库）
  dist\bin\           发布执行文件（入库）
```

## 版本

当前发布版本：**v0.1.1**（git 标签 `v0.1.1`）。

版本号的单一来源是 `src/version.h`（`APPBOOT_VERSION`），CLI 的 `version` 子命令、
`-V`、usage 头行、两版 GUI 窗口标题和服务日志都从这里取。发版时同步三处：

1. 修改 `src/version.h`；
2. 修改 `win32/appboot.rc` 里的 `FILEVERSION/PRODUCTVERSION/String`（Windows
   文件版本属性，构建时嵌入四个 exe）；
3. `git tag v<版本号>`。

## 发布包布局

可执行文件放在 `dist/bin/`，配置目录即 `dist/conf/`（`bin` 的 `../conf`）：

```
win32\dist\                 posix/dist/
  bin\startup-gcc.exe         bin/startup
  bin\startup-cl.exe          conf/startup.conf
  conf\startup.conf           README.md
  README.md
```

- Windows：`win32\package_win32.cmd`（自动先调用 build_win32.cmd；**只更新 bin 与
  README，`dist\conf\startup.conf` 是现场用户配置，从不覆盖**，仅在缺失时用样例播种）
- POSIX：`cd posix && make package`

部署 = 把 `win32\dist`（或 `posix/dist`）整体拷到目标机器任意目录；服务注册：
`<安装目录>\bin\startup-gcc.exe install`（非管理员会自动弹出 UAC 提权确认）。

## 配置文件位置

默认配置文件目录为 **`../conf`（相对于可执行文件所在目录）**，文件名 `startup.conf`：

| 可执行文件 | 默认读取的配置 |
|---|---|
| `<安装目录>\bin\startup-gcc.exe` | `<安装目录>\conf\startup.conf` |
| `<安装目录>/bin/startup` | `<安装目录>/conf/startup.conf` |
| `/usr/local/bin/startup`（make install） | `/usr/local/conf/startup.conf` |
| 开发构建 `win32\build\{debug,release}\*.exe` | `win32\conf\startup.conf`（`..\..\conf` 回退） |
| 开发构建 `posix/build/{debug,release}/startup` | `posix/conf/startup.conf`（`../../conf` 回退） |

POSIX 版通过 `/proc/self/exe` 解析自身位置（失败时回退 `argv[0]`），与当前工作目录无关。
`-c <文件>` 可显式覆盖配置路径。开发构建位于 `build/debug|release` 两级子目录时，会自动回退查找 exe 目录的 `../../conf/startup.conf`（优先 `../conf`，发布/安装布局不受影响）。

## 配置文件格式

```ini
# 以 # 或 ; 开头的行为注释；[名字] 开始一个应用条目（名字唯一）
[logging]
cmd = C:\tools\logsvc.exe --dir D:\logs   ; 要启动的命令行
wait = 500                                ; 启动后暂停毫秒数（可选）

[database]
cmd = C:\tools\db.exe --data D:\data
deps = logging                            ; 必须先于本应用启动的名字，逗号分隔

[appserver]
cmd = C:\tools\appserver.exe
deps = database, logging
```

语义：

- `deps` 构成有向依赖图，启动顺序 = 拓扑序（Kahn 算法）。同层就绪的应用按配置文件中出现的先后启动（确定性）。
- 依赖了不存在的名字 → 报错退出（不启动任何应用）。
- 存在依赖环 → 报错并列出环上的应用，退出。
- `wait` 是启动某应用后的缓冲时间，用于给依赖它的下一个应用留出初始化时间；Win32 版中该等待可被服务停止请求打断。
- 已启动的应用在停止/卸载服务时**不会被杀掉**（有意为之，应用生命周期归其自身管理）。

## 命令行管理接口（CLI，两版通用）

两个版本共享同一个 `cli_main()`（`src/cli.c`），子命令语义完全一致；
修改类命令读取配置 → 内存模型编辑 → 通过共享的 `cfg_save()` 写回规范格式文件
（GUI 的“保存配置”走同一函数，两边产出逐字节一致）。

```
list                     列出所有应用（cmd/deps/wait）
order                    打印按依赖解析出的启动顺序
check                    仅校验配置（解析 + 拓扑）
run                      校验后按序启动全部应用
add <name> <cmdline>     新增应用（命令行可直接跟在后面，可不加引号）
rm <name>                删除应用（其他应用对它的依赖引用一并清除）
rename <old> <new>       改名（其他应用的依赖引用自动跟随）
set <name> cmd <line>    修改命令行
set <name> wait <ms>     修改启动后等待
dep add <a> <b>          让 a 依赖 b（b 必须已存在）
dep del <a> <b>          删除该依赖
help                     子命令帮助
version                  打印发布版本号（POSIX 还有等价的 -V 选项）
app ...                  控制运行中的应用（IPC，见下节；不改配置文件）
```

> `gui`（仅 POSIX 且构建时带 GTK）：强制打开图形配置窗口，不在共享 `cli.c` 子命令表内。
> `serve`（两版通用，也不是子命令表项）：以前台 supervisor 方式开机启动全部应用并保持
> 运行，对外提供控制接口，见「进程模型与控制接口」。

Windows（`-c` 可出现在任意位置；无参数仍打开 GUI）：

```bat
win32\build\release\startup-gcc.exe list
win32\build\release\startup-gcc.exe order -c win32\test\order.conf
win32\build\release\startup-gcc.exe add myapp cmd.exe /c echo hi ^>^> out.txt -c win32\build\my.conf
win32\build\release\startup-gcc.exe dep add myapp second -c win32\build\my.conf
win32\build\release\startup-gcc.exe run -c win32\build\my.conf
```

POSIX（选项须在子命令**之前**，getopt 标准行为）：

```sh
./build/release/startup list                             # 在 posix/ 目录下
./build/release/startup -c test/order.conf order
./build/release/startup -c /usr/local/conf/startup.conf add myapp "echo hi >> /tmp/x"
./build/release/startup -c /usr/local/conf/startup.conf set myapp wait 200
./build/release/startup -c /usr/local/conf/startup.conf dep add myapp appserver
./build/release/startup -c /usr/local/conf/startup.conf rename appserver app
```

退出码：`0` 成功；`1` 配置/依赖错误；`2` 用法错误；`3` `app` 控制命令连不上服务进程。
`add` 生成的条目立即带合法 `cmd`，因此任何时刻配置文件都可被服务/开机流程读取。

## 进程模型与控制接口（IPC）

项目内的进程分三类：

| 类别 | 角色 | 生命周期 |
|---|---|---|
| **服务进程** | 开机启动的主服务（Win32 服务 `service`，或两版通用的 `serve` 前台 supervisor） | 常驻，持有配置模型与已启动应用的进程句柄/PID |
| **子应用进程** | 服务进程按依赖序启动的用户程序 | 由服务进程跟踪，可被停止/重启 |
| **用户进程** | 登录之后在控制台/窗口里执行的程序（CLI `app` 子命令等） | 一次性：连接服务进程 → 发一条命令 → 打印应答后退出 |

服务进程为用户进程提供对子应用进程的四类接口：**启动（start）、中止（stop）、
重启（restart）、状态（status）**（外加 list/ping/shutdown）。

### 传输与端点

- Win32：命名管道 `\\.\pipe\AppBootLauncher`；
- POSIX：AF_UNIX 流式套接字 `/tmp/bootlauncher.ctl`；
- 覆盖方式：`-s <endpoint>` 选项或环境变量 `BOOTLAUNCHER_CTL`（服务侧与客户侧一致；
  服务进程启动日志会打印它监听的端点）。

### 协议（两平台一致，文本行，一次连接一条命令）

```
请求                          应答（首行）
PING                          OK PONG startup <version>
LIST                          OK configured applications: ...
STATUS  | STATUS <name>       OK status: ...（running / stopped / not-started + pid）
START <name>                  OK started ... / ERR unknown app / already running
STOP <name>                   OK stopped ... / ERR not running
RESTART <name>                  OK restarted ...
QUIT                          OK bye（服务进程随即退出）
```

共享实现在 `src/ipc.[ch]`（语法解析与客户端调用）+ `src/ipc_win32.c` /
`src/ipc_posix.c`（管道 / unix 套接字传输）。

### 用法示例

服务进程启动（任选其一）：

```bat
:: Win32：正式服务（SCM），开机自动
win32\dist\bin\startup-gcc.exe install && win32\dist\bin\startup-gcc.exe start
:: Win32：前台 supervisor（调试/桌面）
win32\build\release\startup-gcc.exe serve -c win32\test\ctl.conf
```

```sh
# POSIX：systemd unit（contrib/startup.service，Type=simple + serve）
systemctl start startup.service
# 或 SysV（contrib/S99startup：-d serve 后台化），或直接前台
./build/release/startup -c test/ctl.conf serve
```

用户进程控制（两平台语法相同；`-s`/`BOOTLAUNCHER_CTL` 指定非默认端点）：

```bat
win32\build\release\startup-gcc.exe app status
win32\build\release\startup-gcc.exe app stop stayalive
win32\build\release\startup-gcc.exe app restart stayalive
win32\build\release\startup-gcc.exe app status stayalive -s \\.\pipe\bl-ctl-smoke
```

```sh
./build/release/startup app status
./build/release/startup app stop b
./build/release/startup app restart b
./build/release/startup app shutdown    # 结束服务进程本身（子应用不受影响）
```

语义要点：

- 服务进程停止（`net stop` / `systemctl stop` / SIGTERM / `app shutdown`）**不会**
  杀掉子应用进程；控制接口随之消失。
- 停止子应用：POSIX 先发 SIGTERM，约 2 秒未退出再 SIGKILL；Win32 用
  `TerminateProcess`（立即强制结束，见已知限制）。
- `start/restart` 使用配置模型里的 `cmd`；改配置后需重启服务进程（或先 `rm` 再
  重新 `serve`）才会重新载入模型。
- 服务进程对子应用的退出做自然回收（POSIX `waitpid WNOHANG`；Win32
  `GetExitCodeProcess`），`status` 会把自然退出的应用标为 `stopped`。

## 构建

### Win32（本机已验证，两个编译器 × debug/release 共四份产物均零警告）

```bat
win32\build_win32.cmd
```

一次构建四种组合：

| 输出 | 编译器 | 配置 |
|---|---|---|
| `win32\build\debug\startup-gcc.exe` | MinGW-w64 | `-g -O0 -D_DEBUG` |
| `win32\build\release\startup-gcc.exe` | MinGW-w64 | `-O2 -DNDEBUG` |
| `win32\build\debug\startup-cl.exe`（+`.pdb/.ilk`） | MSVC 6.0 | `/Zi /Od /D_DEBUG /link /DEBUG` |
| `win32\build\release\startup-cl.exe` | MSVC 6.0 | `/O2 /DNDEBUG` |

脚本内已配置 VC6 所需的 `INCLUDE/LIB/PATH`（注意 VC6 的 cl.exe 依赖
`Common\MSDev98\Bin\mspdb60.dll`，必须加入 PATH）。发布打包只取 `release\`。

### POSIX

```sh
cd posix
make            # 默认 all：release(-O2 -DNDEBUG) + debug(-g -O0 -D_DEBUG)
make release    # 仅 build/release/startup
make debug      # 仅 build/debug/startup
make install    # release 版装到 /usr/local/bin，样例配置到 /usr/local/conf
```

若系统装有 GTK+3 开发文件（`pkg-config gtk+-3.0`，如 `apt install libgtk-3-dev`），
Makefile 会自动加 `-DHAVE_GUI` 编译 `gui_gtk.c` 并链接 GTK（其 include 以
`-isystem` 引入，第三方头文件不会污染 `-std=c89 -pedantic` 的零警告要求）；
未安装则退化为纯命令行/开机版本，其余行为不变。

已在 WSL Ubuntu 24.04 (gcc 13) 下验证零警告编译与全部行为。

## Win32 用法

```bat
win32\build\release\startup-gcc.exe                  :: 无参数：打开 GUI 配置/状态窗口
win32\build\release\startup-gcc.exe service          :: 服务入口（SCM 用；install 时自动写入 binPath）
win32\build\release\startup-gcc.exe serve            :: 前台 supervisor（不注册服务，也有控制接口）
win32\build\release\startup-gcc.exe install          :: 创建服务 AppBootLauncher（自动-延迟启动：所有自启动服务完成后、登录前拉起；非管理员自动 UAC 提权）
win32\build\release\startup-gcc.exe start | stop | uninstall   :: uninstall 会先停服务并等待其停止再真正删除
win32\build\release\startup-gcc.exe run | console -c test\order.conf   :: 不装服务，直接在控制台调试
win32\build\release\startup-gcc.exe help             :: 管理子命令列表（见上文“命令行管理接口”）
```

GUI 窗口功能（结构化编辑每个参数）：

- 左侧应用列表（新增/删除应用），右侧每个参数一个控件：
  名称、命令行（**带“浏览...”按钮**）、启动后等待毫秒、**依赖多选列表**（Ctrl/Shift 多选）；
- 改名会自动同步其他应用对它的依赖引用；删除应用会清除悬空引用；
- 保存配置：按列表顺序重新生成规范格式的 `conf\startup.conf`（共享 `cfg_save()`，与 CLI 产出一致）；
- 校验依赖顺序（打印拓扑序）、按序启动应用；
- 安装/卸载/启动/停止服务、刷新状态（显示服务安装与否、启动类型、运行状态）；
  安装/卸载在权限不足时自动触发 UAC 提权（弹窗确认后重试，GUI 无需以管理员运行）；
- **子应用进程控制区**：列表实时显示运行中服务进程所管理的应用（名称/状态/进程号，
  通过控制 IPC 的 `STATUS`），右侧按钮对选中行执行 启动应用/停止应用/重启应用/刷新列表；
  未找到服务进程时列表清空并在状态框提示端点；
- 核心日志（解析错误、环、启动记录）全部重定向到状态框。

### 脚本应用与解释器自动前置（两平台）

GUI 的“浏览...”按钮打开文件对话框，可筛选 `.exe` 及 `.bat/.cmd/.ps1/.py/.js/.vbs/.wsf/.pl/.sh/.lua`
脚本（GTK 版筛 `.sh/.bash/.py/.pl/.rb/.js/.lua` + 所有文件）。**配置与编辑框里只存带引号的脚本路径**；
真正的解释器命令行在**执行时**才由 `app_build_cmdline()`（Win32 在 `win32_service.c`，
POSIX 在 `posix_main.c`）即时生成。Win32 规则：

| 存储的 cmd | 实际执行 | 可用性检测 |
|---|---|---|
| `"D:\tools\app.exe"` | 直接执行 | — |
| `"D:\scripts\run.bat"` / `.cmd` | `cmd.exe /c "…bat"` | 总是可执行 |
| `"D:\scripts\task.ps1"` | `cmd.exe /c powershell.exe -NoProfile -ExecutionPolicy Bypass -File "…"` | `SearchPathA` 找 powershell |
| `"D:\scripts\x.py"`（.js/.vbs/…） | `"关联解释器" "脚本"` | `FindExecutableA` 查系统文件关联 |

POSIX 规则（有执行位的文件本就可直接运行，内核会认它的 shebang）：

| 存储的 cmd | 实际执行 | 可用性检测 |
|---|---|---|
| 有执行位的可执行文件 / shebang 脚本 | 直接执行 | — |
| 无执行位的 `.sh/.bash` | `/bin/sh "…"`、`/bin/bash "…"` | 解释器必然存在 |
| 无执行位的 `.py/.pl/.rb/.js/.lua` | `python3`/`perl`/`ruby`/`node`/`lua` `"…"` | 在 PATH 中探测该运行时 |
| 无执行位且扩展名未知（如 `.zzz`） | 拒绝启动 | — |

选择文件时就先检测：系统没有该类型的可执行方式 → 状态框提示，**不写入**命令行；
已选择脚本的“将实际运行为：…”完整命令显示在状态框（不在编辑框）。
执行侧同样兜底：无解释器的条目启动失败并记日志（服务不会崩）。带参数的完整命令行
（如 `python x.py --opt`）或 shell 命令不受影响，原样执行。示例见 `win32\test\scripts.conf`
（batapp/psapp 正常执行，unknownapp 被拒绝），可用于 `serve` 冒烟；POSIX 侧同类场景由
测试套件第 11 节自动验证（shebang 直启 / 无执行位 .sh 补 `/bin/sh` / .py 补 python3 /
无运行时拒绝）。

- 服务注册的 binPath 为 `"<exe>" service`，开机由 SCM 带 `service` 参数拉起。
- **开机启动被刻意推迟**：install 会把服务设为“自动(延迟启动)”
  （`ChangeServiceConfig2` + `SERVICE_CONFIG_DELAYED_AUTO_START_INFO`）。SCM 先完成全部
  普通自启动服务的启动，**开机流程收尾时才拉起本服务**（仍在任何用户登录之前），不与
  其他服务争抢启动资源；手动 `start` / GUI“启动服务”不受影响，随时可用。
- `service`/`serve` 都以 supervisor 模式常驻：启动全部应用后保留其句柄，并在命名管道
  `\\.\pipe\AppBootLauncher` 上提供 `app start|stop|restart|status` 控制接口
  （见“进程模型与控制接口”）；`net stop` 只结束服务进程，应用留在系统里。
  该管道带显式 DACL（创建者/管理员/系统全权，**交互用户读写**），因此普通用户会话的
  CLI/GUI 能直接控制以 LocalSystem 运行的服务。
- 服务默认读取 **`<exe 目录>\..\conf\startup.conf`**（`-c` 可覆盖）。
- 服务以 LocalSystem 运行（session 0），但启动应用时通过 `WTSGetActiveConsoleSessionId`
  + `WTSQueryUserToken` + `CreateProcessAsUserA` 落入**当前登录用户的桌面会话**——
  应用的环境块用 `CreateEnvironmentBlock(用户令牌)` 构建（`USERPROFILE`/`APPDATA` 等
  指向该用户；若沿用服务的 LocalSystem 环境块，依赖这些目录的应用会在启动瞬间死亡且
  无任何输出）；控制台应用无窗口（`CREATE_NO_WINDOW`），GUI 应用正常出现在用户桌面；
  无活动用户会话时回退普通 `CreateProcess`（POSIX 侧 `serve` 不涉及）。
- **服务日志自动写入 `<exe 目录>\..\logs\startup.log`**（启动时创建目录；设置环境变量
  `BOOTLAUNCHER_LOG=<路径>` 可改用任意文件）。日志同时仍输出到 stderr。
- `install`/`uninstall` 权限不足时自动重新以管理员身份运行自身（UAC 弹窗确认；环境变量
  `STARTUP_ELEV` 标记防止递归提权；用户取消弹窗则报 -1223）。`uninstall` 先向运行中的
  服务发停止请求并**等待其真正停止**（最长 10 秒）再 `DeleteService`，避免“只标记删除、
  服务仍在列表”的假象。
- 也可用系统命令注册：`sc create AppBootLauncher binPath= "\"D:\...\bin\startup-gcc.exe\" service" start= auto` 然后
  `sc config AppBootLauncher start= delayed-auto`（延迟启动）。
- 完整生命周期可用 `win32\test\svc_lifecycle.cmd` 一键验证（管理员直接跑；非管理员则
  install/uninstall 自动走 UAC 提权路径），含“卸载后 `sc query` 必须 1060（彻底消失）”断言。
- 编码说明：`win32\win32_gui.c` 与 `win32\conf\startup.conf` 保存为 **GBK（代码页 936）**，
  在中文系统上 VC6/MinGW 直接编译/读取即可正确显示中文；改成 UTF-8 会导致 GUI 乱码。
  （`.gitattributes` 已将这两个文件标记为 `binary`，git 不会做行尾/编码转换。）

## POSIX 图形配置窗口（GTK+3）

与 Windows 版对齐：无参数启动打开图形配置窗口，但先检测桌面会话——

1. 检测到 **GNOME 会话**（`XDG_CURRENT_DESKTOP`/`XDG_SESSION_DESKTOP` 含 gnome）→ 打开 GUI；
2. 否则若存在任意显示服务（`WAYLAND_DISPLAY` 或 `DISPLAY`，**WSLg 即此情形**）→ 尝试打开 GUI
   （连不上显示时 `gtk_init_check` 失败，自动退回第 3 条）；
3. 都没有（服务器开机、systemd/SysV/rc.local 环境）→ 保持原开机行为，按序启动应用；
4. `-d` 守护模式与任何子命令都不触发 GUI；`gui` 子命令可无条件强制打开。

GUI 功能与 Win32 版一致（复用同一套 `cfg_*`/`topo_sort`，保存产物字节级一致）：
应用列表（新增/删除）、每个参数一个控件（名称、命令行**带“浏览...”按钮**、等待毫秒、
依赖 Ctrl/Shift
多选）、改名跟随引用、删除清除悬空引用、保存配置、校验依赖顺序、按序启动应用、
核心日志重定向到状态框。服务安装/控制是 Win32 特有（Linux 由 systemd 管理），无此按钮。

测试辅助环境变量（无需人工点击）：

```sh
# 脚本化自检：载入模型、验证依赖预选、跑拓扑校验；状态行镜像到 stderr
APPBOOT_GUI_SELFTEST=1 APPBOOT_GUI_EXPECT_N=3 ./build/release/startup -c cfg.conf gui

# 自截图：窗口映射 0.8s 后用 cairo 渲染保存 PNG 并退出（X11/Wayland 后端都可用）
APPBOOT_GUI_SHOT=/tmp/gui.png ./build/release/startup -c cfg.conf
```

> WSLg：Windows 11 的 WSL 自带 Wayland/X11（`DISPLAY=:0`），`posix/build/gui-gtk.png`
> 即此方式截得的验证图。WSL 内没有 GNOME 会话，走的是第 2 条检测路径；WSLg 桥接没有
> 物理键盘 seat，gdk 会打印两条无害的 `gdk_seat_get_keyboard` CRITICAL，可忽略。

## POSIX 开机自启

- systemd：`cp posix/contrib/startup.service /etc/systemd/system/ && systemctl enable startup`
  （unit 用 `Type=simple` + `ExecStart=/usr/local/bin/startup serve`：服务进程常驻
  并提供控制接口；`KillMode=mixed` 保证 `systemctl stop` 只终止 supervisor，
  已启动的子应用进程留在系统里。无需 `-c`，配置自动解析为
  `/usr/local/conf/startup.conf`）
- SysV：`cp posix/contrib/S99startup /etc/init.d/`（start 用 `-d serve` 后台化；
  stop/status 通过 `app shutdown` / `app status` 控制接口实现）
- 或 rc.local：`/usr/local/bin/startup -d serve`
- 一次性开机（不需要控制接口）仍可用：`/usr/local/bin/startup -d`（启动完即退出）

## 测试

```sh
cd posix && sh test/run_posix_tests.sh
# WSL 下全量（含 install/systemd 用例需要 root）：
#   wsl -u root bash -c "cd /mnt/d/My/DEV/startup/posix && sh test/run_posix_tests.sh"
```

21 项检查，其中第 10 节专门测控制接口：用 `test/ctl.conf`（长驻 dummy 应用）起一个
自定义端点的 `serve`，走 `app ping/status/stop/restart/unknown/shutdown` 全序列，
并验证服务退出后 `app` 命令 rc=3；第 11 节验证脚本解释器自动解析（shebang 直启、
无执行位补 `/bin/sh`、`.py` 补 python3、未知扩展名拒绝启动）。

Win32 服务完整生命周期（含服务运行期间跨进程 `app status/stop` 控制接口验证，
以及“卸载后服务必须从 SCM 彻底消失（`sc query` 返回 1060）”断言）：
运行 `win32\test\svc_lifecycle.cmd`（管理员直接跑；非管理员时 install/uninstall
自动走 UAC 提权路径）；
`win32\test\ctl.conf` 配 `serve -s \\.\pipe\...` 可做前台控制接口冒烟，
`win32\test\svc.conf` 用于让已安装服务加载 disposable 测试应用（不碰真实应用）。

## 已通过的验证

- **用户环境块**（真机根因修复）：实机上服务以用户令牌启动的应用“启动即退出、
  重定向日志为空”——`CreateProcessAsUser` 传 `lpEnvironment=NULL` 时环境块继承的是
  服务的 **LocalSystem** 环境，`USERPROFILE` 指向 `C:\Windows\System32\config\systemprofile`
  而用户令牌对其无写权限。修复：`CreateEnvironmentBlock`（userenv.dll，动态加载）按
  用户令牌生成环境块传入。部署用 `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)` 排队
  （运行中的服务镜像无法原地覆盖）：下次开机 Session Manager 在服务启动前完成替换。
- **DSH 网关的无头启动接口**（可直接写进 startup.conf 的条目形式）：安装包自带 dsh CLI
  可纯 Node 模式运行——`"…\DeepSeek Harness.exe" --expose-internals
  "…\resources\app.asar\dsh\node_modules\@deepseek-ai\dsh\lib\bin.js" --profile web`
  （即官方 `dsh --profile web`：常驻 Web/网关 surface，无窗口；`--port`/`--no-open` 可选；
  认证 URL 打印到 stdout）。实测：独立端口下启动、无窗口常驻、带 token 的 URL 经
  `cmd /c … > 日志 2>&1` 包装落盘、HTTP 401（未认证探测的正常应答）、停止干净。
- **开机启动次序**：install 自动把服务设为“自动(延迟启动)”——SCM 完成全部普通自启动
  服务后才拉起本服务（属开机流程，先于任何用户登录；手动 start 不受影响）。实测
  install 后 `QueryServiceConfig2` 回读 `fDelayedAutostart=TRUE`，日志打印确认；
  随后 uninstall 仍 1060 彻底删除。注意：延迟信息不显示在 `sc qc` 的 START_TYPE 里
  （仍标 AUTO_START），在 services.msc 中显示为“自动(延迟启动)”。
- Win32 服务三问题修复 + GUI 进程控制（本机实测）：
  - **卸载**：对运行中的服务 `uninstall` 先 ControlService(STOP) 并轮询等待 STOPPED
    再 DeleteService；非管理员下自动 UAC 提权。实测真实机器上的运行服务：非管理员
    `uninstall` → `sc query` 返回 1060（彻底删除），随后 `install` 亦经 UAC 成功注册。
    `svc_lifecycle.cmd` 新增 GONE-OK（1060）断言，管理员模式一次通过。
  - **纯 GUI 应用**：服务经 `WTSGetActiveConsoleSessionId`+`WTSQueryUserToken`+
    `CreateProcessAsUserA` 把应用创建到当前用户会话；实测服务（session 0）启动的
    测试应用 owner=当前用户、session=1。`wtsapi32.dll` 运行时动态加载，VC6 无头文件/
    导入库依赖。同时修复控制管道的两处：`CreateNamedPipe` 的 SD 必须以
    `SECURITY_ATTRIBUTES` 包装（裸描述符会被静默忽略——之前普通用户连接报
    access denied 的根因），显式 DACL 授予交互用户读写；修复后非管理员 CLI/GUI
    对服务全部控制动词（ping/status/stop/start/restart）实测通过。
  - **服务日志**：服务进程自动写 `dist\logs\startup.log`（`BOOTLAUNCHER_LOG` 可覆盖）；
    通过 `putenv`（而非 `SetEnvironmentVariableA`——VC6 CRT 的 `getenv` 看不到后者）注入。
  - **GUI 子应用进程控制区**：SysListView32（名称/状态/进程号）+ 启动/停止/重启/刷新
    按钮，走 `STATUS`/`START`/`STOP`/`RESTART` IPC。程序化实测：选中行→点“停止应用”
    →`app status` 显示 stopped；点“启动应用”→ running 新 pid；点“重启应用”→ 换 pid。
    截图确认布局完整（窗口调整为 900x830）。
  - `package_win32.cmd` 不再整目录删除 dist：**现场配置 `dist\conf\startup.conf`
    永不覆盖**（曾被旧版脚本连带删除导致服务空配置快速退出——已恢复该文件并新增
    回归断言）；复制失败现在显式报错而非被 `>nul` 吞掉。
- 命名统一为 **startup**：POSIX 可执行文件 `bootlauncher` → `startup`（Makefile、
  `make install`、unit `contrib/startup.service`、`contrib/S99startup`、测试脚本同步）；
  版本信息中的应用名统一为 `startup`（`version`/`-V`、usage 头行、`PING` 应答
  `OK PONG startup <version>`、`appboot.rc` 的 ProductName/FileDescription/CompanyName、
  两版 GUI 标题 `… - startup v<版本>`、cfg_save 生成头注释）。
  有意保留的旧标识：Windows 服务名 `AppBootLauncher`、管道 `\\.\pipe\AppBootLauncher`、
  套接字 `/tmp/bootlauncher.ctl`、环境变量 `BOOTLAUNCHER_CTL`（均为接口标识，非版本信息）。
- 控制接口（三类进程模型）实测：
  - POSIX：`serve` + 默认/自定义端点下 `app ping/status/stop/restart/start ghost/shutdown`
    全序列正确；`stop` 走 SIGTERM→（2s）→SIGKILL；服务自然退出被 `status` 标记为
    `stopped`；`app shutdown` 后 supervisor 干净退出（rc=0），再执行 `app` 命令 rc=3。
    曾发现 fork 出的子应用继承控制连接 fd 导致应答无法 EOF 挂起，已用 `FD_CLOEXEC` 修复
    并被第 10 节回归覆盖。
  - Win32：`svc_lifecycle.cmd`（管理员）实测 安装→启动→`sc query` RUNNING→服务活跃期间
    另一进程 `app status`（长驻应用 `running`、已退出的探测应用被正确标 `stopped`）→
    `app stop`→`net stop`（自连 QUIT 释放 accept 线程，≤3s 内 STOPPED）→卸载，全程 rc=0；
    前台 `serve -s` 冒烟同样全序列通过（CreateProcess 不继承句柄，无 fd 泄漏问题）。
  - systemd unit 已改为 `Type=simple` + `serve`，套件第 6 节相应补充 `systemctl stop`
    清理（常驻服务不 stop 会跨运行泄漏，曾导致相邻用例互相干扰）。
- WSL Ubuntu 24.04 (gcc 13) + WSLg 实测 GTK+3 版：`-std=c89 -pedantic` 零警告编译（GTK 头走
  `-isystem`）；`gui` 子命令与无参数自动检测（Wayland/X11 路径）均正确打开窗口；
  自截图确认窗口完整渲染（中文标签、列表、表单、依赖多选预选、状态框载入 3 应用）；
  脚本化自检（载入 + 依赖预选 + 拓扑校验）通过；`APPBOOT_GUI_SHOT` cairo 自截图在 Wayland
  后端可用（服务端像素回读不可用，故不用截屏工具）。
- `wsl -u root` 下 `posix/test/run_posix_tests.sh` **18/18 通过**（新增 GTK GUI 自检与版本一致性用例；
  开机类用例显式 `env -u DISPLAY -u WAYLAND_DISPLAY` 以覆盖“无桌面=开机模式”语义）。
- 目录整理（共享 src/，win32/、posix/ 各带源文件+工程文件，产物在 <平台>/build，发布在 <平台>/dist/bin）后全量重验：
  - `win32\build_win32.cmd` 双编译器 × debug/release 四份产物零警告（debug 含 .pdb），产出到 `win32\build\{debug,release}\`；
  - `win32\build\release\startup-gcc.exe order` 不带 `-c` 通过两级回退正确解析 `win32\conf\startup.conf`，两个 exe 一致；
  - `wsl -u root` 下 `cd posix && sh test/run_posix_tests.sh` **18/18 通过**（debug/release 双配置、版本子命令、含 GTK GUI 自检）；
  - GUI 无参数启动正确读取新位置样例配置（截图确认）；
  - 发布包重新生成：`win32\dist\bin\*.exe`、`posix/dist/bin/bootlauncher`（含 `dist/conf`）。
- 双编译器（MinGW-w64 gcc 16.2 / MSVC 6.0 SP6）构建 Win32 版，零警告。
- gcc `-std=c89 -pedantic` 构建 POSIX 版，零警告。
- 乱序配置 → 实际按 first→second→third 启动（落盘证实），两个 exe 均通过（`win32/test/order.conf`、`posix/test/order.conf`）。
- 环检测（`cycle.conf`）与未知依赖（`baddep.conf`）：报错并列出问题应用，退出码非 0（两平台）。
- 版本 v0.1.1 实测：四个 Win32 exe 文件属性 `FileVersion=0.1.1`（windres 走 `-O coff` 链入 gcc，
  rc.exe 的 `.res` 经 `/link` 交给 VC6 link.exe）；两平台 `version` 子命令与 POSIX `-V` 输出一致；
  Win32 GUI 标题实测为 `开机启动应用管理器 - AppBootLauncher v0.1.1`，GTK 标题同样带版本。
- POSIX 测试套件 18 项：**version 子命令与 -V 输出一致**、**release+debug 双构建零警告**、**debug 版可运行且 order 正确**、前台顺序、环、未知依赖、`-d` 守护模式、`make install`（装到 `/usr/local/conf`）、systemd unit（`systemctl start` 后按序落盘）、**exe 相对 `../conf` 默认路径解析**、**CLI 管理接口 7 项**（list、order、add/set/dep/rename 往返、check 环检测与修复、run 应用编辑后的配置真实按新序启动、rm 清除引用、错误退出码）、**GTK GUI 自检**（模型载入、依赖预选计数、拓扑校验输出）。
- Win32 CLI 实测（两个 exe）：完整序列 `list/order/check/add/set/dep/rename` 正确；rename 后其他应用依赖引用跟随；人为制造环 → `check` 报错 rc=1，`dep del` 后恢复 OK；未知命令 rc=2 并打印完整用法。
- SysV 脚本 `/etc/init.d/bootlauncher start` 按序启动验证通过。
- 发布包布局实测：`dist\bin\*.exe` 从无关 cwd 启动，均正确读取同包 `conf/startup.conf` 并按序启动。
- Win32 GUI 实测（截图确认）：无参数启动打开窗口；模型载入应用、表单按选中项正确填充（名称/命令行/等待/依赖候选）；程序化点击“校验依赖顺序”→ 状态框打印正确拓扑序；点击“保存配置”→ 生成的文件被核心重新解析且顺序一致（往返闭环）；服务状态查询显示“未安装”。
- `service` 参数在非 SCM 环境正确拒绝并提示用 `console` 调试。
- 服务 install 在非管理员下正确报 err=5（权限不足；后已改为自动 UAC 提权重试，见上文
  “三问题修复”记录）；完整生命周期用 `win32\test\svc_lifecycle.cmd` 验证。

## 已知限制

- 单行配置最长 1024 字节；命令行最长 4096 字节。
- 依赖顺序是“启动顺序”保证，不是“就绪”保证；需要严格就绪语义时用 `wait` 或让被依赖应用自行监听端口/信号。
- Win32 的 `app stop/restart` 用 `TerminateProcess` 强制结束（无优雅退出宽限）；POSIX 为 SIGTERM→2s→SIGKILL。
- 控制接口单实例串行处理（同一时刻一个客户端）；模型只在服务进程启动时载入，
  改配置后需重启服务进程生效。
- 仅 ANSI（A 系）Win32 API；路径含非 ASCII 时建议配置文件与程序目录使用 ASCII 路径。
