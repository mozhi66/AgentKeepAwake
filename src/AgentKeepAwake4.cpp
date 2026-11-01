/* ============================================================================
 * AgentKeepAwake v4.5 —— 黑屏守护（液态玻璃 UI）
 * ============================================================================
 *
 * 目标：让 Trae / WorkBuddy / Qoder / DeepSeek 等 agent 继续跑，
 *       同时让屏幕看起来是黑的（保护屏幕，避免长时间亮屏烧屏/耗电）。
 *
 * 做的事：
 *   1. 守护线程持有
 *      SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
 *      —— 系统和显示器都保持逻辑唤醒，避免 Windows 进入 Modern Standby
 *         把普通桌面进程挂起；不挂起、不限速任何进程。
 *   2. 在所有显示器上盖一层置顶、不抢焦点的纯黑窗口
 *      —— 不发送 SC_MONITORPOWER、不关显示器电源（S0 机器上那会触发整机待机）。
 *   3. 黑屏期间通过 WMI 把内置屏背光压到最低（可关），撤幕/退出时恢复；
 *      —— 只是背光，不改画面内容，不改变电源状态，普通权限即可调用。
 *   4. `AgentKeepAwake.exe --peek [秒]`：给截屏型 agent 用的接口，
 *      临时撤幕（并可恢复亮度）若干秒，超时自动再黑。截屏期间不重黑。
 *
 * 唤醒：鼠标移动/按键会立即撤掉黑屏；无操作 N 秒后自动再盖上（默认 15 秒，
 *       可切 关/5/15/30）。锁屏、合盖、电源键、开始菜单睡眠仍由系统接管。
 *
 * UI：无边框异形窗口 + 逐像素 alpha + 桌面毛玻璃底 + 半透明玻璃卡片 + 动效，
 *     全部由 GDI+ 自绘；无任何第三方运行时，不需要管理员权限。
 *
 * 构建（TDM-GCC / MinGW-w64 已验证）：
 *   windres res.rc -o res.o
 *   g++ -municode -mwindows -O2 -s -fno-exceptions -fno-rtti -fno-threadsafe-statics \
 *       AgentKeepAwake4.cpp res.o -o AgentKeepAwake.exe \
 *       -lgdiplus -lgdi32 -luser32 -lshell32 -ladvapi32 -lole32 -loleaut32 -lwbemuuid
 * ==========================================================================*/

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
/* 必须早于 windows.h：不声明目标版本时 MinGW 会让 GetTickCount64 隐式返回 int */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef _WIN32_DCOM
#define _WIN32_DCOM
#endif

#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <wbemidl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <gdiplus.h>
#include <math.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

using namespace Gdiplus;

/* ---------------------------------------------------------------- 基本常量 */
#define APP_CLASS    L"AgentKeepAwakeGlassWnd"
#define APP_TITLE    L"AgentKeepAwake · 黑屏守护"
#define APP_NAME     L"AgentKeepAwake"
#define APP_VER      L"v4.5"

#define WM_TRAY      (WM_USER + 10)
#define WM_AKA_SHOW  (WM_USER + 11)   /* 第二个实例请求显示主窗口 */
#define WM_AKA_PEEK  (WM_USER + 12)   /* 截屏前临时亮屏，wParam = 秒 */

#define BLACK_CLASS  L"AgentKeepAwakeBlackOverlay"

#define TIMER_UI     1
#define TIMER_CLOCK  2
#define TIMER_SCAN   3

/* Agent 名单容量：最多 24 条，界面同时显示 8 个 */
#define AGENT_MAX    24
#define AGENT_KEYS   4
#define AGENT_CHIPS  8

/* 逻辑像素（96 DPI 下的设计稿），运行时按 DPI 缩放 */
#define PANEL_W      520
#define PANEL_H      656
#define SHADOW_M     30      /* 四周留给投影的透明边距 */
#define PAD          22

#define SCALE(x)     MulDiv((x), (int)g_dpi, 96)

/* 电源请求标志 */
#ifndef ES_CONTINUOUS
#define ES_CONTINUOUS       0x80000000
#endif
#ifndef ES_SYSTEM_REQUIRED
#define ES_SYSTEM_REQUIRED  0x00000001
#endif
#ifndef ES_DISPLAY_REQUIRED
#define ES_DISPLAY_REQUIRED 0x00000002
#endif

#define AKA_DEVICE_NOTIFY_WINDOW_HANDLE 0x00000000

/* GUID_CONSOLE_DISPLAY_STATE —— 显示器 开/关/变暗 的系统通知 */
static const GUID GUID_DISPLAY_STATE =
    { 0x6FE69556, 0x704A, 0x47A0,
      { 0x8F, 0x24, 0xC2, 0x8D, 0x93, 0x6F, 0xDA, 0x47 } };

typedef struct {
    GUID  PowerSetting;
    DWORD DataLength;
    UCHAR Data[1];
} PBS_AKA;

typedef HANDLE (WINAPI *PFN_RegPowerNotify)(HANDLE, LPCGUID, DWORD);
typedef BOOL   (WINAPI *PFN_UnregPowerNotify)(HANDLE);

/* ---------------------------------------------------------------- 配色 */
static const Color C_TXT      (255, 244, 247, 252);
static const Color C_TXT2     (255, 170, 180, 196);
static const Color C_TXT3     (255, 124, 135, 152);
static const Color C_TXT4     (255,  96, 106, 122);
static const Color C_MINT     (255,  94, 226, 197);
static const Color C_BLUE     (255,  98, 142, 255);
static const Color C_ROSE     (255, 255, 126, 130);
static const Color C_DARKTXT  (255,   6,  26,  30);
static const Color C_GRAYDOT  (255,  92, 100, 114);

/* ---------------------------------------------------------------- 按钮 ID */
enum {
    B_NONE = -1,
    B_MAIN = 0,
    B_MIN,
    B_CLOSE,
    B_EXIT,
    B_RB0, B_RB1, B_RB2, B_RB3,
    B_MD0, B_MD1,
    B_BR0, B_BR1,
    B_COUNT
};

/* ---------------------------------------------------------------- 全局 */
static HINSTANCE   g_inst;
static HWND        g_hwnd;
static HDC         g_memDC;
static HBITMAP     g_memBmp;
static void       *g_memBits;
static Bitmap     *g_canvas;
static Bitmap     *g_backdrop;
static Bitmap     *g_glass;
static int         g_cw, g_ch;         /* 画布像素尺寸（含投影边距） */
static UINT        g_dpi = 96;
static ULONG_PTR   g_gdipToken;
static BOOL        g_quit;

/* 字体 */
static Font *fName, *fSub, *fStatus, *fStatusSub, *fMetricL, *fMetricV,
            *fBtn, *fPill, *fPillOn, *fChip, *fCardTitle, *fFoot, *fHint;

/* 布局（像素，面板坐标系） */
static RectF rPanel, rHero, rMain, rBtnMin, rBtnClose, rBtnExit,
             rAgents, rSettings, rRb[4], rMd[2], rBr[2], rChip[AGENT_CHIPS],
             rSettingsRow1,
             rSettingsRow2, rSettingsRow3, rPeekHint, rAgentHint, rAgentsFoot;

/* 交互 */
static int   g_hoverId = B_NONE;
static int   g_downId  = B_NONE;
static float g_hover[B_COUNT];
static float g_press[B_COUNT];
static BOOL  g_tracking;

/* 运行状态 */
static volatile LONG g_mode;           /* 0=关 1=黑屏+防睡 2=常亮防睡 */
static int   g_prefMode = 1;           /* 下次启动用哪种模式 */
static volatile LONG g_blackOn;        /* 纯黑遮罩是否正在显示 */
static DWORD g_lastDispState = 1;      /* 系统通知的真实显示状态: 0关 1开 2变暗 */
static DWORD g_blackAt;                /* 遮罩最近一次显示的时刻（防抖） */
static int   g_reblackSec = 15;
static ULONGLONG g_startTick;
static int   g_protectSec;
static float g_pulse;

/* 黑屏遮罩 / 后台键盘唤醒 */
static HWND g_blackWnd;
static BOOL g_rawKb;

/* 截屏窗口（peek）：遮住的时间点之前不再自动再黑 */
static DWORD g_peekUntil;
static int   g_peekSec = 3;

/* 背光（WMI root\WMI：只调内置屏背光，不关显示器、不改电源状态） */
static int   g_blackBright = 1;        /* 1=黑屏时把背光压到最低 */
static volatile LONG g_brightAvail;    /* 1=本机有可调背光 */
static volatile LONG g_brightWant;     /* -1=恢复用户亮度，>=0=目标档 */
static volatile LONG g_brightApplied = -1000;
static LONG    g_brightUser = -1;      /* 压低之前的用户亮度，用于恢复 */
static HANDLE  g_brightEvent;
static HANDLE  g_brightThread;
static volatile LONG g_brightStop;
static CRITICAL_SECTION g_brightCs;
static IWbemServices *g_wmiSvc;
static BOOL    g_wmiDead;

/* 守护线程 */
static HANDLE g_stopEvent;
static HANDLE g_guardThread;
static HANDLE g_guardReady;
static volatile LONG g_guardOk;

/* 显示器状态通知 */
static HANDLE g_displayNotify;

/* 托盘 */
static NOTIFYICONDATAW g_nid;
static BOOL g_trayOk;
static HICON g_iconIdle, g_iconActive;

/* Agent 进程检测：内置 11 个 + 同目录 agents.ini 自定义。
 * 只读进程名，不注入、不挂钩。 */
typedef struct {
    WCHAR name[28];
    WCHAR keys[AGENT_KEYS][32];
    int   nkeys;
    BOOL  running;
} AgentInfo;
static AgentInfo g_agents[AGENT_MAX];
static int g_agentCount;
static DWORD g_agentBits;   /* 运行中组合的位图，用于只在变化时写日志 */

static void LogMsg(const WCHAR *fmt, ...);
static void CleanupAndExit(HWND hwnd);

/* ---------------------------------------------------------------- 小工具 */
static RectF RF(float x, float y, float w, float h) { return RectF(x, y, w, h); }

static RectF ScaleAboutCenter(const RectF &r, float s)
{
    float w = r.Width * s, h = r.Height * s;
    return RectF(r.X + (r.Width - w) * 0.5f, r.Y + (r.Height - h) * 0.5f, w, h);
}

static void AddRound(GraphicsPath &p, const RectF &r, float rad)
{
    float d = rad * 2.0f;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;
    if (d <= 0.0f) { p.AddRectangle(r); return; }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

/* 单行文本；align: 0=左 1=中 2=右；垂直居中 */
static void Txt(Graphics &g, const WCHAR *s, Font *f, const RectF &r,
                const Color &c, int align = 0)
{
    StringFormat fmt;
    fmt.SetAlignment(align == 1 ? StringAlignmentCenter
                   : align == 2 ? StringAlignmentFar : StringAlignmentNear);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(s, -1, f, r, &fmt, &b);
}

static Font *MakeFont(float px, int style)
{
    static const WCHAR *fams[] = { L"Microsoft YaHei UI", L"Microsoft YaHei",
                                   L"Segoe UI", L"Arial" };
    for (int i = 0; i < 4; ++i) {
        FontFamily ff(fams[i]);
        if (ff.IsAvailable())
            return new Font(&ff, px, style, UnitPixel);
    }
    return new Font(L"Arial", px, style, UnitPixel);
}

static BOOL Step(float &v, float target, float k)
{
    float d = target - v;
    if (fabsf(d) < 0.004f) {
        if (v == target) return FALSE;
        v = target;
        return TRUE;
    }
    v += d * k;
    return TRUE;
}

static DWORD IdleMillis()
{
    LASTINPUTINFO lii;
    lii.cbSize = sizeof(lii);
    if (!GetLastInputInfo(&lii)) return 0;
    return (DWORD)(GetTickCount() - lii.dwTime);
}

/* ---------------------------------------------------------------- 日志 */
static void LogMsg(const WCHAR *fmt, ...)
{
    WCHAR path[MAX_PATH], buf[1024];
    WCHAR *p;
    FILE *fp;
    va_list ap;
    SYSTEMTIME st;

    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"AgentKeepAwake.log");
    fp = _wfopen(path, L"a, ccs=UTF-8");
    if (!fp) return;

    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    buf[1023] = 0;
    va_end(ap);

    p = buf;
    while (*p) {
        if (*p == L'\r' || *p == L'\n') *p = L' ';
        ++p;
    }
    GetLocalTime(&st);
    fwprintf(fp, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu] %ls\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds, GetCurrentProcessId(), buf);
    fclose(fp);
}

/* ---------------------------------------------------------------- 背光（WMI）
 * 只调内置屏背光档位：不关显示器、不发 SC_MONITORPOWER、不改任何电源状态，
 * 普通用户权限即可调用。所有 WMI 调用都在独立线程里做，UI 不卡。
 * 本机没有可调背光（台式机/外接屏/驱动不支持）时自动降级为空操作。 */
#define BRIGHT_RESTORE (-1)        /* 恢复用户原来的档位 */
#define BRIGHT_NONE    (-1000)     /* 没有待处理请求 */
#define BRIGHT_MIN     0           /* 压到最低档 */

static BOOL BrightReadDw(const WCHAR *name, DWORD *out)
{
    HKEY k;
    DWORD sz = sizeof(DWORD), t = 0;
    BOOL ok = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AgentKeepAwake", 0,
                      KEY_READ, &k) != ERROR_SUCCESS) return FALSE;
    if (RegQueryValueExW(k, name, NULL, &t, (BYTE *)out, &sz) == ERROR_SUCCESS &&
        t == REG_DWORD) ok = TRUE;
    RegCloseKey(k);
    return ok;
}

static void BrightWriteDw(const WCHAR *name, DWORD v)
{
    HKEY k;
    DWORD d;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AgentKeepAwake", 0, NULL, 0,
                        KEY_WRITE, NULL, &k, &d) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}

static BOOL BrightConnect(void)
{
    HRESULT hr;
    IWbemLocator *loc = NULL;
    BSTR ns;

    if (g_wmiSvc) return TRUE;
    if (g_wmiDead) return FALSE;

    ns = SysAllocString(L"root\\WMI");
    hr = CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, (LPVOID *)&loc);
    if (SUCCEEDED(hr) && loc)
        hr = loc->ConnectServer(ns, NULL, NULL, NULL, 0, NULL, NULL, &g_wmiSvc);
    if (loc) loc->Release();
    SysFreeString(ns);

    if (FAILED(hr) || !g_wmiSvc) {
        g_wmiSvc = NULL;
        g_wmiDead = TRUE;
        LogMsg(L"bright: WMI connect failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }
    hr = CoSetProxyBlanket(g_wmiSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                           RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           NULL, EOAC_NONE);
    if (FAILED(hr)) {
        g_wmiSvc->Release();
        g_wmiSvc = NULL;
        g_wmiDead = TRUE;
        LogMsg(L"bright: proxy blanket failed (0x%08lX)", (unsigned long)hr);
        return FALSE;
    }
    return TRUE;
}

static void BrightDisconnect(void)
{
    if (g_wmiSvc) {
        g_wmiSvc->Release();
        g_wmiSvc = NULL;
    }
}

/* 读当前背光档（0-100） */
static BOOL BrightGetLevel(LONG *out)
{
    BSTR lang = NULL, wql = NULL;
    IEnumWbemClassObject *en = NULL;
    IWbemClassObject *obj = NULL;
    VARIANT v;
    ULONG got = 0;
    BOOL ok = FALSE;
    HRESULT hr;

    lang = SysAllocString(L"WQL");
    wql = SysAllocString(L"select CurrentBrightness from WmiMonitorBrightness");
    hr = g_wmiSvc->ExecQuery(lang, wql,
                             WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                             NULL, &en);
    SysFreeString(wql);
    SysFreeString(lang);
    if (FAILED(hr) || !en) return FALSE;

    if (SUCCEEDED(en->Next(WBEM_INFINITE, 1, &obj, &got)) && got == 1 && obj) {
        VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, NULL, NULL))) {
            if (v.vt == VT_UI1)      { *out = (LONG)v.bVal;  ok = TRUE; }
            else if (v.vt == VT_I2)  { *out = (LONG)v.iVal;  ok = TRUE; }
            else if (v.vt == VT_I4)  { *out = (LONG)v.lVal;  ok = TRUE; }
            else if (v.vt == VT_UI4) { *out = (LONG)v.ulVal; ok = TRUE; }
        }
        VariantClear(&v);
        obj->Release();
    }
    en->Release();
    return ok;
}

/* 给每个 WmiMonitorBrightnessMethods 实例调用 WmiSetBrightness(Timeout, Brightness) */
static BOOL BrightSetLevel(int level)
{
    BSTR lang = NULL, wql = NULL, clsName = NULL, meth = NULL;
    IEnumWbemClassObject *en = NULL;
    IWbemClassObject *inst = NULL, *cls = NULL, *sig = NULL, *prm = NULL;
    VARIANT vt;
    ULONG got = 0;
    int done = 0;
    BOOL ok = FALSE;
    HRESULT hr;
    if (level < 0) level = 0;
    if (level > 100) level = 100;

    lang = SysAllocString(L"WQL");
    wql = SysAllocString(L"select * from WmiMonitorBrightnessMethods");
    hr = g_wmiSvc->ExecQuery(lang, wql,
                             WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                             NULL, &en);
    SysFreeString(wql);
    if (FAILED(hr) || !en) {
        LogMsg(L"bright: 查询 Methods 失败 (0x%08lX)", (unsigned long)hr);
        SysFreeString(lang);
        return FALSE;
    }

    clsName = SysAllocString(L"WmiMonitorBrightnessMethods");
    meth = SysAllocString(L"WmiSetBrightness");
    /* GetMethod 的第三个参数本身就是 InParameters 定义，直接 SpawnInstance */
    hr = g_wmiSvc->GetObject(clsName, 0, NULL, &cls, NULL);
    if (FAILED(hr)) LogMsg(L"bright: GetObject 失败 (0x%08lX)", (unsigned long)hr);
    if (SUCCEEDED(hr) && cls) {
        hr = cls->GetMethod(meth, 0, &sig, NULL);
        if (FAILED(hr)) LogMsg(L"bright: GetMethod 失败 (0x%08lX)", (unsigned long)hr);
        if (SUCCEEDED(hr) && sig) {
            hr = sig->SpawnInstance(0, &prm);
            if (FAILED(hr)) { LogMsg(L"bright: SpawnInstance 失败 (0x%08lX)",
                                     (unsigned long)hr); prm = NULL; }
        }
    }
    if (!prm) goto done_out;

    VariantInit(&vt);
    vt.vt = VT_UI1; vt.bVal = (UCHAR)level;
    hr = prm->Put(L"Brightness", 0, &vt, CIM_UINT8);
    if (FAILED(hr)) LogMsg(L"bright: Put Brightness 失败 (0x%08lX)", (unsigned long)hr);
    VariantInit(&vt);
    /* Timeout 是 CIM_UINT32，但本机 WMI 只接受 VT_I4 承载（用 VT_UI4 会报
       WBEM_E_TYPE_MISMATCH 0x80041005）——实测结论，别改回去 */
    vt.vt = VT_I4; vt.lVal = 0;                   /* 0 = 立即生效 */
    hr = prm->Put(L"Timeout", 0, &vt, CIM_UINT32);
    if (FAILED(hr)) LogMsg(L"bright: Put Timeout 失败 (0x%08lX)", (unsigned long)hr);

    while (en->Next(WBEM_INFINITE, 1, &inst, &got) == WBEM_S_NO_ERROR && got == 1) {
        VARIANT vp;
        if (!inst) break;
        VariantInit(&vp);
        if (SUCCEEDED(inst->Get(L"__PATH", 0, &vp, NULL, NULL)) && vp.vt == VT_BSTR) {
            hr = g_wmiSvc->ExecMethod(vp.bstrVal, meth, 0, NULL, prm, NULL, NULL);
            if (SUCCEEDED(hr)) ++done;
            else LogMsg(L"bright: WmiSetBrightness failed (0x%08lX) path=%ls",
                        (unsigned long)hr, vp.bstrVal ? vp.bstrVal : L"(null)");
        } else {
            LogMsg(L"bright: 实例没有 __PATH (vt=%d)", (int)vp.vt);
        }
        VariantClear(&vp);
        inst->Release();
        inst = NULL;
    }
    ok = done > 0;
    if (!ok) LogMsg(L"bright: 没有可用的背光控制实例（枚举到 %d 个）", got);

done_out:
    if (prm) prm->Release();
    if (sig) sig->Release();
    if (cls) cls->Release();
    SysFreeString(meth);
    SysFreeString(clsName);
    en->Release();
    SysFreeString(lang);
    return ok;
}

/* 记住“是我们把背光压低的”，程序被强杀后下次启动能自动恢复 */
static void BrightMark(int zeroed)
{
    if (zeroed) {
        if (g_brightUser >= 0) BrightWriteDw(L"UserBright", (DWORD)g_brightUser);
        BrightWriteDw(L"BrightZeroed", 1);
    } else {
        BrightWriteDw(L"BrightZeroed", 0);
    }
}

/* 必须在 g_brightCs 内调用。g_brightApplied：>=0 表示我们压到了该档，-1 表示已恢复 */
static BOOL BrightApply(LONG want)
{
    LONG target;

    if (want >= 0 && !g_blackBright) return FALSE;
    if (want < 0 && (LONG)g_brightApplied < 0) return FALSE;
    if (g_brightAvail < 0) return FALSE;         /* 探测过：本机没有可调背光 */
    if (!BrightConnect()) return FALSE;

    if (g_brightUser < 0 && !BrightGetLevel(&g_brightUser)) {
        LogMsg(L"bright: 读不到当前背光档，跳过");
        return FALSE;
    }

    target = (want < 0) ? g_brightUser : want;
    if (!BrightSetLevel((int)target)) return FALSE;

    InterlockedExchange(&g_brightApplied, want);
    BrightMark(want >= 0 ? 1 : 0);
    LogMsg(L"bright: 背光 -> %ld 档（%ls，用户档 %ld）",
           target, want >= 0 ? L"黑屏压到最低" : L"恢复用户亮度", g_brightUser);
    return TRUE;
}

/* 连一次 WMI，确认本机有没有可调背光，并记下用户当前档位 */
static void BrightProbe(void)
{
    LONG cur = -1;
    BOOL ok = FALSE;

    EnterCriticalSection(&g_brightCs);
    if (BrightConnect() && BrightGetLevel(&cur)) ok = TRUE;
    LeaveCriticalSection(&g_brightCs);

    if (ok) {
        if (g_brightUser < 0) g_brightUser = cur;   /* 崩溃恢复时已带上次的值，不覆盖 */
        InterlockedExchange(&g_brightAvail, 1);
        LogMsg(L"bright: panel ok (level=%ld, 将恢复=%ld)", cur, g_brightUser);
    } else {
        InterlockedExchange(&g_brightAvail, -1);
        LogMsg(L"bright: 本机没有可调背光（外接屏或驱动不支持），只用纯黑遮罩");
    }
}

static DWORD WINAPI BrightThread(LPVOID)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    BrightProbe();

    for (;;) {
        LONG want;

        if (WaitForSingleObject(g_brightEvent, INFINITE) != WAIT_OBJECT_0) break;
        if (g_brightStop) break;

        want = (LONG)InterlockedExchange(&g_brightWant, BRIGHT_NONE);
        if (want == BRIGHT_NONE || (LONG)g_brightApplied == want) continue;

        EnterCriticalSection(&g_brightCs);
        if (BrightApply(want)) InterlockedExchange(&g_brightAvail, 1);
        else if (g_wmiDead)    InterlockedExchange(&g_brightAvail, -1);
        LeaveCriticalSection(&g_brightCs);
    }

    BrightDisconnect();
    CoUninitialize();
    return 0;
}

static void BrightAsk(LONG want)
{
    if (!g_brightEvent) return;
    InterlockedExchange(&g_brightWant, want);
    SetEvent(g_brightEvent);
}

static void BrightStart(void)
{
    DWORD v = 0;
    LONG saved = -1;

    InitializeCriticalSection(&g_brightCs);

    /* 上次没来得及恢复（被强杀/断电）：这次启动先补回来 */
    if (BrightReadDw(L"BrightZeroed", &v) && v == 1 &&
        BrightReadDw(L"UserBright", &v) && v >= 0 && v <= 100) {
        saved = (LONG)v;
    }
    if (saved >= 0) {
        g_brightUser = saved;
        InterlockedExchange(&g_brightApplied, 0);   /* 标记“被压低过”，触发恢复 */
        LogMsg(L"bright: 上次异常退出，恢复背光到 %ld", saved);
    }

    g_brightEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_brightEvent) return;

    g_brightThread = CreateThread(NULL, 0, BrightThread, NULL, 0, NULL);
    if (!g_brightThread) {
        CloseHandle(g_brightEvent);
        g_brightEvent = NULL;
        return;
    }
    if (saved >= 0) BrightAsk(BRIGHT_RESTORE);
}

/* 同步恢复：退出路径用，避免线程还没来得及跑就进程没了 */
static void BrightRestoreNow(void)
{
    HRESULT hr;

    if (!g_brightEvent || (LONG)g_brightApplied < 0) return;   /* 没压低过，无需恢复 */

    InterlockedExchange(&g_brightStop, 1);
    SetEvent(g_brightEvent);

    EnterCriticalSection(&g_brightCs);
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        if (BrightConnect()) BrightApply(BRIGHT_RESTORE);
        BrightDisconnect();
        CoUninitialize();
    }
    LeaveCriticalSection(&g_brightCs);

    if (g_brightThread) {
        WaitForSingleObject(g_brightThread, 2000);
        CloseHandle(g_brightThread);
        g_brightThread = NULL;
    }
    CloseHandle(g_brightEvent);
    g_brightEvent = NULL;
    DeleteCriticalSection(&g_brightCs);
}

/* ---------------------------------------------------------------- 电源内核 */
static DWORD WINAPI GuardThread(LPVOID)
{
    EXECUTION_STATE es;

    /* ES_DISPLAY_REQUIRED 是关键：显示空闲计时器不再归零，Windows 不会因为
     * “屏幕该关了”而进入 Modern Standby，桌面进程也就不会被 DAM 挂起。 */
    es = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
                                 ES_DISPLAY_REQUIRED);
    InterlockedExchange(&g_guardOk, es ? 1 : 0);
    LogMsg(L"guard: hold ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED -> %ls",
           es ? L"ok" : L"FAILED");
    if (g_guardReady) SetEvent(g_guardReady);
    if (!es) return 0;

    while (WaitForSingleObject(g_stopEvent, 20000) == WAIT_TIMEOUT) {
        es = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
                                     ES_DISPLAY_REQUIRED);
        if (!es) LogMsg(L"guard: refresh FAILED");
    }

    SetThreadExecutionState(ES_CONTINUOUS);
    LogMsg(L"guard: power request released");
    return 0;
}

/* ---------------------------------------------------------------- 黑屏遮罩
 * 纯黑、置顶、不抢焦点、不在任务栏/Alt+Tab 出现，覆盖整个虚拟桌面。
 * 不发送 SC_MONITORPOWER：在 Modern Standby (S0) 机器上那等于让整机待机，
 * 桌面进程会被 DAM 挂起，agent 随之停摆。
 * 遮罩会吃掉鼠标消息：任何移动/点击都会先撤掉它，避免在黑屏下误点到桌面。 */
static void BlackFitToDesktop()
{
    int x, y, w, h;

    if (!g_blackWnd) return;
    x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) {
        x = 0; y = 0;
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
    }
    SetWindowPos(g_blackWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

static void BlackEnsureWindow()
{
    int x, y, w, h;

    if (g_blackWnd && IsWindow(g_blackWnd)) return;

    x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) {
        x = 0; y = 0;
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
    }

    g_blackWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        BLACK_CLASS, L"", WS_POPUP,
        x, y, w, h, NULL, NULL, g_inst, NULL);
    if (!g_blackWnd)
        LogMsg(L"black: CreateWindowEx failed (%lu)", GetLastError());
}

static void BlackShow(BOOL on)
{
    if (!on) {
        if (g_blackWnd) ShowWindow(g_blackWnd, SW_HIDE);
        if (InterlockedExchange(&g_blackOn, 0)) {
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
            BrightAsk(BRIGHT_RESTORE);
            LogMsg(L"black: overlay OFF");
        }
        return;
    }

    BlackEnsureWindow();
    if (!g_blackWnd) return;
    if (InterlockedCompareExchange(&g_blackOn, 1, 0) != 0) return;

    BlackFitToDesktop();
    g_blackAt = GetTickCount();
    SetWindowPos(g_blackWnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetCursor(NULL);
    BrightAsk(BRIGHT_MIN);
    LogMsg(L"black: overlay ON (virtual %d,%d %dx%d)",
           GetSystemMetrics(SM_XVIRTUALSCREEN),
           GetSystemMetrics(SM_YVIRTUALSCREEN),
           GetSystemMetrics(SM_CXVIRTUALSCREEN),
           GetSystemMetrics(SM_CYVIRTUALSCREEN));
}

static void BlackHideByInput(const WCHAR *why)
{
    if (!g_blackOn) return;
    BlackShow(FALSE);
    LogMsg(L"black: %ls -> overlay hidden", why);
}

static LRESULT CALLBACK BlackWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR:
        SetCursor(NULL);
        return TRUE;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEMOVE:
        /* 窗口刚出现时系统会补一条合成 mousemove，它不更新 GetLastInputInfo；
         * 只有“确实有真实输入”才撤掉遮罩，避免刚盖黑就被自己唤醒。 */
        if (GetTickCount() - g_blackAt >= 1500 && IdleMillis() < 1500)
            BlackHideByInput(L"mouse");
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        BlackHideByInput(L"mouse button");
        return 0;

    case WM_DISPLAYCHANGE:
        BlackFitToDesktop();
        return 0;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MONITORPOWER) {
            LogMsg(L"black: ignored external SC_MONITORPOWER");
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void RegisterRawKeyboard()
{
    RAWINPUTDEVICE rid;

    rid.usUsagePage = 0x01;   /* Generic Desktop */
    rid.usUsage     = 0x06;   /* Keyboard */
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = g_hwnd;
    g_rawKb = RegisterRawInputDevices(&rid, 1, sizeof(rid));
    LogMsg(L"black: raw keyboard wake %ls", g_rawKb ? L"ok" : L"unavailable");
}

static void UnregisterRawKeyboard()
{
    RAWINPUTDEVICE rid;

    if (!g_rawKb) return;
    rid.usUsagePage = 0x01;
    rid.usUsage     = 0x06;
    rid.dwFlags     = RIDEV_REMOVE;
    rid.hwndTarget  = NULL;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    g_rawKb = FALSE;
    LogMsg(L"black: raw keyboard wake released");
}

static void RegisterDisplayNotify(HWND hwnd)
{
    PFN_RegPowerNotify reg = (PFN_RegPowerNotify)(void *)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "RegisterPowerSettingNotification");
    if (!reg) { LogMsg(L"display: RegisterPowerSettingNotification missing"); return; }

    g_displayNotify = reg((HANDLE)hwnd, &GUID_DISPLAY_STATE,
                          AKA_DEVICE_NOTIFY_WINDOW_HANDLE);
    LogMsg(L"display: notify handle=%p", g_displayNotify);
}

static void UnregisterDisplayNotify()
{
    PFN_UnregPowerNotify unreg;
    if (!g_displayNotify) return;
    unreg = (PFN_UnregPowerNotify)(void *)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "UnregisterPowerSettingNotification");
    if (unreg) unreg(g_displayNotify);
    g_displayNotify = NULL;
}

/* ---------------------------------------------------------------- 设置持久化 */
static void LoadSettings()
{
    HKEY k;
    DWORD v = 0, sz = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AgentKeepAwake", 0,
                      KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"ReblackSec", NULL, NULL, (BYTE *)&v, &sz) == ERROR_SUCCESS) {
            if (v == 0 || v == 5 || v == 15 || v == 30) g_reblackSec = (int)v;
        }
        sz = sizeof(v);
        if (RegQueryValueExW(k, L"PrefMode", NULL, NULL, (BYTE *)&v, &sz) == ERROR_SUCCESS) {
            if (v == 1 || v == 2) g_prefMode = (int)v;
        }
        sz = sizeof(v);
        if (RegQueryValueExW(k, L"BlackBright", NULL, NULL, (BYTE *)&v, &sz) == ERROR_SUCCESS) {
            if (v == 0 || v == 1) g_blackBright = (int)v;
        }
        sz = sizeof(v);
        if (RegQueryValueExW(k, L"PeekSec", NULL, NULL, (BYTE *)&v, &sz) == ERROR_SUCCESS) {
            if (v >= 1 && v <= 60) g_peekSec = (int)v;
        }
        RegCloseKey(k);
    }
}

static void SaveSettings()
{
    HKEY k;
    DWORD d;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AgentKeepAwake", 0, NULL, 0,
                        KEY_WRITE, NULL, &k, &d) != ERROR_SUCCESS) return;
    d = (DWORD)g_reblackSec;
    RegSetValueExW(k, L"ReblackSec", 0, REG_DWORD, (const BYTE *)&d, sizeof(d));
    d = (DWORD)g_prefMode;
    RegSetValueExW(k, L"PrefMode", 0, REG_DWORD, (const BYTE *)&d, sizeof(d));
    d = (DWORD)g_blackBright;
    RegSetValueExW(k, L"BlackBright", 0, REG_DWORD, (const BYTE *)&d, sizeof(d));
    RegCloseKey(k);
}

/* ---------------------------------------------------------------- Agent 检测 */
static void TrimW(WCHAR *s)
{
    WCHAR *p = s, *e;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') ++p;
    if (p != s) memmove(s, p, ((size_t)lstrlenW(p) + 1) * sizeof(WCHAR));
    e = s + lstrlenW(s);
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t' ||
                     e[-1] == L'\r' || e[-1] == L'\n')) *--e = 0;
}

static void AddAgentDef(const WCHAR *name, const WCHAR *keys)
{
    AgentInfo *a;
    WCHAR buf[128], *p, *q;

    if (g_agentCount >= AGENT_MAX) return;
    a = &g_agents[g_agentCount];
    ZeroMemory(a, sizeof(*a));
    lstrcpynW(a->name, name, 28);

    lstrcpynW(buf, keys, 128);
    p = buf;
    while (*p && a->nkeys < AGENT_KEYS) {
        q = p;
        while (*q && *q != L',' && *q != L';' && *q != L'|') ++q;
        if (*q) *q++ = 0;
        TrimW(p);
        if (*p) {
            lstrcpynW(a->keys[a->nkeys], p, 32);
            CharLowerBuffW(a->keys[a->nkeys], (DWORD)lstrlenW(a->keys[a->nkeys]));
            a->nkeys++;
        }
        p = q;
    }
    if (a->nkeys > 0) g_agentCount++;
}

/* 内置名单 + 同目录 agents.ini（UTF-8 或 ANSI，每行：显示名=关键字1,关键字2） */
static void LoadAgents()
{
    WCHAR path[MAX_PATH], line[256];
    WCHAR *p;
    FILE *fp;

    g_agentCount = 0;
    /* 国内版 / 国际版是不同 exe，分开登记（= 表示精确整名，互不串台） */
    AddAgentDef(L"Trae",         L"=trae.exe");
    AddAgentDef(L"Trae CN",      L"=trae cn.exe");
    AddAgentDef(L"WorkBuddy",    L"=workbuddy.exe");
    AddAgentDef(L"WorkBuddy AI", L"=workbuddyai.exe");
    AddAgentDef(L"Qoder",        L"=qoder.exe,=qoder launcher.exe");
    AddAgentDef(L"Qoder CN",     L"qoder cn");
    AddAgentDef(L"DeepSeek",     L"deepseek");          /* DeepSeek Harness */
    AddAgentDef(L"ZCode",        L"=zcode.exe");
    AddAgentDef(L"OpenCode",     L"=opencode.exe");
    AddAgentDef(L"Hermes",       L"=hermes.exe");
    AddAgentDef(L"MiniMax",      L"=minimax code.exe");
    AddAgentDef(L"MiMo",         L"=xiaomi mimo.exe");
    AddAgentDef(L"妙手",         L"=妙手.exe,miaoshou");
    AddAgentDef(L"Cursor",       L"=cursor.exe");
    AddAgentDef(L"Windsurf",     L"windsurf");
    AddAgentDef(L"Claude",       L"=claude.exe");
    /* Codex 必须精确：否则会命中后台服务 codex-windows-sandbox-service */
    AddAgentDef(L"Codex",        L"=codex.exe");

    GetModuleFileNameW(NULL, path, MAX_PATH);
    p = wcsrchr(path, L'\\');
    if (p) p[1] = 0;
    lstrcatW(path, L"agents.ini");

    fp = _wfopen(path, L"r, ccs=UTF-8");
    if (!fp) fp = _wfopen(path, L"r");
    if (!fp) {
        LogMsg(L"agents: builtin %d, no agents.ini", g_agentCount);
        return;
    }
    while (fgetws(line, 256, fp)) {
        WCHAR *eq;
        TrimW(line);
        if (!line[0] || line[0] == L'#' || line[0] == L';' || line[0] == L'[')
            continue;
        eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0;
        TrimW(line);
        TrimW(eq + 1);
        if (line[0] && eq[1]) AddAgentDef(line, eq + 1);
    }
    fclose(fp);
    LogMsg(L"agents: total %d (builtin + agents.ini)", g_agentCount);
}

/* 关键字匹配：默认“进程名包含关键字”；关键字以 = 开头表示精确匹配整名
 * （可省略 .exe）。用来排除同名后台服务，例如 =codex.exe 不会命中
 * codex-windows-sandbox-service。 */
static BOOL AgentKeyMatch(const WCHAR *exeLow, const WCHAR *key)
{
    if (key[0] == L'=') {
        const WCHAR *k = key + 1;
        if (lstrcmpW(exeLow, k) == 0) return TRUE;
        {
            WCHAR base[MAX_PATH];
            WCHAR *dot;
            lstrcpynW(base, exeLow, MAX_PATH);
            dot = wcsrchr(base, L'.');
            if (dot) { *dot = 0; if (lstrcmpW(base, k) == 0) return TRUE; }
        }
        return FALSE;
    }
    return wcsstr(exeLow, key) != NULL;
}

static void ScanAgents()
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    int i, k;

    for (i = 0; i < g_agentCount; ++i) g_agents[i].running = FALSE;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            WCHAR low[MAX_PATH];
            lstrcpynW(low, pe.szExeFile, MAX_PATH);
            CharLowerBuffW(low, (DWORD)lstrlenW(low));
            for (i = 0; i < g_agentCount; ++i) {
                for (k = 0; k < g_agents[i].nkeys; ++k) {
                    if (AgentKeyMatch(low, g_agents[i].keys[k])) {
                        g_agents[i].running = TRUE;
                        break;
                    }
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    /* 组合变化时才写一行日志，便于排查“为什么某个没亮” */
    {
        DWORD bits = 0;
        WCHAR buf[512] = L"";
        for (i = 0; i < g_agentCount && i < 32; ++i) {
            if (!g_agents[i].running) continue;
            bits |= (1u << i);
            if (buf[0]) lstrcatW(buf, L", ");
            lstrcatW(buf, g_agents[i].name);
        }
        if (bits != g_agentBits) {
            g_agentBits = bits;
            LogMsg(L"agents: running = [%ls]", buf);
        }
    }
}

/* ---------------------------------------------------------------- 保护开关 */
static void MarkDirty();
static void UpdateTrayTip();
static void StopProtection();

static void HideToTray()
{
    KillTimer(g_hwnd, TIMER_UI);
    KillTimer(g_hwnd, TIMER_SCAN);
    g_hoverId = B_NONE;
    g_downId = B_NONE;
    ShowWindow(g_hwnd, SW_HIDE);
}

static void ShowMain()
{
    if (g_blackOn) BlackShow(FALSE);
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    ScanAgents();
    SetTimer(g_hwnd, TIMER_UI, 16, NULL);
    SetTimer(g_hwnd, TIMER_SCAN, 3000, NULL);
    MarkDirty();
}

static void StartProtection(int mode)
{
    if (g_mode != 0) return;

    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_guardReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stopEvent || !g_guardReady) {
        LogMsg(L"start: CreateEvent failed");
        if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = NULL; }
        if (g_guardReady) { CloseHandle(g_guardReady); g_guardReady = NULL; }
        return;
    }

    InterlockedExchange(&g_guardOk, 0);
    g_guardThread = CreateThread(NULL, 0, GuardThread, NULL, 0, NULL);
    if (!g_guardThread) {
        CloseHandle(g_stopEvent); g_stopEvent = NULL;
        CloseHandle(g_guardReady); g_guardReady = NULL;
        LogMsg(L"start: CreateThread failed");
        return;
    }

    /* 等守护线程真正拿到电源请求，再盖黑屏，避免“先黑后请求”的窗口期 */
    WaitForSingleObject(g_guardReady, 3000);
    CloseHandle(g_guardReady);
    g_guardReady = NULL;

    if (!g_guardOk) {
        LogMsg(L"start: guard request not acquired, abort");
        SetEvent(g_stopEvent);
        WaitForSingleObject(g_guardThread, 3000);
        CloseHandle(g_guardThread); g_guardThread = NULL;
        CloseHandle(g_stopEvent); g_stopEvent = NULL;
        MessageBoxW(g_hwnd,
            L"系统没有接受防睡电源请求，守护未启动。\n\n"
            L"请确认没有其他程序强制睡眠，然后重试。",
            APP_NAME, MB_ICONWARNING);
        return;
    }

    InterlockedExchange(&g_mode, mode);
    g_startTick = GetTickCount64();
    g_protectSec = 0;
    SetTimer(g_hwnd, TIMER_CLOCK, 1000, NULL);

    LogMsg(L"start: protection ON (mode=%d, reblack=%d)", mode, g_reblackSec);

    UpdateTrayTip();
    HideToTray();

    if (mode == 1) {
        RegisterRawKeyboard();
        BlackShow(TRUE);
        if (!g_blackOn) {
            LogMsg(L"start: black overlay failed, abort");
            StopProtection();
            MessageBoxW(g_hwnd,
                L"无法创建黑屏遮罩窗口，守护未启动。",
                APP_NAME, MB_ICONERROR);
            return;
        }
    }
    MarkDirty();
}

static void StopProtection()
{
    if (g_mode == 0) {
        if (g_blackOn) BlackShow(FALSE);
        return;
    }

    KillTimer(g_hwnd, TIMER_CLOCK);
    BlackShow(FALSE);
    UnregisterRawKeyboard();
    g_peekUntil = 0;

    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_guardThread) {
        WaitForSingleObject(g_guardThread, 3000);
        CloseHandle(g_guardThread);
        g_guardThread = NULL;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
    }
    InterlockedExchange(&g_mode, 0);
    g_protectSec = 0;
    LogMsg(L"stop: protection OFF");

    ShowMain();
    UpdateTrayTip();
}

static void ToggleProtection()
{
    if (g_mode == 0) StartProtection(g_prefMode == 2 ? 2 : 1);
    else             StopProtection();
}

/* 截屏型 agent 用的临时亮屏：撤掉遮罩 + 恢复背光 N 秒，到点自动再黑。
 * 触发方式：AgentKeepAwake.exe --peek [秒]（第二个实例只发消息，不改电源状态） */
static void StartPeek(int sec)
{
    if (sec < 1) sec = g_peekSec;
    if (sec > 60) sec = 60;

    if (g_mode != 1) {
        LogMsg(L"peek: ignored (mode=%d, 只有黑屏守护需要亮屏)", (int)g_mode);
        return;
    }

    g_peekUntil = GetTickCount() + (DWORD)sec * 1000;
    BlackShow(FALSE);
    MarkDirty();
    LogMsg(L"peek: 亮屏 %d 秒（截屏窗口）", sec);
}

/* ---------------------------------------------------------------- 托盘 */
static void AddTrayIcon()
{
    int sx = GetSystemMetrics(SM_CXSMICON), sy = GetSystemMetrics(SM_CYSMICON);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, sx, sy, 0);
    lstrcpynW(g_nid.szTip, L"AgentKeepAwake · 未开启", ARRAYSIZE(g_nid.szTip));

    g_trayOk = Shell_NotifyIconW(NIM_ADD, &g_nid);
    if (g_trayOk) {
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    }
    LogMsg(L"tray: add ok=%d", g_trayOk);
}

static void UpdateTrayTip()
{
    WCHAR tip[128];
    HICON ic;

    if (!g_trayOk) return;

    if (g_mode == 0)
        lstrcpynW(tip, L"AgentKeepAwake · 未开启", ARRAYSIZE(tip));
    else if (g_mode == 1)
        lstrcpynW(tip, L"AgentKeepAwake · 黑屏守护中", ARRAYSIZE(tip));
    else
        lstrcpynW(tip, L"AgentKeepAwake · 常亮防睡中", ARRAYSIZE(tip));

    ic = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(g_mode ? 2 : 1), IMAGE_ICON,
                           GetSystemMetrics(SM_CXSMICON),
                           GetSystemMetrics(SM_CYSMICON), 0);
    if (ic) g_nid.hIcon = ic;
    lstrcpynW(g_nid.szTip, tip, ARRAYSIZE(g_nid.szTip));
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (ic) DestroyIcon(ic);
    g_nid.hIcon = NULL;
}

static void ShowTrayMenu()
{
    HMENU m = CreatePopupMenu();
    POINT pt;

    if (g_mode == 0)
        AppendMenuW(m, MF_STRING, 2001,
                    g_prefMode == 2 ? L"开始常亮防睡" : L"开始黑屏守护");
    else
        AppendMenuW(m, MF_STRING, 2001, L"停止守护");
    AppendMenuW(m, MF_STRING | (g_mode == 2 ? MF_GRAYED : 0), 2002, L"立即黑屏");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 2003, L"显示主窗口");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 2005, L"退出程序");

    SetMenuDefaultItem(m, 2003, FALSE);
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
}

/* ---------------------------------------------------------------- 绘制资源 */
static void BuildLayout()
{
    float pw = (float)SCALE(PANEL_W), ph = (float)SCALE(PANEL_H);
    float pad = (float)SCALE(PAD);
    float w = pw - pad * 2.0f;

    rPanel = RF(0, 0, pw, ph);

    rHero     = RF(pad, (float)SCALE(62),  w, (float)SCALE(240));
    rMain     = RF(rHero.X + SCALE(18), rHero.Y + SCALE(152),
                   rHero.Width - SCALE(36), (float)SCALE(52));
    rAgents   = RF(pad, (float)SCALE(314), w, (float)SCALE(122));
    rSettings = RF(pad, (float)SCALE(446), w, (float)SCALE(164));

    rBtnMin   = RF(pw - pad - SCALE(26) * 2 - SCALE(6), (float)SCALE(16),
                   (float)SCALE(26), (float)SCALE(26));
    rBtnClose = RF(pw - pad - SCALE(26), (float)SCALE(16),
                   (float)SCALE(26), (float)SCALE(26));
    rBtnExit  = RF(pw - pad - SCALE(64), (float)SCALE(618),
                   (float)SCALE(64), (float)SCALE(20));

    /* 设置卡三行 */
    {
        float x = rSettings.X + SCALE(476 - 16) - SCALE(202);
        float y = rSettings.Y + SCALE(12);
        for (int i = 0; i < 4; ++i)
            rRb[i] = RF(x + i * (SCALE(46) + SCALE(6)), y, (float)SCALE(46), (float)SCALE(26));

        x = rSettings.X + SCALE(476 - 16) - SCALE(200);
        y = rSettings.Y + SCALE(58);
        rMd[0] = RF(x, y, (float)SCALE(108), (float)SCALE(26));
        rMd[1] = RF(x + SCALE(108) + SCALE(8), y, (float)SCALE(84), (float)SCALE(26));

        x = rSettings.X + SCALE(476 - 16) - SCALE(176);
        y = rSettings.Y + SCALE(104);
        rBr[0] = RF(x, y, (float)SCALE(84), (float)SCALE(26));
        rBr[1] = RF(x + SCALE(84) + SCALE(8), y, (float)SCALE(84), (float)SCALE(26));

        rSettingsRow1 = RF(rSettings.X + SCALE(16), rSettings.Y + SCALE(12),
                           (float)SCALE(220), (float)SCALE(26));
        rSettingsRow2 = RF(rSettings.X + SCALE(16), rSettings.Y + SCALE(58),
                           (float)SCALE(220), (float)SCALE(26));
        rSettingsRow3 = RF(rSettings.X + SCALE(16), rSettings.Y + SCALE(104),
                           (float)SCALE(220), (float)SCALE(26));
        rPeekHint = RF(rSettings.X + SCALE(16), rSettings.Y + SCALE(140),
                       rSettings.Width - SCALE(32), (float)SCALE(14));
    }

    /* Agent 卡片 */
    rAgentHint = RF(rAgents.X + rAgents.Width - SCALE(16) - SCALE(240),
                    rAgents.Y + SCALE(14), (float)SCALE(240), (float)SCALE(16));
    rAgentsFoot = RF(rAgents.X + SCALE(16), rAgents.Y + SCALE(84),
                     rAgents.Width - SCALE(32), (float)SCALE(16));
    {
        float x = rAgents.X + SCALE(16);
        for (int i = 0; i < AGENT_CHIPS; ++i) {
            int row = i / 4, col = i % 4;
            rChip[i] = RF(x + col * (SCALE(106) + SCALE(6)),
                          rAgents.Y + SCALE(44) + row * (SCALE(34) + SCALE(6)),
                          (float)SCALE(106), (float)SCALE(34));
        }
    }
}

static void InitGfx()
{
    fName      = MakeFont((float)SCALE(13),   FontStyleBold);
    fSub       = MakeFont((float)SCALE(9),    0);
    fStatus    = MakeFont((float)SCALE(19),   FontStyleBold);
    fStatusSub = MakeFont((float)SCALE(10),   0);
    fMetricL   = MakeFont((float)SCALE(8.5f), 0);
    fMetricV   = MakeFont((float)SCALE(15),   FontStyleBold);
    fBtn       = MakeFont((float)SCALE(12.5f),FontStyleBold);
    fPill      = MakeFont((float)SCALE(9.5f), 0);
    fPillOn    = MakeFont((float)SCALE(9.5f), FontStyleBold);
    fChip      = MakeFont((float)SCALE(9.5f), 0);
    fCardTitle = MakeFont((float)SCALE(10),   FontStyleBold);
    fFoot      = MakeFont((float)SCALE(8.5f), 0);
    fHint      = MakeFont((float)SCALE(8.5f), 0);

    BuildLayout();
}

/* 简易箱式模糊（在小图上做，代价极低） */
static void BoxBlur(Bitmap *bmp, int r, int passes)
{
    Rect rc(0, 0, bmp->GetWidth(), bmp->GetHeight());
    BitmapData bd;
    int w, h, stride, pass, x, y, k, n;
    BYTE *base, *tmp, *dst;

    if (bmp->LockBits(&rc, ImageLockModeRead | ImageLockModeWrite,
                      PixelFormat32bppPARGB, &bd) != Ok) return;
    w = bmp->GetWidth();
    h = bmp->GetHeight();
    stride = bd.Stride;
    base = (BYTE *)bd.Scan0;
    tmp = (BYTE *)malloc((size_t)stride * h);

    if (tmp) {
        for (pass = 0; pass < passes; ++pass) {
            /* 横向 */
            for (y = 0; y < h; ++y) {
                for (x = 0; x < w; ++x) {
                    int sb = 0, sg = 0, sr = 0, sa = 0;
                    n = 0;
                    for (k = -r; k <= r; ++k) {
                        int xx = x + k;
                        BYTE *p;
                        if (xx < 0) xx = 0;
                        if (xx >= w) xx = w - 1;
                        p = base + y * stride + xx * 4;
                        sb += p[0]; sg += p[1]; sr += p[2]; sa += p[3];
                        n++;
                    }
                    dst = tmp + y * stride + x * 4;
                    dst[0] = (BYTE)(sb / n);
                    dst[1] = (BYTE)(sg / n);
                    dst[2] = (BYTE)(sr / n);
                    dst[3] = (BYTE)(sa / n);
                }
            }
            memcpy(base, tmp, (size_t)stride * h);

            /* 纵向 */
            for (x = 0; x < w; ++x) {
                for (y = 0; y < h; ++y) {
                    int sb = 0, sg = 0, sr = 0, sa = 0;
                    n = 0;
                    for (k = -r; k <= r; ++k) {
                        int yy = y + k;
                        BYTE *p;
                        if (yy < 0) yy = 0;
                        if (yy >= h) yy = h - 1;
                        p = base + yy * stride + x * 4;
                        sb += p[0]; sg += p[1]; sr += p[2]; sa += p[3];
                        n++;
                    }
                    dst = tmp + y * stride + x * 4;
                    dst[0] = (BYTE)(sb / n);
                    dst[1] = (BYTE)(sg / n);
                    dst[2] = (BYTE)(sr / n);
                    dst[3] = (BYTE)(sa / n);
                }
            }
            memcpy(base, tmp, (size_t)stride * h);
        }
        free(tmp);
    }
    bmp->UnlockBits(&bd);
}

/* 采样桌面（只用于玻璃底：重度模糊后仅保留色块与明暗） */
static void BuildBackdrop()
{
    HDC sdc, mdc;
    BITMAPINFO bmi;
    HBITMAP hb, old;
    void *bits = NULL;
    RECT wr;
    int w, h, i;
    BYTE *px;

    if (g_backdrop) { delete g_backdrop; g_backdrop = NULL; }
    if (g_glass)    { delete g_glass;    g_glass = NULL; }

    g_backdrop = new Bitmap(g_cw, g_ch, PixelFormat32bppPARGB);
    {
        Graphics g(g_backdrop);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.Clear(Color(255, 10, 13, 20));
    }

    GetWindowRect(g_hwnd, &wr);
    w = g_cw; h = g_ch;

    sdc = GetDC(NULL);
    mdc = CreateCompatibleDC(sdc);
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    hb = CreateDIBSection(mdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);

    if (hb && bits) {
        old = (HBITMAP)SelectObject(mdc, hb);
        BitBlt(mdc, 0, 0, w, h, sdc, wr.left, wr.top, SRCCOPY | CAPTUREBLT);
        SelectObject(mdc, old);

        /* GDI 不写 alpha 通道：必须手动置 255，否则 GDI+ 视为全透明 */
        px = (BYTE *)bits;
        for (i = 0; i < w * h; ++i) px[i * 4 + 3] = 255;

        {
            Bitmap shot(w, h, w * 4, PixelFormat32bppPARGB, (BYTE *)bits);
            Graphics g(g_backdrop);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.DrawImage(&shot, RectF(0, 0, (REAL)w, (REAL)h));
        }
        DeleteObject(hb);
    }
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);

    /* 压暗 + 冷调 */
    {
        Graphics g(g_backdrop);
        LinearGradientBrush tint(RectF(0, 0, (REAL)w, (REAL)h),
                                 Color(196, 9, 12, 18), Color(232, 3, 5, 10),
                                 LinearGradientModeVertical);
        g.FillRectangle(&tint, RectF(0, 0, (REAL)w, (REAL)h));

        /* 三团柔光，给玻璃提供“有东西可折射”的颜色 */
        struct { float x, y, r; Color c; } blobs[3] = {
            { 0.16f, 0.12f, 0.46f, Color(70,  64, 226, 197) },   /* 薄荷 */
            { 0.92f, 0.42f, 0.42f, Color(64, 120, 110, 255) },   /* 蓝紫 */
            { 0.30f, 0.96f, 0.52f, Color(52,  80, 150, 255) }    /* 深蓝 */
        };
        for (i = 0; i < 3; ++i) {
            GraphicsPath ep;
            float cx = blobs[i].x * w, cy = blobs[i].y * h, rr = blobs[i].r * w;
            ep.AddEllipse(cx - rr, cy - rr, rr * 2, rr * 2);
            PathGradientBrush pg(&ep);
            pg.SetCenterColor(blobs[i].c);
            Color edge(0, blobs[i].c.GetR(), blobs[i].c.GetG(), blobs[i].c.GetB());
            int n = 1;
            pg.SetSurroundColors(&edge, &n);
            g.FillPath(&pg, &ep);
        }

        /* 四角压暗，突出中心玻璃 */
        GraphicsPath all;
        all.AddRectangle(RectF(0, 0, (REAL)w, (REAL)h));
        PathGradientBrush vig(&all);
        vig.SetCenterPoint(PointF(w * 0.5f, h * 0.38f));
        vig.SetCenterColor(Color(0, 0, 0, 0));
        Color edges[1] = { Color(120, 0, 0, 0) };
        int n = 1;
        vig.SetSurroundColors(edges, &n);
        g.FillPath(&vig, &all);
    }

    /* 玻璃取样图：重度模糊版（降采样再升采样 = 廉价高质量模糊） */
    {
        int sw = g_cw / 22 + 1, sh = g_ch / 22 + 1;
        Bitmap small(sw, sh, PixelFormat32bppPARGB);
        {
            Graphics gs(&small);
            gs.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            gs.DrawImage(g_backdrop, RectF(0, 0, (REAL)sw, (REAL)sh));
        }
        BoxBlur(&small, 2, 3);
        g_glass = new Bitmap(g_cw, g_ch, PixelFormat32bppPARGB);
        Graphics gg(g_glass);
        gg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        gg.DrawImage(&small, RectF(0, 0, (REAL)g_cw, (REAL)g_ch));
        SolidBrush milk(Color(14, 255, 255, 255));
        gg.FillRectangle(&milk, RectF(0, 0, (REAL)g_cw, (REAL)g_ch));
    }
}

static void InitCanvas()
{
    BITMAPINFO bmi;
    int sh = SCALE(SHADOW_M);

    g_cw = SCALE(PANEL_W) + sh * 2;
    g_ch = SCALE(PANEL_H) + sh * 2;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_cw;
    bmi.bmiHeader.biHeight = -g_ch;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_memDC = CreateCompatibleDC(NULL);
    g_memBmp = CreateDIBSection(g_memDC, &bmi, DIB_RGB_COLORS, &g_memBits, NULL, 0);
    SelectObject(g_memDC, g_memBmp);

    g_canvas = new Bitmap(g_cw, g_ch, g_cw * 4, PixelFormat32bppPARGB, (BYTE *)g_memBits);
}

/* 面板投影：多层叠加模拟高斯阴影 */
static void DrawPanelShadow(Graphics &g)
{
    for (int i = 7; i >= 1; --i) {
        GraphicsPath p;
        float e = (float)SCALE(2) * i;
        AddRound(p, RF(rPanel.X - e * 0.55f, rPanel.Y + (float)SCALE(10) - e * 0.35f,
                       rPanel.Width + e * 1.1f, rPanel.Height + e * 0.9f),
                 (float)SCALE(26) + e * 0.55f);
        SolidBrush b(Color(7, 0, 0, 0));
        g.FillPath(&b, &p);
    }
}

/* 玻璃卡片：模糊底 + 提亮 + 顶部高光 + 细描边 */
static void GlassCard(Graphics &g, const RectF &r, float rad, int tint = 20)
{
    int sh = SCALE(SHADOW_M);
    GraphicsPath path;
    AddRound(path, r, rad);

    GraphicsState st = g.Save();
    g.SetClip(&path);
    g.DrawImage(g_glass, RF(r.X, r.Y, r.Width, r.Height),
                (REAL)(sh + r.X), (REAL)(sh + r.Y), (REAL)r.Width, (REAL)r.Height,
                UnitPixel);
    SolidBrush milk(Color(tint, 255, 255, 255));
    g.FillPath(&milk, &path);

    LinearGradientBrush gloss(RectF(r.X, r.Y, r.Width, r.Height * 0.62f),
                              Color(16, 255, 255, 255), Color(0, 255, 255, 255),
                              LinearGradientModeVertical);
    g.FillPath(&gloss, &path);
    g.Restore(st);

    LinearGradientBrush borderC(RectF(r.X, r.Y, r.Width, r.Height),
                                Color(46, 255, 255, 255), Color(16, 255, 255, 255),
                                LinearGradientModeVertical);
    Pen pen(&borderC, 1.0f);
    g.DrawPath(&pen, &path);
}

static void DrawOrb(Graphics &g, float cx, float cy, float rad, BOOL active)
{
    float pulse = active ? (0.5f + 0.5f * sinf(g_pulse)) : 0.0f;

    for (int i = 4; i >= 1; --i) {
        float rr = rad + (float)SCALE(6) * i + pulse * SCALE(3);
        int a = active ? (int)(18 * (1.0f - i / 5.0f) + 6)
                       : (int)(6 * (1.0f - i / 5.0f));
        SolidBrush b(active ? Color(a, 94, 226, 197) : Color(a, 150, 160, 175));
        g.FillEllipse(&b, cx - rr, cy - rr, rr * 2, rr * 2);
    }

    GraphicsPath circ;
    circ.AddEllipse(cx - rad, cy - rad, rad * 2, rad * 2);
    PathGradientBrush pg(&circ);
    pg.SetCenterPoint(PointF(cx - rad * 0.3f, cy - rad * 0.35f));
    pg.SetCenterColor(active ? Color(255, 26, 74, 68) : Color(255, 30, 38, 49));
    Color edge = active ? Color(255, 7, 20, 24) : Color(255, 15, 19, 26);
    Color edges[1] = { edge };
    int n = 1;
    pg.SetSurroundColors(edges, &n);
    g.FillPath(&pg, &circ);

    Pen rim(active ? Color(120, 255, 255, 255) : Color(54, 255, 255, 255), 1.1f);
    g.DrawEllipse(&rim, cx - rad + 1, cy - rad + 1, (rad - 1) * 2, (rad - 1) * 2);

    /* 电源图形 */
    Pen gp(active ? Color(255, 236, 255, 250) : Color(190, 210, 220, 232),
           (REAL)SCALE(2.4));
    gp.SetStartCap(LineCapRound);
    gp.SetEndCap(LineCapRound);
    float a = (float)SCALE(9);
    g.DrawArc(&gp, cx - a, cy - a, a * 2, a * 2, 290, 320);
    g.DrawLine(&gp, cx, cy - (float)SCALE(13), cx, cy - (float)SCALE(2));
}

static void DrawPill(Graphics &g, const RectF &r, BOOL on, float hv,
                     const WCHAR *label)
{
    GraphicsPath p;
    AddRound(p, r, r.Height * 0.5f);

    if (on) {
        LinearGradientBrush b(r, Color(255, 104, 230, 202),
                              Color(255, 74, 196, 232), LinearGradientModeVertical);
        g.FillPath(&b, &p);
        Pen pen(Color(120, 255, 255, 255), 1.0f);
        g.DrawPath(&pen, &p);
        Txt(g, label, fPillOn, r, C_DARKTXT, 1);
    } else {
        SolidBrush b(Color((int)(14 + 20 * hv), 255, 255, 255));
        g.FillPath(&b, &p);
        Pen pen(Color((int)(26 + 26 * hv), 255, 255, 255), 1.0f);
        g.DrawPath(&pen, &p);
        Txt(g, label, fPill, r, C_TXT2, 1);
    }
}

static void DrawMainButton(Graphics &g, BOOL active)
{
    float pr = g_press[B_MAIN], hv = g_hover[B_MAIN];
    RectF r = ScaleAboutCenter(rMain, 1.0f - 0.012f * pr);
    if (pr > 0.01f) r.Y += SCALE(1) * pr;

    /* 外发光 */
    for (int i = 3; i >= 1; --i) {
        GraphicsPath gp;
        float e = (float)SCALE(5) * i;
        AddRound(gp, RF(r.X - e, r.Y - e * 0.4f, r.Width + e * 2, r.Height + e * 1.6f),
                 r.Height * 0.5f + e);
        int a = active ? (int)(13 * (1.0f - i / 4.0f) + 3)
                       : (int)(16 * (1.0f - i / 4.0f) + 4);
        SolidBrush b(active ? Color(a, 255, 110, 118) : Color(a, 80, 220, 190));
        g.FillPath(&b, &gp);
    }

    GraphicsPath p;
    AddRound(p, r, r.Height * 0.5f);

    if (active) {
        LinearGradientBrush b(r, Color(242, 255, 128, 134),
                              Color(238, 226, 72, 94), LinearGradientModeVertical);
        g.FillPath(&b, &p);
    } else {
        LinearGradientBrush b(r, Color(255, 102, 232, 205),
                              Color(255, 58, 124, 246), LinearGradientModeVertical);
        g.FillPath(&b, &p);
    }

    /* 顶部高光 = 液态玻璃的灵魂 */
    {
        GraphicsState st = g.Save();
        g.SetClip(&p);
        LinearGradientBrush sheen(RectF(r.X, r.Y, r.Width, r.Height * 0.5f),
                                  Color(64, 255, 255, 255), Color(0, 255, 255, 255),
                                  LinearGradientModeVertical);
        g.FillRectangle(&sheen, RectF(r.X, r.Y, r.Width, r.Height));
        g.Restore(st);
    }

    Pen pen(Color(150, 255, 255, 255), 1.0f);
    g.DrawPath(&pen, &p);

    if (hv > 0.01f) {
        SolidBrush hb(Color((int)(26 * hv), 255, 255, 255));
        g.FillPath(&hb, &p);
    }

    Txt(g, active ? L"停止守护" : (g_prefMode == 2 ? L"开始常亮防睡" : L"开始黑屏守护"),
        fBtn, r, active ? Color(255, 255, 250, 250) : C_DARKTXT, 1);
}

static void DrawRoundIconButton(Graphics &g, const RectF &r, int id,
                                const WCHAR *glyph, BOOL danger)
{
    float hv = g_hover[id];
    GraphicsPath p;
    AddRound(p, r, r.Height * 0.34f);
    SolidBrush b(Color((int)(10 + 26 * hv),
                       danger ? 255 : 255, danger ? 120 : 255, danger ? 120 : 255));
    g.FillPath(&b, &p);
    Pen pen(Color((int)(18 + 28 * hv), 255, 255, 255), 1.0f);
    g.DrawPath(&pen, &p);
    Txt(g, glyph, fPill, r, hv > 0.5f ? C_TXT : C_TXT2, 1);
}

static void FormatDuration(WCHAR *out, int cap, int sec)
{
    int h = sec / 3600, m = (sec / 60) % 60, s = sec % 60;
    if (h > 99) h = 99;
    wsprintfW(out, L"%02d:%02d:%02d", h, m, s);
}

static void DrawMetric(Graphics &g, float x, float y, const WCHAR *label,
                       const WCHAR *value, const Color &vc)
{
    Txt(g, label, fMetricL, RF(x, y, SCALE(120), SCALE(12)), C_TXT4, 0);
    Txt(g, value, fMetricV, RF(x - SCALE(2), y + SCALE(14), SCALE(140), SCALE(22)), vc, 0);
}

/* ---------------------------------------------------------------- 主绘制 */
static void PaintFrame()
{
    if (!g_canvas || !g_backdrop) return;

    Graphics g(g_canvas);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.Clear(Color(0, 0, 0, 0));

    int sh = SCALE(SHADOW_M);
    g.TranslateTransform((REAL)sh, (REAL)sh);

    DrawPanelShadow(g);

    /* 面板底：桌面毛玻璃 */
    GraphicsPath panelPath;
    AddRound(panelPath, rPanel, (float)SCALE(26));
    {
        GraphicsState st = g.Save();
        g.SetClip(&panelPath);
        g.DrawImage(g_backdrop, RF(0, 0, rPanel.Width, rPanel.Height),
                    (REAL)sh, (REAL)sh, (REAL)rPanel.Width, (REAL)rPanel.Height,
                    UnitPixel);
        g.Restore(st);
    }

    /* 外圈描边 */
    {
        LinearGradientBrush bc(RectF(0, 0, rPanel.Width, rPanel.Height),
                               Color(64, 255, 255, 255), Color(22, 255, 255, 255),
                               LinearGradientModeVertical);
        Pen pen(&bc, 1.0f);
        g.DrawPath(&pen, &panelPath);
    }

    BOOL active = (g_mode != 0);
    WCHAR buf[160], buf2[160];

    /* ---------- 顶栏 ---------- */
    {
        /* 应用图标：圆角玻璃托底 + 闪电剪影 */
        RectF ir = RF((float)SCALE(PAD), (float)SCALE(17),
                      (float)SCALE(24), (float)SCALE(24));
        GraphicsPath ip;
        AddRound(ip, ir, (float)SCALE(7));
        LinearGradientBrush ib(ir, Color(255, 96, 226, 197),
                               Color(255, 80, 130, 255), LinearGradientModeVertical);
        g.FillPath(&ib, &ip);
        {
            SolidBrush bolt(Color(255, 6, 26, 30));
            GraphicsPath bp;
            PointF pts[6] = {
                PointF(ir.X + ir.Width * 0.56f, ir.Y + ir.Height * 0.16f),
                PointF(ir.X + ir.Width * 0.26f, ir.Y + ir.Height * 0.56f),
                PointF(ir.X + ir.Width * 0.46f, ir.Y + ir.Height * 0.56f),
                PointF(ir.X + ir.Width * 0.42f, ir.Y + ir.Height * 0.86f),
                PointF(ir.X + ir.Width * 0.76f, ir.Y + ir.Height * 0.44f),
                PointF(ir.X + ir.Width * 0.54f, ir.Y + ir.Height * 0.44f)
            };
            bp.AddPolygon(pts, 6);
            g.FillPath(&bolt, &bp);
        }

        Txt(g, L"AgentKeepAwake", fName,
            RF((float)SCALE(54), (float)SCALE(14), (float)SCALE(300), (float)SCALE(18)),
            C_TXT, 0);
        Txt(g, L"黑屏守护 · 让 agent 一直跑，让屏幕先休息", fSub,
            RF((float)SCALE(54), (float)SCALE(33), (float)SCALE(330), (float)SCALE(14)),
            C_TXT3, 0);

        DrawRoundIconButton(g, rBtnMin, B_MIN, L"—", FALSE);
        DrawRoundIconButton(g, rBtnClose, B_CLOSE, L"✕", TRUE);
    }

    /* ---------- 英雄卡 ---------- */
    GlassCard(g, rHero, (float)SCALE(26), 28);
    {
        float cx = rHero.X + (float)SCALE(62);
        float cy = rHero.Y + (float)SCALE(62);
        DrawOrb(g, cx, cy, (float)SCALE(33), active);

        const WCHAR *title;
        if (g_mode == 0)              title = L"守护未开启";
        else if (g_mode == 2)         title = L"常亮防睡中";
        else if (g_blackOn)           title = L"屏幕已黑 · 任务照常跑";
        else                          title = L"守护中 · 屏幕亮着";

        Txt(g, title, fStatus,
            RF(rHero.X + SCALE(108), rHero.Y + SCALE(36),
               rHero.Width - SCALE(128), SCALE(26)), C_TXT, 0);

        if (g_mode == 0)
            lstrcpynW(buf, L"点一下，屏幕盖上一层纯黑，任务继续跑", ARRAYSIZE(buf));
        else if (g_mode == 2)
            lstrcpynW(buf, L"系统与屏幕保持唤醒 · 不会再自动熄屏", ARRAYSIZE(buf));
        else if (g_blackOn)
            lstrcpynW(buf, L"显示器只是被纯黑遮住 · agent 不会被挂起", ARRAYSIZE(buf));
        else {
            if (g_reblackSec > 0)
                wsprintfW(buf, L"无操作 %d 秒后自动黑屏", g_reblackSec);
            else
                lstrcpynW(buf, L"自动黑屏已关闭，由你手动控制", ARRAYSIZE(buf));
        }
        Txt(g, buf, fStatusSub,
            RF(rHero.X + SCALE(109), rHero.Y + SCALE(66),
               rHero.Width - SCALE(130), SCALE(16)), C_TXT3, 0);

        /* 三个指标 */
        {
            float my = rHero.Y + SCALE(108);
            WCHAR dur[32];

            if (g_mode == 0) lstrcpynW(dur, L"--:--:--", 32);
            else             FormatDuration(dur, 32, g_protectSec);

            DrawMetric(g, rHero.X + SCALE(22), my, L"已守护", dur,
                       g_mode ? C_TXT : C_TXT2);
            DrawMetric(g, rHero.X + SCALE(172), my, L"黑屏",
                       g_mode == 0 ? L"—" :
                       g_mode == 2 ? L"未启用" :
                       g_blackOn ? L"已开启" : L"未开启",
                       (g_mode == 1 && g_blackOn) ? C_MINT : C_TXT2);
            DrawMetric(g, rHero.X + SCALE(322), my, L"系统",
                       g_mode ? L"不休眠" : L"正常",
                       g_mode ? C_MINT : C_TXT3);

            Pen sep(Color(22, 255, 255, 255), 1.0f);
            g.DrawLine(&sep, rHero.X + SCALE(158), my + SCALE(2),
                       rHero.X + SCALE(158), my + SCALE(34));
            g.DrawLine(&sep, rHero.X + SCALE(308), my + SCALE(2),
                       rHero.X + SCALE(308), my + SCALE(34));
        }

        DrawMainButton(g, g_mode != 0);

        if (g_mode == 0)
            lstrcpynW(buf, g_prefMode == 2 ? L"不黑屏，适合需要看屏幕的场景"
                                           : L"点一下立即黑屏 · 动鼠标/按键就恢复",
                       ARRAYSIZE(buf));
        else if (g_mode == 2)
            lstrcpynW(buf, L"屏幕保持常亮 · 由本程序阻止系统睡眠", ARRAYSIZE(buf));
        else
            lstrcpynW(buf, L"只盖一层纯黑窗口 · 不关显示器电源 · Ctrl+Alt+K 停止", ARRAYSIZE(buf));

        Txt(g, buf, fHint,
            RF(rHero.X + SCALE(18), rHero.Y + SCALE(212),
               rHero.Width - SCALE(36), SCALE(16)), C_TXT3, 1);
    }

    /* ---------- Agent 卡 ---------- */
    GlassCard(g, rAgents, (float)SCALE(20), 26);
    {
        int order[AGENT_MAX], n = 0, i, shown, runN = 0, overflow = 0;

        Txt(g, L"正在运行的 Agent", fCardTitle,
            RF(rAgents.X + SCALE(16), rAgents.Y + SCALE(14),
               SCALE(180), SCALE(16)), C_TXT2, 0);
        {
            WCHAR hint[64];
            wsprintfW(hint, L"已配置 %d 个 · agents.ini 可加", g_agentCount);
            Txt(g, hint, fHint, rAgentHint, C_TXT4, 2);
        }

        /* 运行中的全部列出（最多 8 个）；超出时最后一位显示 +N */
        for (i = 0; i < g_agentCount; ++i)
            if (g_agents[i].running) order[runN++] = i;
        if (runN > AGENT_CHIPS) {
            n = AGENT_CHIPS - 1;
            overflow = runN - n;
        } else {
            n = runN;
            for (i = 0; i < g_agentCount && n < 4; ++i)
                if (!g_agents[i].running) order[n++] = i;
        }
        shown = runN;

        for (i = 0; i < n; ++i) {
            AgentInfo *ag = &g_agents[order[i]];
            RectF cr = rChip[i];
            GraphicsPath cp;
            AddRound(cp, cr, (float)SCALE(11));
            SolidBrush cb(Color(10, 255, 255, 255));
            g.FillPath(&cb, &cp);
            Pen cpen(Color(22, 255, 255, 255), 1.0f);
            g.DrawPath(&cpen, &cp);

            float dx = cr.X + SCALE(13), dy = cr.Y + cr.Height * 0.5f;
            if (ag->running) {
                SolidBrush glow(Color(46, 94, 226, 197));
                g.FillEllipse(&glow, dx - (REAL)SCALE(5), dy - (REAL)SCALE(5),
                              (REAL)SCALE(10), (REAL)SCALE(10));
            }
            SolidBrush dot(ag->running ? C_MINT : C_GRAYDOT);
            g.FillEllipse(&dot, dx - (REAL)SCALE(2.6f), dy - (REAL)SCALE(2.6f),
                          (REAL)SCALE(5.2f), (REAL)SCALE(5.2f));

            Txt(g, ag->name, fChip,
                RF(cr.X + SCALE(24), cr.Y, cr.Width - SCALE(30), cr.Height),
                ag->running ? C_TXT : C_TXT4, 0);
        }

        if (overflow > 0) {
            RectF cr = rChip[n];
            GraphicsPath cp;
            AddRound(cp, cr, (float)SCALE(11));
            SolidBrush cb(Color(10, 255, 255, 255));
            g.FillPath(&cb, &cp);
            Pen cpen(Color(22, 255, 255, 255), 1.0f);
            g.DrawPath(&cpen, &cp);

            WCHAR ov[32];
            wsprintfW(ov, L"+%d 个", overflow);
            Txt(g, ov, fChip,
                RF(cr.X + SCALE(13), cr.Y, cr.Width - SCALE(22), cr.Height),
                C_TXT3, 0);
        }

        if (shown <= 4)
            Txt(g, L"黑屏期间它们照常运行：不挂起、不限速、不断网", fHint,
                rAgentsFoot, C_TXT4, 0);
    }

    /* ---------- 设置卡 ---------- */
    GlassCard(g, rSettings, (float)SCALE(20), 26);
    {
        WCHAR hint[128];

        Txt(g, L"操作后自动再黑屏", fCardTitle, rSettingsRow1, C_TXT2, 0);
        Txt(g, L"守护模式", fCardTitle, rSettingsRow2, C_TXT2, 0);
        Txt(g, L"黑屏时背光", fCardTitle, rSettingsRow3, C_TXT2, 0);

        static const WCHAR *labels[4] = { L"关", L"5 秒", L"15 秒", L"30 秒" };
        static const int vals[4] = { 0, 5, 15, 30 };
        for (int i = 0; i < 4; ++i)
            DrawPill(g, rRb[i], g_reblackSec == vals[i], g_hover[B_RB0 + i], labels[i]);

        DrawPill(g, rMd[0], g_prefMode == 1, g_hover[B_MD0], L"黑屏 + 防睡");
        DrawPill(g, rMd[1], g_prefMode == 2, g_hover[B_MD1], L"常亮防睡");

        DrawPill(g, rBr[0], g_blackBright == 0, g_hover[B_BR0], L"跟随系统");
        DrawPill(g, rBr[1], g_blackBright == 1, g_hover[B_BR1], L"压到最低");

        if (g_brightAvail < 0)
            lstrcpynW(hint, L"本机没有可调背光（外接屏/驱动不支持），只用纯黑遮罩", 128);
        else
            wsprintfW(hint, L"截屏型 agent：先运行 AgentKeepAwake.exe --peek %d，亮屏 %d 秒后自动再黑",
                      g_peekSec, g_peekSec);
        Txt(g, hint, fHint, rPeekHint, C_TXT4, 0);
    }

    /* ---------- 页脚 ---------- */
    Txt(g, L"Ctrl+Alt+K 一键开关 · 关闭窗口只收进托盘", fHint,
        RF((float)SCALE(PAD), (float)SCALE(618), (float)SCALE(360), (float)SCALE(20)),
        C_TXT3, 0);
    Txt(g, L"退出程序", fFoot, rBtnExit,
        g_hover[B_EXIT] > 0.4f ? C_ROSE : C_TXT3, 2);

    /* 交给 DWM */
    {
        RECT wr;
        POINT ptDst, ptSrc = { 0, 0 };
        SIZE sz = { g_cw, g_ch };
        BLENDFUNCTION bf;
        HDC sdc;

        GetWindowRect(g_hwnd, &wr);
        ptDst.x = wr.left;
        ptDst.y = wr.top;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;

        sdc = GetDC(NULL);
        UpdateLayeredWindow(g_hwnd, sdc, &ptDst, &sz, g_memDC, &ptSrc, 0, &bf, ULW_ALPHA);
        ReleaseDC(NULL, sdc);
    }
}

static void MarkDirty()
{
    if (IsWindow(g_hwnd) && IsWindowVisible(g_hwnd)) {
        PaintFrame();
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

/* ---------------------------------------------------------------- 命中测试 */
static int HitTest(int x, int y)
{
    PointF p((REAL)(x - SCALE(SHADOW_M)), (REAL)(y - SCALE(SHADOW_M)));
    int i;
    RECT rc;

    for (i = 0; i < 4; ++i)
        if (rRb[i].Contains(p)) return B_RB0 + i;
    for (i = 0; i < 2; ++i)
        if (rMd[i].Contains(p)) return B_MD0 + i;
    for (i = 0; i < 2; ++i)
        if (rBr[i].Contains(p)) return B_BR0 + i;

    if (rMain.Contains(p))  return B_MAIN;
    if (rBtnMin.Contains(p))return B_MIN;
    if (rBtnClose.Contains(p)) return B_CLOSE;
    if (rBtnExit.Contains(p))  return B_EXIT;

    /* 顶栏空白区可拖动 */
    GetClientRect(g_hwnd, &rc);
    if (y < SCALE(58) && x > SCALE(90) && x < SCALE(400)) return -2;
    return B_NONE;
}

static void OnClick(int id)
{
    switch (id) {
    case B_MAIN:
        ToggleProtection();
        break;
    case B_MIN:
    case B_CLOSE:
        HideToTray();
        LogMsg(L"ui: hidden to tray");
        break;
    case B_EXIT:
        g_quit = TRUE;
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        break;
    case B_RB0: g_reblackSec = 0;  SaveSettings(); MarkDirty(); break;
    case B_RB1: g_reblackSec = 5;  SaveSettings(); MarkDirty(); break;
    case B_RB2: g_reblackSec = 15; SaveSettings(); MarkDirty(); break;
    case B_RB3: g_reblackSec = 30; SaveSettings(); MarkDirty(); break;
    case B_MD0: g_prefMode = 1;    SaveSettings(); MarkDirty(); break;
    case B_MD1: g_prefMode = 2;    SaveSettings(); MarkDirty(); break;
    case B_BR0:
        g_blackBright = 0;
        SaveSettings();
        if (g_blackOn) BrightAsk(BRIGHT_RESTORE);   /* 关掉这档：立刻恢复亮度 */
        MarkDirty();
        break;
    case B_BR1:
        g_blackBright = 1;
        SaveSettings();
        if (g_blackOn) BrightAsk(BRIGHT_MIN);
        MarkDirty();
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- 动画循环 */
static void TickAnimate()
{
    if (!IsWindowVisible(g_hwnd)) return;

    BOOL moved = FALSE;
    for (int i = 0; i < B_COUNT; ++i) {
        moved |= Step(g_hover[i], (g_hoverId == i) ? 1.0f : 0.0f, 0.24f);
        moved |= Step(g_press[i], (g_downId  == i) ? 1.0f : 0.0f, 0.35f);
    }
    if (g_mode != 0) {
        g_pulse += 0.07f;
        if (g_pulse > 6.2831853f) g_pulse -= 6.2831853f;
        PaintFrame();
    } else if (moved) {
        PaintFrame();
    }
}

/* ---------------------------------------------------------------- 窗口过程 */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        /* 画面已由 UpdateLayeredWindow 提交，这里只清一下重绘区域 */
        (void)hdc;
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_NCHITTEST: {
        POINT pt;
        int id;
        pt.x = GET_X_LPARAM(lp);
        pt.y = GET_Y_LPARAM(lp);
        ScreenToClient(hwnd, &pt);
        id = HitTest(pt.x, pt.y);
        if (id == -2) return HTCAPTION;
        if (id == B_NONE) {
            /* 面板内部：吞掉点击，避免误点到下面被遮住的窗口；
               面板外的投影边距：穿透 */
            PointF pf((REAL)(pt.x - SCALE(SHADOW_M)),
                      (REAL)(pt.y - SCALE(SHADOW_M)));
            return rPanel.Contains(pf) ? HTCLIENT : HTTRANSPARENT;
        }
        return HTCLIENT;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int id = HitTest(pt.x, pt.y);
        if (id != g_hoverId) {
            g_hoverId = id;
            /* 手型光标 */
            if (id >= 0) SetCursor(LoadCursorW(NULL, IDC_HAND));
        } else if (id >= 0) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
        }
        if (!g_tracking) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            g_tracking = TRUE;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        g_hoverId = B_NONE;
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (HitTest(pt.x, pt.y) >= 0) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int id = HitTest(pt.x, pt.y);
        LogMsg(L"ui: down %ld,%ld -> hit=%d", pt.x, pt.y, id);
        if (id >= 0) {
            g_downId = id;
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int id = HitTest(pt.x, pt.y);
        int was = g_downId;
        g_downId = B_NONE;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (was >= 0 && id == was) OnClick(id);
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        return 0;

    case WM_TIMER:
        if (wp == TIMER_UI) {
            TickAnimate();
        } else if (wp == TIMER_CLOCK) {
            if (g_mode != 0)
                g_protectSec = (int)((GetTickCount64() - g_startTick) / 1000);

            /* 黑屏模式下：有真实输入就撤掉遮罩；空闲够久再盖上 */
            if (g_mode == 1) {
                if (g_blackOn) {
                    /* 刚盖上时留 1.5 秒防抖，避免启动时的合成输入误唤醒 */
                    if (GetTickCount() - g_blackAt >= 1500 && IdleMillis() < 1500)
                        BlackHideByInput(L"idle");
                } else if (g_peekUntil) {
                    /* peek 窗口结束就再黑；确实有人在用就交给空闲逻辑 */
                    if ((LONG)(GetTickCount() - g_peekUntil) >= 0) {
                        g_peekUntil = 0;
                        if (IdleMillis() >= 1500) {
                            LogMsg(L"peek: 结束 -> 黑屏");
                            BlackShow(TRUE);
                        }
                    }
                } else if (g_reblackSec > 0) {
                    DWORD idle = IdleMillis();
                    if (idle >= (DWORD)g_reblackSec * 1000) {
                        LogMsg(L"reblack: idle=%lu ms -> black", idle);
                        BlackShow(TRUE);
                    }
                }
            }
            MarkDirty();
        } else if (wp == TIMER_SCAN) {
            ScanAgents();
            MarkDirty();
        }
        return 0;

    case WM_INPUT:
        /* RIDEV_INPUTSINK 只用来“知道有按键”，不拦截、不记录内容 */
        if (g_blackOn) BlackHideByInput(L"keyboard");
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) {
            LogMsg(L"power: PBT_APMSUSPEND (entering sleep/Modern Standby)");
            return TRUE;
        }
        if (wp == PBT_APMRESUMESUSPEND || wp == PBT_APMRESUMEAUTOMATIC) {
            LogMsg(L"power: resume");
            if (g_blackOn) {
                BlackFitToDesktop();
                SetWindowPos(g_blackWnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            return TRUE;
        }
        if (wp == PBT_POWERSETTINGCHANGE && lp) {
            PBS_AKA *pbs = (PBS_AKA *)lp;
            if (pbs->DataLength >= 4 &&
                memcmp(&pbs->PowerSetting, &GUID_DISPLAY_STATE, sizeof(GUID)) == 0) {
                DWORD st = *(DWORD *)pbs->Data;
                g_lastDispState = st;
                LogMsg(L"display: state=%lu", st);
                if (st == 0 && g_mode != 0)
                    LogMsg(L"display: WARNING OS turned display off; agents may be suspended");
                MarkDirty();
            }
        }
        return TRUE;

    case WM_HOTKEY:
        if (wp == 1) ToggleProtection();
        return 0;

    case WM_AKA_SHOW:
        ShowMain();
        return 0;

    case WM_AKA_PEEK:
        StartPeek((int)wp);
        return 0;

    case WM_SYSCOMMAND:
        /* 别的软件广播关屏时，至少别让本窗口跟着执行 */
        if (g_mode != 0 &&
            ((wp & 0xFFF0) == SC_MONITORPOWER || (wp & 0xFFF0) == SC_SCREENSAVE)) {
            LogMsg(L"screen: ignored external SC_MONITORPOWER/SC_SCREENSAVE");
            return 0;
        }
        break;

    case WM_TRAY: {
        WORD ev = LOWORD(lp);
        if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            ShowTrayMenu();
        } else if (ev == WM_LBUTTONUP || ev == NIN_SELECT || ev == NIN_KEYSELECT) {
            ShowMain();
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 2001: ToggleProtection(); break;
        case 2002:
            if (g_mode == 0) StartProtection(1);
            else if (g_mode == 1) BlackShow(TRUE);
            break;
        case 2003: ShowMain(); break;
        case 2005: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        }
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (g_mode) StopProtection();
        return 0;

    case WM_CLOSE:
        CleanupAndExit(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------- 退出清理 */
static void CleanupAndExit(HWND hwnd)
{
    if (g_mode != 0) StopProtection();

    KillTimer(hwnd, TIMER_UI);
    KillTimer(hwnd, TIMER_CLOCK);
    KillTimer(hwnd, TIMER_SCAN);

    BlackShow(FALSE);
    UnregisterRawKeyboard();
    BrightRestoreNow();
    if (g_blackWnd) {
        DestroyWindow(g_blackWnd);
        g_blackWnd = NULL;
    }

    if (g_trayOk) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayOk = FALSE;
    }
    UnregisterDisplayNotify();
    UnregisterHotKey(hwnd, 1);
    DestroyWindow(hwnd);
}

/* ---------------------------------------------------------------- 入口 */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    MSG msg;
    HDC sdc;
    HANDLE mutex;
    int w, h, x, y;

    (void)hPrev; (void)show;

    g_inst = hInstance;
    SetProcessDPIAware();

    /* AgentKeepAwake.exe --peek [秒]：只给已运行的实例发消息，自己马上退出。
     * 截屏型 agent 用它换一段“亮屏窗口”，期间不盖黑幕、不压背光。 */
    if (cmd && *cmd) {
        WCHAR *p = cmd, *q;
        while (*p == L' ') ++p;
        if (p[0] == L'-' && p[1] == L'-' &&
            _wcsnicmp(p, L"--peek", 6) == 0) {
            int sec = 0;
            HWND old;
            p += 6;
            while (*p == L' ' || *p == L'=') ++p;
            for (q = p; *q >= L'0' && *q <= L'9'; ++q) sec = sec * 10 + (*q - L'0');
            old = FindWindowW(APP_CLASS, NULL);
            if (old) {
                PostMessageW(old, WM_AKA_PEEK, (WPARAM)sec, 0);
                LogMsg(L"peek: requested from cli (%d s)", sec);
            } else {
                LogMsg(L"peek: 没有运行中的实例，先开始黑屏守护再用 --peek");
            }
            return 0;
        }
    }

    sdc = GetDC(NULL);
    g_dpi = (UINT)GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);
    if (g_dpi < 96) g_dpi = 96;

    mutex = CreateMutexW(NULL, TRUE, L"AgentKeepAwake_SingleInstance_v4");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND old = FindWindowW(APP_CLASS, NULL);
        if (old) {
            /* 让旧实例先撤掉黑屏遮罩，再显示主窗口 */
            PostMessageW(old, WM_AKA_SHOW, 0, 0);
            SetForegroundWindow(old);
        }
        return 0;
    }

    {
        GdiplusStartupInput gsi;
        if (GdiplusStartup(&g_gdipToken, &gsi, NULL) != Ok) return 1;
    }

    LogMsg(L"=== AgentKeepAwake %ls start (pid=%lu, dpi=%u) ===",
           APP_VER, GetCurrentProcessId(), g_dpi);

    LoadSettings();
    LoadAgents();
    BrightStart();

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = APP_CLASS;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);

    if (!RegisterClassExW(&wc)) {
        LogMsg(L"RegisterClassEx failed (%lu)", GetLastError());
        return 1;
    }

    {
        WNDCLASSEXW bc;
        ZeroMemory(&bc, sizeof(bc));
        bc.cbSize = sizeof(bc);
        bc.lpfnWndProc = BlackWndProc;
        bc.hInstance = hInstance;
        bc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        bc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        bc.lpszClassName = BLACK_CLASS;
        if (!RegisterClassExW(&bc))
            LogMsg(L"black: RegisterClassEx failed (%lu)", GetLastError());
    }

    InitCanvas();
    InitGfx();

    w = g_cw;
    h = g_ch;
    x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - h) / 3;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, APP_CLASS, APP_TITLE,
                             WS_POPUP, x, y, w, h, NULL, NULL, hInstance, NULL);
    if (!g_hwnd) {
        LogMsg(L"CreateWindowEx failed (%lu)", GetLastError());
        return 1;
    }

    /* 桌面毛玻璃底必须在窗口显示之前采样，否则会把窗口自身也拍进去 */
    BuildBackdrop();

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    if (RegisterHotKey(g_hwnd, 1, MOD_CONTROL | MOD_ALT, 'K'))
        LogMsg(L"hotkey: Ctrl+Alt+K ok");
    else
        LogMsg(L"hotkey: Ctrl+Alt+K register failed (%lu)", GetLastError());

    RegisterDisplayNotify(g_hwnd);
    AddTrayIcon();
    ScanAgents();
    SetTimer(g_hwnd, TIMER_UI, 16, NULL);
    SetTimer(g_hwnd, TIMER_SCAN, 3000, NULL);
    PaintFrame();

    LogMsg(L"ui: shown %dx%d", w, h);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LogMsg(L"=== AgentKeepAwake exit ===");

    if (g_blackWnd) {
        DestroyWindow(g_blackWnd);
        g_blackWnd = NULL;
    }
    UnregisterClassW(BLACK_CLASS, hInstance);

    /* GDI+ 对象必须在 GdiplusShutdown 之前销毁 */
    delete g_canvas;    g_canvas = NULL;
    delete g_backdrop;  g_backdrop = NULL;
    delete g_glass;     g_glass = NULL;
    delete fName; delete fSub; delete fStatus; delete fStatusSub;
    delete fMetricL; delete fMetricV; delete fBtn; delete fPill; delete fPillOn;
    delete fChip; delete fCardTitle; delete fFoot; delete fHint;

    DeleteObject(g_memBmp);
    DeleteDC(g_memDC);

    GdiplusShutdown(g_gdipToken);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
