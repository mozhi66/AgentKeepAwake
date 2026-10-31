# AgentKeepAwake v4.4 — 黑屏守护

**一句话**：不关显示器电源，只在所有屏幕最上层盖一张纯黑遮罩，同时让系统与显示器保持唤醒。屏幕看起来是黑的，Trae / Qoder / Codex / Claude Code / PowerShell 等任务照常在后台跑。

- 单文件原生 exe，**无运行时依赖**（不需要 .NET / Python / Node）
- **不需要管理员权限**：manifest 为 `asInvoker`，全程无 UAC，不改电源计划
- **通用**：Win10 / Win11 x64，多显示器、任意显卡、Modern Standby 或传统 S3 都可用
- **可移植**：绿色免安装，设置仅写 `HKCU`，日志仅写 `%TEMP%`
- 液态玻璃界面，全部自绘；17 个内置 Agent + 自定义名单；全局热键 `Ctrl+Alt+K`；托盘常驻

> v4.4 的核心变化：**彻底移除 `SC_MONITORPOWER` 关屏**。关屏 API 在 Modern Standby 机器上会让整机进入待机，Desktop Activity Moderator 会把普通桌面进程挂起，agent 随之冻结。现在改为「保持显示器唤醒 + 纯黑置顶遮罩」，从根上绕开这个坑。

---

## 1. 快速使用

1. 双击 `AgentKeepAwake.exe`
2. 点大按钮「开始黑屏守护」→ 当前屏幕立即变黑，窗口收进托盘
3. **动鼠标 / 点一下 / 按任意键** → 黑幕撤掉，屏幕内容恢复
4. 无操作 15 秒后自动再变黑（可选 关 / 5 / 15 / 30 秒）
5. 想彻底停止：`Ctrl+Alt+K`，或托盘右键 →「停止守护」
6. 「常亮防睡」模式：不盖黑幕，只阻止系统睡眠（适合需要一直看屏幕时）

关闭（✕）或最小化只是收进托盘，**不停止守护**；托盘右键「退出程序」才真正退出并释放全部电源请求。

| 设置 | 位置 |
|---|---|
| 唤醒后自动再黑屏 | 主界面「操作后自动再黑屏」右侧胶囊：关 / 5 / 15 / 30 秒 |
| 守护模式 | 主界面「守护模式」右侧胶囊：黑屏 + 防睡 / 常亮防睡 |
| 立即黑屏 | 托盘右键 →「立即黑屏」 |
| Agent 名单 | 同目录 `agents.ini` |
| 配置持久化 | 注册表 `HKCU\Software\AgentKeepAwake`（`ReblackSec`、`PrefMode`） |

**第一次点击会被黑幕吃掉**：黑幕需要先接到这次输入才会撤掉，所以唤醒用的那一下点击不会落到下面的窗口。移一下鼠标或按任意键也可以唤醒。

---

## 2. 它做了什么

### 2.1 双电源请求：系统不睡，显示器也不睡

守护线程持有：

```c
SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
```

- `ES_SYSTEM_REQUIRED`：阻止系统因空闲进入睡眠
- `ES_DISPLAY_REQUIRED`：阻止显示器因空闲关闭，从而**阻止「显示器空闲 → Modern Standby」这条链路**
- `ES_CONTINUOUS`：请求持续有效，线程存活期间无需反复续期（仍每 20 秒刷新一次做保险）
- 停止守护或退出进程时调用 `SetThreadExecutionState(ES_CONTINUOUS)` 释放请求，**不会残留**

源码：[`GuardThread()`](src/AgentKeepAwake4.cpp#L307-L335)

### 2.2 黑屏遮罩：只是视觉上的黑

一个独立的置顶窗口覆盖整个虚拟桌面：

- 样式：`WS_POPUP` + `WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`
- 画刷：纯黑 `BLACK_BRUSH`，不绘制任何内容
- 范围：`SM_XVIRTUALSCREEN / SM_YVIRTUALSCREEN / SM_CXVIRTUALSCREEN / SM_CYVIRTUALSCREEN`，多显示器一起盖住
- 显示器分辨率/布局变化：收到 `WM_DISPLAYCHANGE` 自动重贴边界
- 不抢焦点（`MA_NOACTIVATE`），不进任务栏、不进 Alt+Tab
- 遮罩期间吞掉鼠标点击，避免误操作下面的窗口

源码：[`BlackEnsureWindow()`](src/AgentKeepAwake4.cpp#L354-L376)、[`BlackShow()`](src/AgentKeepAwake4.cpp#L378-L403)、[`BlackWndProc`](src/AgentKeepAwake4.cpp#L412-L461)

### 2.3 唤醒：只观察，不拦截

| 输入 | 检测方式 |
|---|---|
| 鼠标移动 / 点击 | 遮罩窗口自己的 `WM_MOUSEMOVE` / `WM_LBUTTONDOWN` 等消息 |
| 键盘任意键 | `RegisterRawInputDevices(RIDEV_INPUTSINK)` 观察原始键盘事件 |
| 兜底 | 每秒一次 `GetLastInputInfo` 空闲时长轮询 |

- 遮罩刚盖上时有 1.5 秒防抖，避免启动瞬间的合成鼠标消息把黑幕秒关
- 键盘只判断「有按键发生」，**不记录键值、不拦截、不注入、不阻断**；停止守护或退出时立即 `RIDEV_REMOVE`
- 唤醒后遮罩隐藏，系统与显示器继续保持唤醒；空闲达到阈值再盖上

源码：[`RegisterRawKeyboard()`](src/AgentKeepAwake4.cpp#L463-L474)、[`BlackHideByInput()`](src/AgentKeepAwake4.cpp#L405-L410)、`WM_INPUT` / `WM_TIMER` 处理在 [`WndProc`](src/AgentKeepAwake4.cpp#L1801-L1832)

### 2.4 显示状态只用于诊断

订阅 `GUID_CONSOLE_DISPLAY_STATE`（`0=关 1=开 2=变暗`），**只写日志，不触发任何关屏/亮屏动作**：

- 守护中出现 `display: state=0` → 记一条 WARNING，说明显示器被系统或别的程序关掉了，agent 可能被挂起
- 收到 `PBT_APMSUSPEND` → 记录「整机要睡了」；`PBT_APMRESUMESUSPEND/AUTOMATIC` → 记录恢复，并把遮罩重新置顶
- 同时忽略投递给自己窗口的 `SC_MONITORPOWER` / `SC_SCREENSAVE`，不跟着别的软件一起关屏

源码：[`RegisterDisplayNotify()`](src/AgentKeepAwake4.cpp#L489-L499)；`WM_POWERBROADCAST` / `WM_SYSCOMMAND` 在 [`WndProc`](src/AgentKeepAwake4.cpp#L1834-L1877)

### 2.5 明确不做

- 不关显示器电源（无 `SC_MONITORPOWER`、无 `SendMessage(HWND_BROADCAST, …)`）
- 不睡眠/休眠系统（无 `SetSuspendState`、无 `ExitWindowsEx`）
- 不挂起、不终止、不限速任何进程（无 `SuspendThread` / `TerminateProcess` / `SetPriorityClass` / Job 对象）
- 不装全局钩子（无 `SetWindowsHookEx`），不注入（无 `CreateRemoteThread` / `WriteProcessMemory`）
- 不打开别人的进程句柄（进程检测只用 `CreateToolhelp32Snapshot` 读名字）
- 不拦截、不模拟输入（无 `BlockInput` / `SendInput` / `keybd_event`）
- 不改电源计划、不写 HKLM、不建服务/计划任务、不联网

---

## 3. 为什么不用「关屏 API」

### 3.1 问题现场

在 Modern Standby（S0 Low Power Idle / AOAC）的笔记本上：

1. 程序调用 `SC_MONITORPOWER` 或系统因显示器空闲关屏
2. 系统把「显示器关闭」当作进入 **Modern Standby** 的触发条件
3. Desktop Activity Moderator（DAM）冻结普通桌面进程
4. Trae / Codex / Node / PowerShell 里的 agent 全部停摆，任务卡死

旧版 v4.0 的日志里能看到完整的失败链路：`monitor-off posted` 之后紧接着 `PBT_APMSUSPEND`，随后 agent 心跳中断。即使只是「屏幕黑一下」，只要触发了显示器关闭，就可能把整机带进待机。

### 3.2 现在的做法

| 目标 | v4.0 旧方案 | v4.4 新方案 |
|---|---|---|
| 看起来黑屏 | `SC_MONITORPOWER` 真关显示器 | 置顶纯黑遮罩 |
| 系统不休眠 | `ES_SYSTEM_REQUIRED` | `ES_SYSTEM_REQUIRED`（不变） |
| 显示器不因空闲关闭 | 无（靠关屏） | `ES_DISPLAY_REQUIRED` |
| 会不会进 Modern Standby | **会**（关屏即触发） | **不会**（显示器始终唤醒） |
| 权限 | 「深度防睡配置」需 UAC + `powercfg` | 纯 `asInvoker`，零 UAC |
| 对第三方软件影响 | 广播关屏可能惊动锁屏监听软件 | 无广播、无系统级电源操作 |

### 3.3 实测结果（本机 HP ProBook，Modern Standby）

- 守护与黑屏期间的心跳进程：163 条记录，连续覆盖 95 秒，无中断
- 同期 `Kernel-Power 506`（Modern Standby 进入事件）：0 次
- 黑屏截图采样：1620 个采样点，平均亮度 0、最大亮度 0（纯黑）
- 空闲 15.9 秒后自动再黑屏；鼠标 / 键盘唤醒立即生效；停止守护后遮罩隐藏、电源请求释放

---

## 4. 兼容性与可移植性

| 项目 | 情况 |
|---|---|
| 系统 | Windows 10 / 11 x64 |
| 固件电源模型 | Modern Standby（S0ix）与传统 S3 都可用 |
| 显示器 | 单屏 / 多屏 / 不同 DPI 混排均可，遮罩按虚拟桌面覆盖 |
| 显卡 | 不依赖 GPU 能力，GDI 黑刷即可 |
| 运行时 | 无 .NET / Python / Node / VC++ 运行库依赖 |
| 安装 | 绿色单文件，放哪都能跑；配置在 HKCU，换目录不丢 |
| 权限 | `asInvoker`，普通用户直接运行，无 UAC、无管理员 |
| 写入位置 | 仅 `HKCU\Software\AgentKeepAwake` 与 `%TEMP%\AgentKeepAwake.log` |
| 退出清理 | 遮罩销毁、原始键盘注销、电源请求释放、托盘图标删除、热键注销 |

**它不能对抗的**：合盖、按电源键、开始菜单 →「睡眠」、其他软件主动请求系统睡眠、组策略强制待机、Windows 更新重启。这些是用户或系统的显式指令，防睡请求无权否决。

---

## 5. Agent 名单与扩展

### 内置 17 个（国内/国际版分开统计）

| 显示名 | 关键字 | 说明 |
|---|---|---|
| Trae | `=trae.exe` | 国际版 |
| Trae CN | `=trae cn.exe` | 国内版 |
| WorkBuddy | `=workbuddy.exe` | |
| WorkBuddy AI | `=workbuddyai.exe` | 国际版 |
| Qoder | `=qoder.exe,=qoder launcher.exe` | 国际版 |
| Qoder CN | `qoder cn` | 国内版 |
| DeepSeek | `deepseek` | DeepSeek Harness |
| ZCode | `=zcode.exe` | |
| OpenCode | `=opencode.exe` | |
| Hermes | `=hermes.exe` | |
| MiniMax | `=minimax code.exe` | MiniMax Code |
| MiMo | `=xiaomi mimo.exe` | 小米 MiMo |
| 妙手 | `=妙手.exe,miaoshou` | |
| Cursor | `=cursor.exe` | |
| Windsurf | `windsurf` | |
| Claude | `=claude.exe` | Claude Code CLI |
| Codex | `=codex.exe` | 必须精确，否则会命中后台服务 `codex-windows-sandbox-service` |

### 自定义：`agents.ini`（exe 同目录）

```ini
# 显示名 = 关键字1,关键字2,...
豆包 = doubao
Cline = cline
某个工具 = =准确的进程名.exe
```

- 默认**子串匹配**（进程名的一部分，不区分大小写）
- 关键字前加 `=` 表示**精确匹配整个进程名**（可省略 `.exe`）
- 一个名字可写多个关键字，逗号/分号/竖线分隔
- 最多 24 条；改完重开程序生效

### 界面规则

- 最多 8 个 chip（两行），**运行中的优先**
- 运行中不足 4 个时，用未运行的补齐，避免卡片空着
- 同时运行的超过 8 个时，第 8 个显示「+N 个」

### 排查「为什么某个没亮」

```powershell
# 1. 看真实进程名（exe 名，不是窗口标题）
Get-Process | Where-Object { $_.ProcessName -match 'miao|mimo|codex' } | Select-Object ProcessName

# 2. 看程序识别结果（只在变化时写一行）
Get-Content "$env:TEMP\AgentKeepAwake.log" -Tail 20 -Encoding UTF8
# 例：agents: running = [Trae, Trae CN, DeepSeek]
```

找不到进程，就说明关键字不对；把真实进程名填进 `agents.ini` 即可。

---

## 6. 二次开发

### 环境

- Windows 10 / 11 x64
- MinGW-w64 或 TDM-GCC（本项目用 TDM-GCC 10.3 验证）
- 无其他依赖

### 构建

```bat
build.cmd
```

或手动：

```bat
cd /d src
windres res.rc -o res.o
g++ -municode -mwindows -O2 -s -fno-exceptions -fno-rtti -fno-threadsafe-statics ^
    AgentKeepAwake4.cpp res.o -o AgentKeepAwake.exe ^
    -lgdiplus -lgdi32 -luser32 -lshell32 -ladvapi32
```

> **`-fno-exceptions -fno-rtti -fno-threadsafe-statics` 不是可选项。**
> TDM-GCC 静态链接 C++ 异常/RTTI 运行库的 exe 会被 Windows Defender 直接判为
> `Trojan:Win64/SpyLoader!pz` 并拒绝运行（`ERROR_VIRUS_INFECTED`）。
> 已逐项验证：GDI+、屏幕采样、进程枚举都不会触发，触发点就是异常运行时。
> 重新编译前请先退出正在运行的 exe，否则链接会因文件占用失败。

### 目录结构

```
AgentKeepAwake-v4/
├─ AgentKeepAwake.exe     成品（v4.4.0.0，可直接双击）
├─ AgentKeepAwake-v4.0-backup.exe  v4.0 备份
├─ agents.ini             自定义 Agent 名单（含注释示例）
├─ build.cmd              一键构建
├─ README.md              本文件
├─ preview-idle.png       界面截图（未开启）
├─ preview-active.png     界面截图（守护中）
└─ src/
   ├─ AgentKeepAwake4.cpp 全部逻辑（单文件，约 2077 行）
   ├─ res.rc              图标 + 版本信息 + manifest
   ├─ manifest.xml        DPI 感知 + Common Controls 6 + asInvoker
   ├─ icon.ico            应用图标
   ├─ icon_idle.ico       托盘图标（未开启）
   └─ icon_active.ico     托盘图标（守护中）
```

当前成品校验信息：

```
FileVersion : 4.4.0.0
Size        : 201216 bytes
SHA256      : fe1293112d59941860ba224fdbabfbdeb30f340361adc41b25cf5fcd27e5aa88
签名        : 未签名（自用工具；如介意请自行签名）
```

### 关键函数地图

> 行号基于 v4.4 源码；重构后请直接搜函数名。

| 函数 | 行 | 作用 |
|---|---|---|
| `LogMsg` | [276](src/AgentKeepAwake4.cpp#L276) | 写 `%TEMP%\AgentKeepAwake.log`（UTF-8，带时间戳与 pid） |
| `GuardThread` | [307](src/AgentKeepAwake4.cpp#L307) | 持有双电源请求的守护线程 |
| `BlackFitToDesktop` | [337](src/AgentKeepAwake4.cpp#L337) | 遮罩贴满虚拟桌面并置顶 |
| `BlackEnsureWindow` | [354](src/AgentKeepAwake4.cpp#L354) | 创建置顶纯黑遮罩窗口 |
| `BlackShow` / `BlackHideByInput` | [378](src/AgentKeepAwake4.cpp#L378) | 显示/隐藏遮罩 |
| `BlackWndProc` | [412](src/AgentKeepAwake4.cpp#L412) | 遮罩消息：鼠标唤醒、吞点击、分辨率变化 |
| `RegisterRawKeyboard` / `UnregisterRawKeyboard` | [463](src/AgentKeepAwake4.cpp#L463) / [475](src/AgentKeepAwake4.cpp#L475) | 原始键盘观察（仅知有按键） |
| `RegisterDisplayNotify` | [489](src/AgentKeepAwake4.cpp#L489) | 订阅显示器真实状态（仅日志） |
| `LoadSettings` / `SaveSettings` | [511](src/AgentKeepAwake4.cpp#L511) / [528](src/AgentKeepAwake4.cpp#L528) | HKCU 两个值：重黑秒数、默认模式 |
| `AddAgentDef` / `LoadAgents` | [552](src/AgentKeepAwake4.cpp#L552) / [580](src/AgentKeepAwake4.cpp#L580) | 内置 + `agents.ini` 名单 |
| `AgentKeyMatch` / `ScanAgents` | [637](src/AgentKeepAwake4.cpp#L637) / [654](src/AgentKeepAwake4.cpp#L654) | 匹配与 Toolhelp 快照扫描 |
| `StartProtection` / `StopProtection` | [726](src/AgentKeepAwake4.cpp#L726) / [791](src/AgentKeepAwake4.cpp#L791) | 守护生命周期（请求 → 遮罩 → 释放） |
| `BuildLayout` | [897](src/AgentKeepAwake4.cpp#L897) | 所有控件坐标（逻辑像素 × DPI） |
| `BuildBackdrop` | [1041](src/AgentKeepAwake4.cpp#L1041) | 采样桌面 → 压暗 → 柔光 → 缓存 |
| `PaintFrame` | [1358](src/AgentKeepAwake4.cpp#L1358) | 整帧绘制 + `UpdateLayeredWindow` 提交 |
| `HitTest` / `OnClick` / `TickAnimate` | [1635](src/AgentKeepAwake4.cpp#L1635) | 自绘控件命中/点击/动效 |
| `WndProc` | [1703](src/AgentKeepAwake4.cpp#L1703) | 主窗口消息（热键/托盘/电源/定时器） |
| `CleanupAndExit` | [1920](src/AgentKeepAwake4.cpp#L1920) | 退出清理 |
| `wWinMain` | [1945](src/AgentKeepAwake4.cpp#L1945) | 初始化、单实例、消息循环 |

### UI 架构

- 无边框 `WS_POPUP | WS_EX_LAYERED | WS_EX_APPWINDOW`
- 画布 = 面板 + 四周 30px 投影边距，32bpp 预乘 alpha DIB，GDI+ 绘制
- `UpdateLayeredWindow` 逐像素提交；窗口形状即 alpha
- 玻璃 = 启动时 `BitBlt` 桌面 → 压暗 → 三团柔光 → 降采样 + 箱式模糊 → 升采样缓存
- 所有控件自绘，命中测试手写（`rMain` / `rRb[]` / `rMd[]` / `rChip[]` …）
- 面板内点击被吞掉（`HTCLIENT`），投影区穿透（`HTTRANSPARENT`），顶栏空白可拖动（`HTCAPTION`）
- 动画只在有悬停/按下/守护光晕时重绘，隐藏或静止时不占 CPU

### 常见改动

- 加 Agent：不用改代码，写 `agents.ini`
- 改配色：源码顶部 `C_TXT` / `C_MINT` / `C_ROSE` 等常量
- 改尺寸/间距：`PANEL_W`、`PANEL_H`、`PAD`，以及 `BuildLayout()`
- 改默认值：`g_reblackSec`（默认 15 秒）、`g_prefMode`（1=黑屏+防睡，2=常亮防睡）
- 改遮罩颜色：`wWinMain()` 注册 `BLACK_CLASS` 时使用的 `BLACK_BRUSH`；若要半透明灰底，需自行给遮罩加 `WS_EX_LAYERED` 并设置 alpha

---

## 7. 代码审查要点

### 7.1 安全边界（可直接核对）

| 检查项 | 结果 |
|---|---|
| 全局钩子 `SetWindowsHookEx` | 无 |
| 进程注入 `CreateRemoteThread` / `WriteProcessMemory` | 无 |
| 打开他人进程 `OpenProcess` | 无（只用 Toolhelp 快照读名字） |
| 挂起/终止进程 `SuspendThread` / `TerminateProcess` | 无 |
| 进程降权/限速 `SetPriorityClass` / Job 对象 | 无 |
| 拦截/模拟输入 `BlockInput` / `SendInput` / `keybd_event` | 无 |
| 原始键盘观察 `RegisterRawInputDevices(RIDEV_INPUTSINK)` | **有**，仅黑屏模式启用，只感知「有按键」，不读键值、不拦截，停止/退出即注销 |
| 网络访问 `WSAStartup` / `socket` / `WinHTTP` | 无 |
| 电源相关 | 仅 `SetThreadExecutionState`；无 `SC_MONITORPOWER`、无 `SetSuspendState`、无 `ExitWindowsEx`、无 `powercfg` |
| 注册表写入 | 仅 `HKCU\Software\AgentKeepAwake`（`ReblackSec`、`PrefMode`） |
| 文件写入 | 仅 `%TEMP%\AgentKeepAwake.log`（追加）；读取同目录 `agents.ini` |
| 提权 | 无。manifest `requestedExecutionLevel level="asInvoker" uiAccess="false"` |
| 服务/计划任务/启动项 | 均不创建 |

### 7.2 资源占用（实机，守护中且窗口隐藏）

| 指标 | 数值 |
|---|---|
| CPU 增量 | 约 0.000 s / 10 s（静止时只有每秒一次的定时器） |
| 内存 | 约 33–40 MB（含三张缓存位图） |
| 自有线程 | 2（主线程 + 守护线程） |

### 7.3 日志验证

日志格式：`[YYYY-MM-DD HH:MM:SS.mmm pid=N] 内容`，UTF-8，位于 `%TEMP%\AgentKeepAwake.log`。
下面是字段示意，实际时间与 pid 以现场为准：

```
[2026-10-31 23:52:28.064 pid=13104] === AgentKeepAwake v4.4 start (pid=13104, dpi=96) ===
[2026-10-31 23:52:28.071 pid=13104] guard: hold ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED -> ok
[2026-10-31 23:52:28.075 pid=13104] start: protection ON (mode=1, reblack=15)
[2026-10-31 23:52:28.078 pid=13104] black: overlay ON (virtual 0,0 2560x1440)
[2026-10-31 23:53:31.123 pid=13104] black: idle -> overlay hidden
[2026-10-31 23:53:47.456 pid=13104] reblack: idle=15984 ms -> black
[2026-10-31 23:54:10.789 pid=13104] black: keyboard -> overlay hidden
[2026-10-31 23:54:10.790 pid=13104] agents: running = [Qoder, Codex]
[2026-10-31 23:55:20.001 pid=13104] guard: power request released
[2026-10-31 23:55:20.002 pid=13104] stop: protection OFF
```

如何快速判断守护是否正常：

| 日志 | 含义 |
|---|---|
| `guard: ... -> ok` | 双电源请求已生效 |
| `black: overlay ON (virtual …)` | 遮罩覆盖了整个虚拟桌面 |
| `reblack: idle=… -> black` | 自动再黑屏工作正常 |
| `black: keyboard/mouse/idle -> overlay hidden` | 唤醒路径工作正常 |
| `display: WARNING OS turned display off…` | 系统仍把显示器关了，agent 有被挂起风险 |
| `power: PBT_APMSUSPEND` | 整机进入睡眠/Modern Standby（防睡未拦住显式睡眠指令） |

### 7.4 代码里三处必须保留的“怪癖”

1. **绝不调用关屏 API，也不广播关屏**
   在 Modern Standby 机器上等于整机待机；广播还可能惊动第三方锁屏监听软件，打断正在跑的 agent。
2. **`WM_POWERBROADCAST` 里绝不调用 `Shell_NotifyIcon`**
   否则 explorer 重绘 → DWM 合成 → 触发显示状态变化 → 又刷新，容易形成抖动循环。回调里只改状态、写日志。
3. **遮罩刚显示时留 1.5 秒防抖，并且用 `GetLastInputInfo` 二次确认**
   启动瞬间系统会产生合成鼠标消息，不防抖的话遮罩会被自己秒关；点击「开始黑屏守护」的那一下鼠标也属于真实输入，需要靠空闲判定区分。合成消息（如 `SetCursorPos`）不更新空闲时间，不会被误判为唤醒。

---

## 8. 已知限制

1. **液晶背光仍然亮着**：像素是黑的，但面板背光不会关，功耗高于真正关屏。纯黑画面不会烧屏；如果是 OLED，黑像素本身不发光，观感更接近关屏，但依然不是断电。
2. **截屏类 agent 会拍到黑图**：遮罩是真实的最上层窗口，GDI/桌面复制/远程桌面看到的都是黑幕。需要截屏的自动化请用「常亮防睡」。
3. **唤醒的第一下点击不会传给下层窗口**：那一下被遮罩消费掉了；先移一下鼠标再点即可。第一次按键则会直接进入当前前台窗口（遮罩不抢焦点），打字时请注意。
4. **UAC / 安全桌面 / Ctrl+Alt+Del 永远在最上层**，黑幕盖不住；UAC 结束后遮罩可能被压到下面，动一下鼠标或按键即可恢复。
5. **显式睡眠指令无法阻止**：合盖、电源键、开始菜单「睡眠」、其他软件调用关屏/睡眠、组策略、Windows 更新重启，都可能让整机进入待机；守护只能阻止「空闲自动睡」。
6. **少数机器/策略会忽略 `ES_DISPLAY_REQUIRED`**：若日志反复出现 `display: WARNING` 或 `PBT_APMSUSPEND`，说明系统没有接受请求，请检查电源策略或改用「常亮防睡」并手动把「关闭显示器」设为「从不」。
7. **依赖进程存活**：程序退出、注销、重启后守护即失效；本工具不写启动项，需要开机自启请自行放入启动文件夹。
8. **Agent 只按进程名识别**：Claude Code 若以 `node.exe` 运行，会识别不到；在 `agents.ini` 里按实际进程名补一条即可（不影响守护本身）。
9. **自动再黑屏依赖空闲时长**：若某个设备/程序持续产生输入，空闲值永远达不到阈值，就不会自动再黑屏；可把选项设为「关」并手动控制。
10. **杀软误报**：见第 6 节编译参数说明，务必保留那三个 `-fno-*`。
11. **未签名**：SmartScreen 可能提示「未知发布者」，属正常现象；介意可自行签名。

---

## 9. 版本记录

| 版本 | 关键变化 |
|---|---|
| v3 | Win32 标准控件界面；广播关屏；可配置重黑 |
| v4 | 液态玻璃自绘 UI（分层窗口 + 毛玻璃 + 动效）；真实显示器状态驱动界面；Agent 运行状态卡片；空闲自动重黑（默认 15s） |
| v4.1 | Agent 名单可扩展（`agents.ini`） |
| v4.2 | 新增精确匹配 `=`；修 Codex 误亮（`codex-windows-sandbox-service`）；补 MiniMax / MiMo / 妙手 |
| v4.3 | 17 个内置、国内/国际版分开登记；超出 8 个显示「+N 个」；日志输出识别结果 |
| v4.4 | **改为纯黑遮罩 + `ES_DISPLAY_REQUIRED`**，彻底移除 `SC_MONITORPOWER` 与广播关屏，修复 Modern Standby 挂起 agent；移除需 UAC 的「深度防睡配置」，manifest 改为 `asInvoker`；增加原始键盘唤醒、多屏遮罩、1.5s 防抖、带时间戳日志 |

---

## 10. 免责说明

本项目为个人自用工具，代码可自由修改与分发。电源与显示相关操作涉及系统行为，请自行评估风险；因使用本工具造成的任务中断、授权失败、截屏黑图、功耗变化或系统行为差异，作者不承担责任。
