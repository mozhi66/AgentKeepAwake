# AgentKeepAwake v4 — 黑屏守护

**一句话**：把显示器关掉（省电、防烧屏），同时让系统保持不休眠，Trae / Codex / Claude Code / PowerShell 等任务继续跑。

- 单文件原生 exe，**无运行时依赖**（不需要 .NET / Python / Node）
- 液态玻璃界面，全部自绘
- 支持 17 个内置 Agent + 自定义名单，**国内版/国际版分开显示**
- 全局热键 `Ctrl+Alt+K`，托盘常驻

---

## 1. 快速使用

1. 双击 `AgentKeepAwake.exe`
2. 点大按钮「开始息屏守护」→ 屏幕立即关闭，窗口收进托盘
3. 动鼠标/按键 → 屏幕亮回；无操作 15 秒后自动再熄屏（可改 关 / 5 / 15 / 30 秒）
4. 想彻底停止：`Ctrl+Alt+K`，或托盘右键 →「停止守护」
5. 「仅防睡」模式：只阻止系统睡眠，不主动关屏（适合需要看屏幕时）

关闭窗口（✕）只是收进托盘，不会停止守护；托盘右键有「退出程序」。

| 设置 | 位置 |
|---|---|
| 唤醒后自动再熄屏 | 主界面「亮屏后自动再熄屏」右侧胶囊 |
| 守护模式 | 主界面「守护模式」右侧胶囊 |
| Agent 名单 | 同目录 `agents.ini` |
| 深睡相关电源配置 | 托盘右键「深度防睡配置（管理员）…」（可选，需 UAC） |

---

## 2. 它做了什么、不做什么

### 做的两件事

1. **阻止系统睡眠**（不是阻止关屏）：
   守护线程持有 `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)`。
   该请求属于**线程**，线程存活即有效，退出自动释放。
   源码：[`GuardThread()`](src/AgentKeepAwake4.cpp#L291-L300)

2. **只关显示输出**：
   `PostMessage(WM_SYSCOMMAND, SC_MONITORPOWER, 2)` 发给**自己的窗口**，由 `DefWindowProc` 执行。
   只在 3 秒内没收到系统「显示器已关」通知时，才回退一次广播兜底。
   源码：[`MonitorOff()`](src/AgentKeepAwake4.cpp#L309-L317)

### 显示器真实状态

订阅 `GUID_CONSOLE_DISPLAY_STATE`：`0=关 1=开 2=变暗`。
用来做三件事：状态灯/文案、判断「是不是真的关掉了」、判断是否被用户唤醒。
源码：[`RegisterDisplayNotify()`](src/AgentKeepAwake4.cpp#L331-L340)、`WM_POWERBROADCAST` 处理在 [`WndProc`](src/AgentKeepAwake4.cpp#L1531) 内。

### 明确不做

- 不挂起、不终止、不限速任何进程（无 `SuspendThread` / `TerminateProcess` / `SetPriorityClass` / Job 对象）
- 不装全局钩子（无 `SetWindowsHookEx`），不注入（无 `CreateRemoteThread` / `WriteProcessMemory`）
- 不拦截键鼠（无 `BlockInput` / `SendInput` / `keybd_event`）
- 不打开别人的进程句柄（进程检测只用 `CreateToolhelp32Snapshot` 读名字）
- 不改电源计划（除非你主动点「深度防睡配置」，且只改睡眠/休眠/合盖，**不改关屏时间**）

---

## 3. 为什么这样实现（几个踩过的坑）

| 问题 | 错误做法 | 正确做法 |
|---|---|---|
| 广播关屏会惊动别的软件 | `SendMessage(HWND_BROADCAST, …)` 让百度网盘等“锁屏监听”软件误弹「触发锁定失败」，打断 agent | 先发给自己的窗口；仅在没生效时兜底广播一次 |
| 显示器状态不能靠猜 | 自己记录“我关过屏了”，用户一动鼠标就与实际不符 | 订阅 `GUID_CONSOLE_DISPLAY_STATE` 系统通知 |
| 通知回调里不能碰 Shell | 回调里 `Shell_NotifyIcon` → explorer 重绘 → DWM 合成 → 刚关掉的屏又被点亮 → OFF/ON 抖动死循环 | 回调里只改状态，不动托盘 |
| 广播不能用 `SendMessageTimeout` | 显示器关闭时会被系统挂起，实测一次广播 103 秒 | 用 `PostMessage`（异步，1ms 返回） |
| 自动重黑不能按固定延时 | 用户还在用时会被强行黑屏 | 用 `GetLastInputInfo` 空闲时长判定，默认 15 秒 |
| msvcrt 宽字符日志乱码 | `_vsnwprintf` 里用 `%s`，只输出首字符 | 用 `%ls` |

---

## 4. Agent 名单与扩展

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

## 5. 二次开发

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
├─ AgentKeepAwake.exe     成品（可直接双击）
├─ agents.ini             自定义 Agent 名单（含注释示例）
├─ build.cmd              一键构建
├─ README.md              本文件
└─ src/
   ├─ AgentKeepAwake4.cpp 全部逻辑（单文件，约 1860 行）
   ├─ res.rc              图标 + 版本信息 + manifest
   ├─ manifest.xml        DPI 感知 + Common Controls 6
   ├─ icon.ico            应用/托盘图标（未开启）
   ├─ icon_idle.ico       托盘图标（未开启）
   └─ icon_active.ico     托盘图标（守护中）
```

### 关键函数地图

> 行号基于本版本源码；重构后请直接搜函数名。

| 函数 | 行 | 作用 |
|---|---|---|
| `LogMsg` | [264](src/AgentKeepAwake4.cpp#L264) | 写 `%TEMP%\AgentKeepAwake.log`（UTF-8） |
| `GuardThread` | [291](src/AgentKeepAwake4.cpp#L291) | 持有防睡请求的守护线程 |
| `MonitorOff` / `MonitorOn` | [309](src/AgentKeepAwake4.cpp#L309) | 关/开显示输出 + 兜底确认 |
| `RegisterDisplayNotify` | [331](src/AgentKeepAwake4.cpp#L331) | 订阅显示器真实状态 |
| `LoadAgents` / `AddAgentDef` | [422](src/AgentKeepAwake4.cpp#L422) | 内置 + `agents.ini` 名单 |
| `AgentKeyMatch` | [479](src/AgentKeepAwake4.cpp#L479) | 精确/子串匹配 |
| `ScanAgents` | [496](src/AgentKeepAwake4.cpp#L496) | Toolhelp 快照扫描进程名 |
| `StartProtection` / `StopProtection` | [566](src/AgentKeepAwake4.cpp#L566) | 守护生命周期 |
| `BuildLayout` | [727](src/AgentKeepAwake4.cpp#L727) | 所有控件坐标（逻辑像素 × DPI） |
| `BoxBlur` | [802](src/AgentKeepAwake4.cpp#L802) | 玻璃模糊（小图箱式模糊） |
| `BuildBackdrop` | [871](src/AgentKeepAwake4.cpp#L871) | 采样桌面 → 压暗 → 柔光 → 缓存 |
| `GlassCard` / `DrawOrb` / `DrawPill` / `DrawMainButton` | [1020](src/AgentKeepAwake4.cpp#L1020) | 玻璃卡片 / 状态球 / 胶囊 / 主按钮 |
| `PaintFrame` | [1188](src/AgentKeepAwake4.cpp#L1188) | 整帧绘制 + `UpdateLayeredWindow` 提交 |
| `HitTest` / `OnClick` / `TickAnimate` | [1463](src/AgentKeepAwake4.cpp#L1463) | 自绘控件的命中测试/点击/动效 |
| `WndProc` | [1531](src/AgentKeepAwake4.cpp#L1531) | 窗口消息 |
| `wWinMain` | [1751](src/AgentKeepAwake4.cpp#L1751) | 初始化、单实例、消息循环 |

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
- 改默认值：`g_reblackSec`（默认 15）、`g_prefMode`（1=息屏+防睡）

---

## 6. 代码审查要点

### 安全边界（可直接核对）

| 检查项 | 结果 |
|---|---|
| 全局钩子 `SetWindowsHookEx` | 无 |
| 进程注入 `CreateRemoteThread` / `WriteProcessMemory` | 无 |
| 打开他人进程 `OpenProcess` | 无（只用 Toolhelp 快照读名字） |
| 挂起/终止进程 `SuspendThread` / `TerminateProcess` | 无 |
| 进程降权/限速 `SetPriorityClass` / Job 对象 | 无 |
| 拦截输入 `BlockInput` / `SendInput` / `keybd_event` | 无 |
| 网络访问 `WSAStartup` / `socket` / `WinHTTP` | 无 |
| 注册表写入 | 仅 `HKCU\Software\AgentKeepAwake` 两个值（重黑秒数、模式） |
| 文件写入 | 仅 `%TEMP%\AgentKeepAwake.log`（追加）与读取同目录 `agents.ini` |
| 提权 | 仅托盘「深度防睡配置」主动触发一次 UAC（运行 `powercfg`） |

### 资源占用（实机，守护中且窗口隐藏）

| 指标 | 数值 |
|---|---|
| CPU 增量 | 0.000 s / 10 s |
| 内存 | 约 33–35 MB（含三张缓存位图） |
| 线程 | 7 |

### 日志验证

```
[pid] start: protection ON (mode=1, reblack=15)
[pid] guard: hold ES_CONTINUOUS|ES_SYSTEM_REQUIRED (no process is suspended)
[pid] screen: monitor-off posted (self)
[pid] display: state=0                 ← 系统确认显示器已关
[pid] display: state=1                 ← 被输入唤醒
[pid] reblack: idle=15891 ms -> off    ← 空闲 15.9 秒后自动再关
[pid] agents: running = [Trae, Trae CN, DeepSeek]
```

日志里区分「谁关的屏」：`monitor-off posted` 是本程序，只有 `display: state` 是系统播报。

### 代码里两处必须保留的“怪癖”

1. **`WM_POWERBROADCAST` 里绝不调用 `Shell_NotifyIcon`**
   否则 explorer 重绘 → DWM 合成 → 屏幕被点亮 → 又收到 ON → 又刷新，形成 OFF/ON 抖动死循环。
2. **关屏优先 PostMessage 给自己，广播只做兜底**
   广播会让第三方“锁屏监听”软件误弹窗，打断用户正在跑的 agent（实测百度网盘）。

---

## 7. 已知限制

1. **屏幕全黑时看不到 UAC / 授权弹窗**。Agent 中途需要授权时，先动鼠标亮屏。
2. **依赖截屏的 GUI 自动化可能拿到黑图**：显示器无输出时 Windows 可能停止合成。这类场景请用「仅防睡」。
3. **Claude Code 若以 `node.exe` 运行**，按 exe 名识别不到；可在 `agents.ini` 里按实际进程名补一条。
4. **自动重黑依赖 `GetLastInputInfo`**：若有设备/程序持续产生输入，空闲时长永远达不到阈值，就不会自动重黑（此时系统自身的关屏时间通常也不会生效）。可改用「关」并手动控制。
5. **杀软误报**：见第 5 节编译参数说明。
6. 「深度防睡配置」会改电源计划的睡眠/休眠/合盖设置且**不备份原值**，请自行 `powercfg` 恢复。
7. 守护只在当前进程生命周期内有效，程序退出即释放防睡请求（不会残留）。

---

## 8. 版本记录

| 版本 | 关键变化 |
|---|---|
| v3 | Win32 标准控件界面；广播关屏；可配置重黑 |
| v4 | **液态玻璃自绘 UI**（分层窗口 + 毛玻璃 + 动效）；关屏改“先自发、再兜底”；真实显示器状态驱动界面；Agent 运行状态卡片；空閒自动重黑（默认 15s） |
| v4.1 | Agent 名单可扩展（`agents.ini`） |
| v4.2 | 新增精确匹配 `=`；修 Codex 误亮（`codex-windows-sandbox-service`）；补 MiniMax / MiMo / 妙手 |
| v4.3 | 17 个内置、国内/国际版分开登记（Trae / Trae CN 可同时亮）；超出 8 个显示「+N 个」；日志输出识别结果 |

---

## 9. 免责说明

本项目为个人自用工具，代码可自由修改与分发。息屏/电源相关操作涉及系统行为，请自行评估风险；因使用本工具造成的任务中断、授权失败或电源设置变化，作者不承担责任。
