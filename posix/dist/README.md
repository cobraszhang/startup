# 开机自动加载应用启动器（boot-time application launcher）

按配置文件中的 **依赖关系（deps）** 决定应用的启动顺序：先做拓扑排序，再按序启动。
提供两个版本，共享同一套纯 **C89** 核心代码：

| 版本 | 入口 | 开机自启方式 |
|---|---|---|
| Win32 | `src/win32_service.c`（服务/CLI 入口）+ `src/win32_gui.c`（无参数时的配置/状态窗口）+ 共享 `src/cli.c` | **Windows 系统服务**（`SERVICE_AUTO_START`，由 SCM 开机拉起） |
| POSIX | `src/posix_main.c`（开机/CLI 入口）+ 共享 `src/cli.c` | init / rc.local / SysV init.d / systemd unit 调用一次 |

## 目录结构

```
README.md .gitignore .gitattributes      根目录：说明与 git 文件
src/                仅放跨平台共享源文件（纯 C89）
  cfg.[ch]            配置解析 + 共享模型编辑/保存（add/rm/rename/set/deps、cfg_save）
  topo.[ch]           Kahn 拓扑排序 + 环检测 / 未知依赖检测
  log.[ch]            带时间戳的日志（可重定向到 GUI）
  cli.[ch]            共享命令行管理接口（Win32 与 POSIX 同一个 cli_main）
  version.h           发布版本号单一来源（当前 0.1.1）

win32/              Win32 平台源文件与工程文件
  win32_service.c     服务/CLI 入口（SCM 注册、CreateProcess 启动）
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
  posix_main.c        开机/CLI 入口（fork + /bin/sh -c，可 -d 后台化）
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
  bin\startup-gcc.exe         bin/bootlauncher
  bin\startup-cl.exe          conf/startup.conf
  conf\startup.conf           README.md
  README.md
```

- Windows：`win32\package_win32.cmd`（自动先调用 build_win32.cmd）
- POSIX：`cd posix && make package`

部署 = 把 `win32\dist`（或 `posix/dist`）整体拷到目标机器任意目录；服务注册：
`<安装目录>\bin\startup-gcc.exe install`（管理员）。

## 配置文件位置

默认配置文件目录为 **`../conf`（相对于可执行文件所在目录）**，文件名 `startup.conf`：

| 可执行文件 | 默认读取的配置 |
|---|---|
| `<安装目录>\bin\startup-gcc.exe` | `<安装目录>\conf\startup.conf` |
| `<安装目录>/bin/bootlauncher` | `<安装目录>/conf/startup.conf` |
| `/usr/local/bin/bootlauncher`（make install） | `/usr/local/conf/startup.conf` |
| 开发构建 `win32\build\{debug,release}\*.exe` | `win32\conf\startup.conf`（`..\..\conf` 回退） |
| 开发构建 `posix/build/{debug,release}/bootlauncher` | `posix/conf/startup.conf`（`../../conf` 回退） |

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
```

> `gui`（仅 POSIX 且构建时带 GTK）：强制打开图形配置窗口，不在共享 `cli.c` 子命令表内。

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
./build/release/bootlauncher list                             # 在 posix/ 目录下
./build/release/bootlauncher -c test/order.conf order
./build/release/bootlauncher -c /usr/local/conf/startup.conf add myapp "echo hi >> /tmp/x"
./build/release/bootlauncher -c /usr/local/conf/startup.conf set myapp wait 200
./build/release/bootlauncher -c /usr/local/conf/startup.conf dep add myapp appserver
./build/release/bootlauncher -c /usr/local/conf/startup.conf rename appserver app
```

退出码：`0` 成功；`1` 配置/依赖错误；`2` 用法错误。
`add` 生成的条目立即带合法 `cmd`，因此任何时刻配置文件都可被服务/开机流程读取。

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
make release    # 仅 build/release/bootlauncher
make debug      # 仅 build/debug/bootlauncher
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
win32\build\release\startup-gcc.exe install          :: 创建服务 AppBootLauncher（自动启动，需管理员）
win32\build\release\startup-gcc.exe start | stop | uninstall
win32\build\release\startup-gcc.exe run | console -c test\order.conf   :: 不装服务，直接在控制台调试
win32\build\release\startup-gcc.exe help             :: 管理子命令列表（见上文“命令行管理接口”）
```

GUI 窗口功能（结构化编辑每个参数）：

- 左侧应用列表（新增/删除应用），右侧每个参数一个控件：
  名称、命令行、启动后等待毫秒、**依赖多选列表**（Ctrl/Shift 多选）；
- 改名会自动同步其他应用对它的依赖引用；删除应用会清除悬空引用；
- 保存配置：按列表顺序重新生成规范格式的 `conf\startup.conf`（共享 `cfg_save()`，与 CLI 产出一致）；
- 校验依赖顺序（打印拓扑序）、按序启动应用；
- 安装/卸载/启动/停止服务、刷新状态（显示服务安装与否、启动类型、运行状态）；
- 核心日志（解析错误、环、启动记录）全部重定向到状态框。

- 服务注册的 binPath 为 `"<exe>" service`，开机由 SCM 带 `service` 参数拉起。
- 服务默认读取 **`<exe 目录>\..\conf\startup.conf`**（`-c` 可覆盖）。
- 服务以 LocalSystem 运行，启动的应用无交互桌面。
- 也可用系统命令注册：`sc create AppBootLauncher binPath= "\"D:\...\bin\startup-gcc.exe\" service" start= auto`。
- 完整生命周期可用 `win32\test\svc_lifecycle.cmd`（以管理员运行）一键验证。
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
应用列表（新增/删除）、每个参数一个控件（名称、命令行、等待毫秒、依赖 Ctrl/Shift
多选）、改名跟随引用、删除清除悬空引用、保存配置、校验依赖顺序、按序启动应用、
核心日志重定向到状态框。服务安装/控制是 Win32 特有（Linux 由 systemd 管理），无此按钮。

测试辅助环境变量（无需人工点击）：

```sh
# 脚本化自检：载入模型、验证依赖预选、跑拓扑校验；状态行镜像到 stderr
APPBOOT_GUI_SELFTEST=1 APPBOOT_GUI_EXPECT_N=3 ./build/release/bootlauncher -c cfg.conf gui

# 自截图：窗口映射 0.8s 后用 cairo 渲染保存 PNG 并退出（X11/Wayland 后端都可用）
APPBOOT_GUI_SHOT=/tmp/gui.png ./build/release/bootlauncher -c cfg.conf
```

> WSLg：Windows 11 的 WSL 自带 Wayland/X11（`DISPLAY=:0`），`posix/build/gui-gtk.png`
> 即此方式截得的验证图。WSL 内没有 GNOME 会话，走的是第 2 条检测路径；WSLg 桥接没有
> 物理键盘 seat，gdk 会打印两条无害的 `gdk_seat_get_keyboard` CRITICAL，可忽略。

## POSIX 开机自启

- systemd：`cp posix/contrib/bootlauncher.service /etc/systemd/system/ && systemctl enable bootlauncher`
  （unit 里的 `KillMode=process` 是必需的：launcher 启动应用后退出，默认
  `control-group` 模式会把 cgroup 里尚未退出的子进程一并杀掉；无需 `-c`，
  配置自动解析为 `/usr/local/conf/startup.conf`）
- SysV：`cp posix/contrib/S99bootlauncher /etc/init.d/`（脚本内部用 `-d` 后台化）
- 或 rc.local：`/usr/local/bin/bootlauncher -d &`

## 测试

```sh
cd posix && sh test/run_posix_tests.sh
# WSL 下全量（含 install/systemd 用例需要 root）：
#   wsl -u root bash -c "cd /mnt/d/My/DEV/startup/posix && sh test/run_posix_tests.sh"
```

Win32 服务完整生命周期：以管理员运行 `win32\test\svc_lifecycle.cmd`。

## 已通过的验证

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
- 服务 install 在非管理员下正确报 err=5（权限不足）；管理员下的完整生命周期请用 `win32\test\svc_lifecycle.cmd` 验证。

## 已知限制

- 单行配置最长 1024 字节；命令行最长 4096 字节。
- 依赖顺序是“启动顺序”保证，不是“就绪”保证；需要严格就绪语义时用 `wait` 或让被依赖应用自行监听端口/信号。
- 仅 ANSI（A 系）Win32 API；路径含非 ASCII 时建议配置文件与程序目录使用 ASCII 路径。
