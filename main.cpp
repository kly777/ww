// 请求 Windows 10+ API (IVirtualDesktopManager 等)
// 不加这三个宏，windows.h 不会暴露 SHCreateItemFromParsingName 等接口
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000007

#include <io.h>
#include <ole2.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdio.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

constexpr int kMaxWindows = 256;
constexpr UINT WM_TRAYICON = WM_APP + 1;
// WM_INIT_TRAY: 延迟初始化托盘 必须在 GetMessage 循环跑起来之后才能
// NIM_ADD，而 WM_CREATE 在 CreateWindow 返回前就处理完了，所以用 PostMessage
// 推迟到消息循环启动后再执行
constexpr UINT WM_INIT_TRAY = WM_APP + 2;
constexpr UINT kIdTrayIcon = 1;
enum class HotkeyId : int {
    Digit = 0,
    ArrowUp = 10,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    Screenshot = 14
};
enum class MenuId : int { AutoStart = 1001, Exit = 1000 };

// 九宫格导航: 数字→(row,col), 箭头方向
//  1 2 3
//  4 5 6
//  7 8 9
//    0
// kNavMap[当前slit][0=Up 1=Down 2=Left 3=Right] = 目标slot (相同=不动)
static const int kNavMap[10][4] = {
    // Up Dn Lt Rt
    { 8, 2, 7, 9 }, // 0: 只有上→8
    { 7, 4, 3, 2 }, // 1
    { 0, 5, 1, 3 }, // 2
    { 9, 6, 2, 1 }, // 3
    { 1, 7, 6, 5 }, // 4
    { 2, 8, 4, 6 }, // 5
    { 3, 9, 5, 4 }, // 6
    { 4, 1, 9, 8 }, // 7
    { 5, 0, 7, 9 }, // 8
    { 6, 3, 8, 7 }, // 9
};

#ifdef RELEASE
#define LOG(fmt, ...) ((void)0)
#else
#define LOG(fmt, ...)                                                          \
    do {                                                                       \
        printf(fmt, ##__VA_ARGS__);                                            \
        if (g_logFile) {                                                       \
            fprintf(g_logFile, fmt, ##__VA_ARGS__);                            \
            fflush(g_logFile);                                                 \
        }                                                                      \
        fflush(stdout);                                                        \
    } while (0)
#endif

struct WinInfo {
    HWND hwnd;
    std::string title; // 仅用于日志
    RECT rect;
    UINT showCmd; // SW_SHOWNORMAL / SW_MINIMIZE / SW_MAXIMIZE
    UINT zOrder;
};

struct Snapshot {
    std::vector<WinInfo> windows;
    HBITMAP screenBmp = NULL;
    int screenW = 0, screenH = 0;
    RECT capUnion = { 0, 0, 0, 0 }; // 截屏时的 rcWork 并集（用于坐标映射）
};

// ---- 全局 ----
static std::vector<WinInfo> g_windows;
static UINT g_zOrderCounter = 0;
static IVirtualDesktopManager* g_pDesktopManager = NULL;
static int g_trayNumber = 1; // 托盘显示的数字 0-9
static int g_prevTrayNumber = 0;

static std::array<Snapshot, 10> g_snapshots; // 当前桌面的快照

// 每个虚拟桌面独立维护 10 槽快照 + 托盘状态
struct GuidLess {
    bool operator()(const GUID& a, const GUID& b) const
    {
        return memcmp(&a, &b, sizeof(GUID)) < 0;
    }
};
struct DesktopState {
    std::array<Snapshot, 10> snapshots;
    int trayNumber = 0;
    int prevTrayNumber = 0;
};
static std::map<GUID, DesktopState, GuidLess> g_desktopStates;
static GUID g_currentDesktopId = GUID_NULL;

static HWND g_hWnd = NULL;
static FILE* g_logFile = NULL; // 文件日志句柄，仅 #ifndef RELEASE 有效

// ---- 工作区总览预览 ----
static HWND g_previewWnd = NULL;
static UINT g_previewTimer = 0;

// ---- 工具函数 ----
std::string
WideToUtf8(const wchar_t* src)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, src, -1, NULL, 0, NULL, NULL);
    if (len <= 0)
        return {};
    std::string result(len - 1, '\0'); // len 含 null terminator
    WideCharToMultiByte(CP_UTF8, 0, src, -1, &result[0], len, NULL, NULL);
    return result;
}

// IVirtualDesktopManager 要求 STA 线程模型，所以用
// COINIT_APARTMENTTHREADED MTA 下 CoCreateInstance 会返回
// CO_E_NOTINITIALIZED
BOOL
VirtualDesktopManagerInit()
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return FALSE;
    hr = CoCreateInstance(CLSID_VirtualDesktopManager,
                          NULL,
                          CLSCTX_INPROC_SERVER,
                          IID_IVirtualDesktopManager,
                          (void**)&g_pDesktopManager);
    return SUCCEEDED(hr) && g_pDesktopManager;
}

void
VirtualDesktopManagerCleanup()
{
    if (g_pDesktopManager) {
        g_pDesktopManager->Release();
        g_pDesktopManager = NULL;
    }
    CoUninitialize();
}

// ---- 捕捉窗口状态 ----
void
FillWindowInfo(WinInfo& w, HWND hwnd)
{
    w.hwnd = hwnd;
    w.zOrder = g_zOrderCounter++;

    wchar_t wt[256];
    GetWindowTextW(hwnd, wt, 256);
    w.title = WideToUtf8(wt);

    BOOL iconic = IsIconic(hwnd);
    BOOL zoomed = IsZoomed(hwnd);
    if (iconic)
        w.showCmd = SW_MINIMIZE;
    else if (zoomed)
        w.showCmd = SW_MAXIMIZE;
    else
        w.showCmd = SW_SHOWNORMAL;

    // 用 GetWindowPlacement 获取正常位置，而非 GetWindowRect
    // 最小化窗口的 GetWindowRect 返回 (-32000,-32000)——这是系统将窗口
    // "移出屏幕"的内部实现 GetWindowPlacement 的 rcNormalPosition 始终
    // 返回缩小前的正常位置
    WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
    GetWindowPlacement(hwnd, &wp);
    w.rect = wp.rcNormalPosition;
    LOG("[枚举] \"%s\" iconic=%d zoomed=%d rect=(%ld,%ld,%ld,%ld) %ldx%ld\n",
        w.title.c_str(),
        iconic,
        zoomed,
        w.rect.left,
        w.rect.top,
        w.rect.right,
        w.rect.bottom,
        w.rect.right - w.rect.left,
        w.rect.bottom - w.rect.top);
}

// ---- 窗口过滤 ----
BOOL
WindowShouldSkip(HWND hwnd)
{
    if (!IsWindowVisible(hwnd))
        return TRUE;
    wchar_t title[256], wclass[256];
    GetWindowTextW(hwnd, title, 256);
    if (wcslen(title) == 0)
        return TRUE;
    GetClassNameW(hwnd, wclass, 256);
    // 跳过桌面窗口本身 (Progman)
    if (_wcsicmp(wclass, L"Progman") == 0)
        return TRUE;
    // 如果存在其他虚拟桌面，只枚举当前桌面的窗口
    if (g_pDesktopManager) {
        GUID desktopId;
        if (FAILED(g_pDesktopManager->GetWindowDesktopId(hwnd, &desktopId))
            || IsEqualGUID(desktopId, GUID_NULL))
            return TRUE;
    }
    return FALSE;
}

BOOL CALLBACK
EnumWindowCallback(HWND hwnd, LPARAM lParam)
{
    if (WindowShouldSkip(hwnd))
        return TRUE;
    if (g_windows.size() >= kMaxWindows)
        return TRUE;
    WinInfo wi;
    FillWindowInfo(wi, hwnd);
    g_windows.push_back(wi);
    return TRUE;
}

void
SnapSave(int num, const std::vector<WinInfo>& windows)
{
    if (num < 0 || num > 9)
        return;
    Snapshot& snap = g_snapshots[num];
    snap.windows = windows;
    LOG("[快照] 保存 %d 个窗口到数字 %d\n", (int)snap.windows.size(), num);
    for (size_t i = 0; i < windows.size(); i++) {
        const auto& w = windows[i];
        LOG("[保存] [%zu] \"%s\" showCmd=%u rect=(%ld,%ld,%ld,%ld) %ldx%ld\n",
            i,
            w.title.c_str(),
            w.showCmd,
            w.rect.left,
            w.rect.top,
            w.rect.right,
            w.rect.bottom,
            w.rect.right - w.rect.left,
            w.rect.bottom - w.rect.top);
    }
}

// ---- 显示器可见性检查 ----
// 多显→单显切换时，保存的窗口位置可能完全脱离当前显示器（比如副屏
// 上的窗口坐标 x>1920，副屏拔掉后该坐标对应区域不存在任何显示器）
BOOL
RectIsOnScreen(const RECT& rect)
{
    // MONITOR_DEFAULTTONULL: 矩形完全脱离所有显示器时返回 NULL
    return MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != NULL;
}

void
EnsureRectVisible(RECT& rect, int width, int height)
{
    if (RectIsOnScreen(rect))
        return;
    // 窗口完全脱离所有显示器，移到主显示器居中 用 rcWork 而非 rcMonitor
    // 避开任务栏区域
    HMONITOR hPrimary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    GetMonitorInfo(hPrimary, &mi);
    const RECT& work = mi.rcWork;
    rect.left = work.left + ((work.right - work.left) - width) / 2;
    rect.top = work.top + ((work.bottom - work.top) - height) / 2;
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    LOG("[修复] 窗口移到主显示器 (%ld,%ld,%ld,%ld)\n",
        rect.left,
        rect.top,
        rect.right,
        rect.bottom);
}

// ---- 恢复快照 ----
static const char*
CmdToStr(UINT cmd);
void
SnapRestore(int num)
{
    if (num < 0 || num > 9)
        return;
    Snapshot& snap = g_snapshots[num];

    // 临时禁用窗口最小化/还原动画，让批量恢复像瞬间切换而非逐个动画
    ANIMATIONINFO ai = { sizeof(ANIMATIONINFO) };
    BOOL origAnimate = FALSE;
    if (SystemParametersInfo(SPI_GETANIMATION, sizeof(ai), &ai, 0))
        origAnimate = ai.iMinAnimate;
    if (origAnimate) {
        ai.iMinAnimate = 0;
        SystemParametersInfo(SPI_SETANIMATION, sizeof(ai), &ai, 0);
    }

    auto& wins = snap.windows;

    // 将当前可见窗口中"不在目标快照里"的全部最小化
    for (const auto& w : g_windows) {
        auto it = std::find_if(
                wins.begin(), wins.end(), [&](const WinInfo& sw) {
                    return sw.hwnd == w.hwnd;
                });
        if (it == wins.end()) {
            ShowWindow(w.hwnd, SW_MINIMIZE);
        }
    }

#ifndef RELEASE
    LOG("[快照] 从数字 %d 恢复 %d 个窗口\n", num, (int)wins.size());
    // 打印快照原始数据，便于诊断"恢复与保存不一致"问题
    for (size_t i = 0; i < wins.size(); i++) {
        const auto& w = wins[i];
        LOG("[快照数据] [%zu] \"%s\" showCmd=%u rect=(%ld,%ld,%ld,%ld) "
            "zOrder=%u\n",
            i,
            w.title.c_str(),
            w.showCmd,
            w.rect.left,
            w.rect.top,
            w.rect.right,
            w.rect.bottom,
            w.zOrder);
    }
#endif

    // Version 2: 使用 SetWindowPlacement 直接设置位置和显示状态
    // 第一步：恢复窗口状态（位置 + 最小化/最大化/还原）
    //
    // 将位置和 Z 序分成两步的原因：SetWindowPlacement 能原子地设置位置
    // 和显示状态但不控制 Z 序；DeferWindowPos 能批量调 Z 序但不支持
    // 显示状态变更（SW_MINIMIZE / SW_MAXIMIZE）
    //
    // 最小化→最大化跨进程窗口时，直接 SetWindowPlacement(SW_MAXIMIZE)
    // 可能渲染异常（只显示还原尺寸的左上角，其余透明），所以拆成两步
    // 先 SW_SHOWNOACTIVATE 还原，再 SW_SHOWMAXIMIZED 最大化
    for (size_t i = 0; i < wins.size(); i++) {
        const auto& w = wins[i];
        if (!IsWindow(w.hwnd)) {
            LOG("[恢复] [%zu] 窗口已销毁，跳过\n", i);
            continue;
        }
        RECT r = w.rect;
        long ww = r.right - r.left;
        long wh = r.bottom - r.top;
        EnsureRectVisible(r, ww, wh);

        BOOL iconic = IsIconic(w.hwnd);
        const char* showCmdStr = CmdToStr(w.showCmd);
        LOG("[恢复] [%zu] \"%s\" iconic=%d -> %s rect=(%ld,%ld,%ld,%ld) "
            "%ldx%ld\n",
            i,
            w.title.c_str(),
            iconic,
            showCmdStr,
            r.left,
            r.top,
            r.right,
            r.bottom,
            ww,
            wh);

        WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
        wp.rcNormalPosition = r;
        if (w.showCmd != SW_MINIMIZE && iconic) {
            LOG("[恢复] [%zu] 两步还原: SW_SHOWNOACTIVATE -> %s\n",
                i,
                showCmdStr);
            wp.showCmd = SW_SHOWNOACTIVATE;
            SetWindowPlacement(w.hwnd, &wp);
            wp.showCmd = w.showCmd;
            SetWindowPlacement(w.hwnd, &wp);
        } else if (w.showCmd == SW_MAXIMIZE && !iconic) {
            // 已在另一显示器最大化；先还原到目标位置再最大化，实现跨屏移动
            LOG("[恢复] [%zu] 跨屏最大化: SW_SHOWNOACTIVATE -> SW_MAXIMIZE\n",
                i);
            wp.showCmd = SW_SHOWNOACTIVATE;
            SetWindowPlacement(w.hwnd, &wp);
            wp.showCmd = SW_MAXIMIZE;
            SetWindowPlacement(w.hwnd, &wp);
        } else {
            wp.showCmd = w.showCmd;
            SetWindowPlacement(w.hwnd, &wp);
        }
    }

    // Version 1: 使用 ShowWindow 的 SW_RESTORE 先还原，再应用目标状态
    // for (const auto& w : wins) {
    //     if (!IsWindow(w.hwnd)) continue;
    //     WINDOWPLACEMENT wp = {sizeof(WINDOWPLACEMENT)};
    //     wp.showCmd = w.showCmd;
    //     wp.rcNormalPosition = w.rect;
    //     // ptMinPosition / ptMaxPosition 置零，由系统自己计算
    //     SetWindowPlacement(w.hwnd, &wp);
    // }
    // 使用 ShowWindow 的 SW_RESTORE 先还原，再应用目标状态

    // Version 3: 使用 ShowWindow 的 SW_RESTORE 先还原，再应用目标状态
    // for (const auto& w : wins) {
    //     if (!IsWindow(w.hwnd)) continue;

    //     // 如果窗口当前是最小化，先还原
    //     if (IsIconic(w.hwnd) && w.showCmd != SW_MINIMIZE) {
    //         ShowWindow(w.hwnd, SW_RESTORE);
    //     }

    //     // 然后设置目标状态
    //     if (w.showCmd == SW_MAXIMIZE) {
    //         ShowWindow(w.hwnd, SW_MAXIMIZE);
    //     } else if (w.showCmd == SW_MINIMIZE) {
    //         ShowWindow(w.hwnd, SW_MINIMIZE);
    //     } else {
    //         // 正常状态：可能需要调整位置
    //         WINDOWPLACEMENT wp = {sizeof(WINDOWPLACEMENT)};
    //         wp.showCmd = SW_SHOWNORMAL;
    //         wp.rcNormalPosition = w.rect;
    //         SetWindowPlacement(w.hwnd, &wp);
    //     }
    // }

    // 第二步：按 zOrder 恢复 Z 序
    // 排序后从 HWND_BOTTOM 开始逐个往上叠，恢复原始前后关系
    // 注意：必须在副本上排序，不能直接修改 snap.windows，
    // 否则会永久破坏已保存快照的窗口顺序
    std::vector<WinInfo> sortedWins = wins;
    std::sort(sortedWins.begin(),
              sortedWins.end(),
              [](const WinInfo& a, const WinInfo& b) {
                  return a.zOrder < b.zOrder;
              });

    if (!sortedWins.empty()) {
        HWND hwndTop = sortedWins[0].hwnd;
        HDWP hdwp = BeginDeferWindowPos((int)sortedWins.size());
        if (hdwp) {
            HWND after = HWND_BOTTOM;
            for (const auto& w : sortedWins) {
                if (!IsWindow(w.hwnd))
                    continue;
                // SWP_NOMOVE | SWP_NOSIZE: 位置和大小已由 SetWindowPlacement
                // 设置好了，这里只修 Z 序
                // SWP_NOACTIVATE: 不改变当前活动窗口，避免抢焦点
                UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
                hdwp = DeferWindowPos(hdwp, w.hwnd, after, 0, 0, 0, 0, flags);
                if (!hdwp)
                    break;
                after = w.hwnd;
            }
            if (hdwp)
                EndDeferWindowPos(hdwp);
        }
        // SetForegroundWindow(hwndTop);
    }

    // 恢复动画设置
    if (origAnimate) {
        ai.iMinAnimate = origAnimate;
        SystemParametersInfo(SPI_SETANIMATION, sizeof(ai), &ai, 0);
    }

    // ---- 恢复后验证：对比实际窗口状态与快照数据 ----
#ifndef RELEASE
    LOG("[验证] 开始对比恢复结果与快照数据...\n");
    for (size_t i = 0; i < wins.size(); i++) {
        const auto& w = wins[i];
        if (!IsWindow(w.hwnd)) {
            LOG("[验证] [%zu] \"%s\" 窗口已销毁，跳过\n", i, w.title.c_str());
            continue;
        }
        BOOL iconic = IsIconic(w.hwnd);
        BOOL zoomed = IsZoomed(w.hwnd);
        UINT actualShowCmd;
        if (iconic)
            actualShowCmd = SW_MINIMIZE;
        else if (zoomed)
            actualShowCmd = SW_MAXIMIZE;
        else
            actualShowCmd = SW_SHOWNORMAL;

        WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
        GetWindowPlacement(w.hwnd, &wp);
        RECT actualRect = wp.rcNormalPosition;

        const char* expectedStr = CmdToStr(w.showCmd);
        const char* actualStr = CmdToStr(actualShowCmd);

        bool showCmdMatch = (w.showCmd == actualShowCmd);
        bool rectMatch = (w.rect.left == actualRect.left
                          && w.rect.top == actualRect.top
                          && w.rect.right == actualRect.right
                          && w.rect.bottom == actualRect.bottom);

        if (!showCmdMatch) {
            LOG("[验证] [!] [%zu] \"%s\" 状态不一致! 期望=%s(%u) 实际=%s(%u)\n",
                i,
                w.title.c_str(),
                expectedStr,
                w.showCmd,
                actualStr,
                actualShowCmd);
        }
        if (!rectMatch) {
            LOG("[验证] [!] [%zu] \"%s\" 位置不一致! 期望=(%ld,%ld,%ld,%ld) "
                "实际=(%ld,%ld,%ld,%ld)\n",
                i,
                w.title.c_str(),
                w.rect.left,
                w.rect.top,
                w.rect.right,
                w.rect.bottom,
                actualRect.left,
                actualRect.top,
                actualRect.right,
                actualRect.bottom);
        }
        if (showCmdMatch && rectMatch) {
            LOG("[验证] [%zu] \"%s\" OK\n", i, w.title.c_str());
        }
    }
    LOG("[验证] 完成\n");
#endif

    LOG("[恢复] 完成\n");
}

// ---- 切换工作区 ----
void
TrayIconUpdate();
// 通过已枚举窗口探测桌面 GUID 并同步桌面状态
// EnumWindows 回调中 ShouldSkipWindow 已用 GetWindowDesktopId 过滤，
// 故 g_windows 中每个窗口都有合法桌面 GUID，比 GetForegroundWindow 可靠
static void
DesktopStateSync()
{
    GUID newId = GUID_NULL;
    if (g_pDesktopManager && !g_windows.empty())
        g_pDesktopManager->GetWindowDesktopId(g_windows[0].hwnd, &newId);

    // 无法确定桌面 GUID（g_windows 为空或虚拟桌面管理器不可用）
    if (IsEqualGUID(newId, GUID_NULL))
        return;

    // 首次运行
    if (IsEqualGUID(g_currentDesktopId, GUID_NULL)) {
        g_currentDesktopId = newId;
        return;
    }
    if (IsEqualGUID(g_currentDesktopId, newId))
        return;

    // 保存当前桌面状态
    DesktopState& oldState = g_desktopStates[g_currentDesktopId];
    oldState.snapshots = g_snapshots;
    oldState.trayNumber = g_trayNumber;
    oldState.prevTrayNumber = g_prevTrayNumber;

    LOG("[桌面] 离开桌面 (tray=%d)\n", g_trayNumber);

    // 加载新桌面状态
    auto it = g_desktopStates.find(newId);
    if (it != g_desktopStates.end()) {
        g_snapshots = it->second.snapshots;
        g_trayNumber = it->second.trayNumber;
        g_prevTrayNumber = it->second.prevTrayNumber;
        LOG("[桌面] 进入已知桌面 (tray=%d)\n", g_trayNumber);
    } else {
        g_snapshots = {};
        g_trayNumber = 0;
        g_prevTrayNumber = 0;
        LOG("[桌面] 进入新桌面\n");
    }
    g_currentDesktopId = newId;
    TrayIconUpdate();
}

// EnumDisplayMonitors 回调上下文
struct CapCtx {
    HDC hdcMem;
    HDC hdcScreen;
    int bmpW, bmpH;
    RECT workUnion; // 所有 rcWork 的并集
};

static BOOL CALLBACK CapMonitorProc(HMONITOR, HDC, LPRECT, LPARAM);
static BOOL CALLBACK CapDrawProc(HMONITOR, HDC, LPRECT, LPARAM);

// ---- 工作区并集 ----
static RECT
WorkAreaUnionGet()
{
    CapCtx ctx = { NULL, NULL };
    ctx.workUnion = { 0, 0, 0, 0 };
    EnumDisplayMonitors(NULL, NULL, CapMonitorProc, (LPARAM)&ctx);
    return ctx.workUnion;
}

// ---- showCmd → 字符串 ----
static const char*
CmdToStr(UINT cmd)
{
    return cmd == SW_MAXIMIZE   ? "最大化"
           : cmd == SW_MINIMIZE ? "最小化"
                                : "正常";
}

static BOOL CALLBACK
CapMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    CapCtx* c = (CapCtx*)lp;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hMon, &mi))
        return TRUE;
    // 累计 rcWork 并集
    if (c->workUnion.left == c->workUnion.right) {
        c->workUnion = mi.rcWork;
    } else {
        if (mi.rcWork.left < c->workUnion.left)
            c->workUnion.left = mi.rcWork.left;
        if (mi.rcWork.top < c->workUnion.top)
            c->workUnion.top = mi.rcWork.top;
        if (mi.rcWork.right > c->workUnion.right)
            c->workUnion.right = mi.rcWork.right;
        if (mi.rcWork.bottom > c->workUnion.bottom)
            c->workUnion.bottom = mi.rcWork.bottom;
    }
    return TRUE;
}

// 第二次遍历：实际绘制
static BOOL CALLBACK
CapDrawProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    CapCtx* c = (CapCtx*)lp;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hMon, &mi))
        return TRUE;
    int mx = mi.rcWork.left - c->workUnion.left;
    int my = mi.rcWork.top - c->workUnion.top;
    int mw = mi.rcWork.right - mi.rcWork.left;
    int mh = mi.rcWork.bottom - mi.rcWork.top;
    int uw = c->workUnion.right - c->workUnion.left;
    int uh = c->workUnion.bottom - c->workUnion.top;
    int dx = mx * c->bmpW / uw;
    int dy = my * c->bmpH / uh;
    int dw = mw * c->bmpW / uw;
    int dh = mh * c->bmpH / uh;
    SetStretchBltMode(c->hdcMem, COLORONCOLOR);
    StretchBlt(c->hdcMem,
               dx,
               dy,
               dw,
               dh,
               c->hdcScreen,
               mi.rcWork.left,
               mi.rcWork.top,
               mw,
               mh,
               SRCCOPY);
    return TRUE;
}

// ---- 工作区截图 ----
static void
SnapCapture(int slot)
{
    Snapshot& snap = g_snapshots[slot];
    if (snap.screenBmp)
        DeleteObject(snap.screenBmp);
    RECT workUnion = WorkAreaUnionGet();
    snap.capUnion = workUnion;
    int uw = workUnion.right - workUnion.left;
    int uh = workUnion.bottom - workUnion.top;
    if (uw <= 0 || uh <= 0)
        return;
    snap.screenW = uw / 4;
    snap.screenH = uh / 4;
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    snap.screenBmp
            = CreateCompatibleBitmap(hdcScreen, snap.screenW, snap.screenH);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, snap.screenBmp);
    RECT rcB = { 0, 0, snap.screenW, snap.screenH };
    FillRect(hdcMem, &rcB, (HBRUSH)GetStockObject(BLACK_BRUSH));
    CapCtx ctx = { hdcMem, hdcScreen, snap.screenW, snap.screenH, workUnion };
    EnumDisplayMonitors(NULL, NULL, CapDrawProc, (LPARAM)&ctx);
    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

// 直接切换到指定快照
void
Snap_Switch(int slot)
{
    if (slot < 0 || slot > 9)
        return;
    if (slot == g_trayNumber)
        return;

    g_windows.clear();
    g_zOrderCounter = 0;
    EnumWindows(EnumWindowCallback, 0);
    DesktopStateSync();

    SnapSave(g_trayNumber, g_windows);
    SnapCapture(g_trayNumber);

    g_prevTrayNumber = g_trayNumber;
    g_trayNumber = slot;

    SnapRestore(slot);
    TrayIconUpdate();
    LOG("[切换] %d -> %d\n", g_prevTrayNumber, slot);
}

// ---- 生成托盘图标 ----
HICON
MakeTrayIcon(int number)
{
    constexpr int kIconSize = 32;

    HDC hScreenDC = GetDC(NULL);
    HDC hMemDC = CreateCompatibleDC(hScreenDC);

    // biHeight 为负值 = top-down DIB，此时位图数据从顶行开始排列
    // 简化后面 alpha 通道填充的寻址（bits[0] 就是第一行第一个像素）
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kIconSize;
    bmi.bmiHeader.biHeight = -kIconSize;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HBITMAP hBmpColor
            = CreateDIBSection(hMemDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hMemDC, hBmpColor);

    static const COLORREF kColors[] = {
        RGB(180, 50, 50),  // 0 红
        RGB(30, 100, 210), // 1 蓝
        RGB(40, 150, 70),  // 2 绿
        RGB(200, 140, 20), // 3 橙
        RGB(130, 60, 180), // 4 紫
        RGB(20, 150, 150), // 5 青
        RGB(200, 80, 140), // 6 粉
        RGB(80, 80, 80),   // 7 灰
        RGB(30, 140, 210), // 8 天蓝
        RGB(180, 160, 30), // 9 金
    };
    const RECT rcFull = { 0, 0, kIconSize, kIconSize };

    // ---- 绘制背景 ----
    HBRUSH hBrBackground = CreateSolidBrush(kColors[number]);
    FillRect(hMemDC, &rcFull, hBrBackground);

    // ---- 九宫格指示灯（底部居中为 0，其余 1-9 按 3×3 排列）----
    // row: 0=上 1=中 2=下 3=最下(仅数字0)
    static const int kDigitCol[10] = { 1, 0, 1, 2, 0, 1, 2, 0, 1, 2 };
    static const int kDigitRow[10] = { 3, 0, 0, 0, 1, 1, 1, 2, 2, 2 };

    constexpr int kCellSize = 10;
    constexpr int kGridX = 1;
    constexpr int kGridY = 1;
    const int cellX = kGridX + kDigitCol[number] * kCellSize;
    const int cellY = kGridY + kDigitRow[number] * kCellSize;

    const RECT rcCell = { cellX, cellY, cellX + kCellSize, cellY + kCellSize };
    HBRUSH hBrCell = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hMemDC, &rcCell, hBrCell);
    DeleteObject(hBrCell);

    // ---- 绘制白色外框（空心矩形）----
    HPEN hPenBorder = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN hOldPen = (HPEN)SelectObject(hMemDC, hPenBorder);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hMemDC, GetStockObject(NULL_BRUSH));
    Rectangle(hMemDC, 0, 0, kIconSize, kIconSize);

    // ---- 绘制数字 ----
    SetBkMode(hMemDC, TRANSPARENT);
    SetTextColor(hMemDC, RGB(255, 255, 255));
    const wchar_t digitText[2] = { (wchar_t)(L'0' + number), L'\0' };
    HFONT hFont = CreateFontW(34,
                              0,
                              0,
                              0,
                              FW_NORMAL,
                              0,
                              0,
                              0,
                              DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY,
                              DEFAULT_PITCH,
                              L"Segoe UI");
    HFONT hOldFont = (HFONT)SelectObject(hMemDC, hFont);
    SIZE textSize;
    GetTextExtentPoint32W(hMemDC, digitText, 1, &textSize);
    TEXTMETRIC textMetrics;
    GetTextMetrics(hMemDC, &textMetrics);
    const int textY = (kIconSize - textMetrics.tmAscent) / 2 - 5;
    TextOutW(hMemDC, (kIconSize - textSize.cx) / 2, textY, digitText, 1);

    // GDI 对象在选入 DC 时不能删除，必须先逐一 SelectObject 恢复原始
    // 对象（按入栈反序），之后再 DeleteObject 才安全
    SelectObject(hMemDC, hOldFont);
    SelectObject(hMemDC, hOldBrush);
    SelectObject(hMemDC, hOldPen);
    SelectObject(hMemDC, hOldBmp);

    DeleteObject(hFont);
    DeleteObject(hPenBorder);
    DeleteObject(hBrBackground);

    // 填充 alpha 通道为不透明：CreateDIBSection 不会初始化像素数据，
    // 背景填充只写了 RGB 没写 A，这里补上
    if (bits) {
        BYTE* pixel = (BYTE*)bits;
        for (int i = 0; i < kIconSize * kIconSize; i++)
            pixel[i * 4 + 3] = 0xFF;
    }

    // 掩码位图：全白代表图标完全不透明，CreateIconIndirect 同时需要
    // 颜色位图和掩码合成最终图标
    HBITMAP hBmpMask = CreateBitmap(kIconSize, kIconSize, 1, 1, NULL);
    HDC hMaskDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hOldMask = (HBITMAP)SelectObject(hMaskDC, hBmpMask);
    FillRect(hMaskDC, &rcFull, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SelectObject(hMaskDC, hOldMask);
    DeleteDC(hMaskDC);

    ICONINFO iconInfo = {};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = hBmpColor;
    iconInfo.hbmMask = hBmpMask;
    HICON hIcon = CreateIconIndirect(&iconInfo);

    DeleteObject(hBmpMask);
    DeleteObject(hBmpColor);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hScreenDC);
    return hIcon;
}

static BOOL g_trayAdded = FALSE;
// ---- 更新托盘图标 ----
void
TrayIconUpdate()
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
    nid.hWnd = g_hWnd;
    nid.uID = kIdTrayIcon;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;

    nid.hIcon = MakeTrayIcon(g_trayNumber);
    swprintf(nid.szTip, 128, L"ww-%d", g_trayNumber);

    if (!g_trayAdded) {
        Shell_NotifyIconW(NIM_ADD, &nid);
        // NIM_ADD 后 Shell 已持有图标副本，可以销毁我们的 GDI 对象
        if (nid.hIcon)
            DestroyIcon(nid.hIcon);
        g_trayAdded = TRUE;
    } else {
        if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
            LOG("[!] NIM_MODIFY 失败: %lu\n", GetLastError());
        if (nid.hIcon)
            DestroyIcon(nid.hIcon);
    }
}

void
RegisterHotkeys(HWND hwnd)
{
    for (int i = 0; i <= 9; i++) {
        if (!RegisterHotKey(hwnd,
                            static_cast<int>(HotkeyId::Digit) + i,
                            MOD_CONTROL | MOD_NOREPEAT,
                            '0' + i)) {
            LOG("[!] 注册热键 Ctrl+%d 失败 (错误码: %lu)\n", i, GetLastError());
        }
    }
    RegisterHotKey(hwnd,
                   static_cast<int>(HotkeyId::ArrowUp),
                   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                   VK_UP);
    RegisterHotKey(hwnd,
                   static_cast<int>(HotkeyId::ArrowDown),
                   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                   VK_DOWN);
    RegisterHotKey(hwnd,
                   static_cast<int>(HotkeyId::ArrowLeft),
                   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                   VK_LEFT);
    RegisterHotKey(hwnd,
                   static_cast<int>(HotkeyId::ArrowRight),
                   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                   VK_RIGHT);
    RegisterHotKey(hwnd,
                   static_cast<int>(HotkeyId::Screenshot),
                   MOD_ALT | MOD_NOREPEAT,
                   'S');
}

void
UnregisterHotkeys(HWND hwnd)
{
    for (int i = 0; i <= 9; i++) {
        UnregisterHotKey(hwnd, static_cast<int>(HotkeyId::Digit) + i);
    }
    UnregisterHotKey(hwnd, static_cast<int>(HotkeyId::ArrowUp));
    UnregisterHotKey(hwnd, static_cast<int>(HotkeyId::ArrowDown));
    UnregisterHotKey(hwnd, static_cast<int>(HotkeyId::ArrowLeft));
    UnregisterHotKey(hwnd, static_cast<int>(HotkeyId::ArrowRight));
    UnregisterHotKey(hwnd, static_cast<int>(HotkeyId::Screenshot));
}

// ---- 开机启动 ----
// 通过 HKCU\...\Run 注册表项实现，系统启动时自动拉起 ww.exe
BOOL
IsAutoStartEnabled()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0,
                      KEY_READ,
                      &hKey)
        != ERROR_SUCCESS)
        return FALSE;
    wchar_t path[MAX_PATH];
    DWORD size = sizeof(path);
    DWORD type;
    LSTATUS ret
            = RegQueryValueExW(hKey, L"WW", NULL, &type, (BYTE*)path, &size);
    RegCloseKey(hKey);
    return ret == ERROR_SUCCESS;
}

void
SetAutoStart(BOOL enable)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0,
                      KEY_SET_VALUE,
                      &hKey)
        != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        RegSetValueExW(hKey,
                       L"WW",
                       0,
                       REG_SZ,
                       (BYTE*)path,
                       (DWORD)(wcslen(path) + 1) * sizeof(wchar_t));
    } else {
        RegDeleteValueW(hKey, L"WW");
    }
    RegCloseKey(hKey);
}

// ---- 工作区总览预览 (snapshot 1-9 拼成 3×3 大图) ----
static const wchar_t* PREVIEW_CLASS = L"WW_Preview";
static HBITMAP g_overviewBmp = NULL;
static int g_overviewW = 0, g_overviewH = 0;

LRESULT CALLBACK
PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_overviewBmp) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int border = 6;
            // 先填深色背景，消除白边
            RECT rcFill = { 0, 0, rc.right, rc.bottom };
            HBRUSH hBrBg = CreateSolidBrush(RGB(40, 40, 40));
            FillRect(hdc, &rcFill, hBrBg);
            DeleteObject(hBrBg);
            // 截图内缩 border px
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, g_overviewBmp);
            SetStretchBltMode(hdc, COLORONCOLOR);
            StretchBlt(hdc,
                       border,
                       border,
                       rc.right - border * 2,
                       rc.bottom - border * 2,
                       memDC,
                       0,
                       0,
                       g_overviewW,
                       g_overviewH,
                       SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteDC(memDC);
            // 6px 外边框（笔宽中心在截图边缘，不侵入内容）
            int hb = border / 2;
            HPEN hPen = CreatePen(PS_SOLID, border, RGB(40, 40, 40));
            HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
            HBRUSH hOldBr
                    = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, hb, hb, rc.right - hb, rc.bottom - hb);
            SelectObject(hdc, hOldBr);
            SelectObject(hdc, hOldPen);
            DeleteObject(hPen);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        // 先隐藏自身再切换，避免被截入快照
        ShowWindow(hwnd, SW_HIDE);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int mx = LOWORD(lParam);
        int my = HIWORD(lParam);
        int col = mx * 3 / (rc.right - rc.left);
        int row = my * 3 / (rc.bottom - rc.top);
        if (col >= 0 && col < 3 && row >= 0 && row < 3) {
            int slot = row * 3 + col + 1;
            Snap_Switch(slot);
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_KEYDOWN:
        DestroyWindow(hwnd);
        return 0;
    case WM_TIMER:
        if (wParam == g_previewTimer)
            DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_previewTimer) {
            KillTimer(hwnd, g_previewTimer);
            g_previewTimer = 0;
        }
        g_previewWnd = NULL;
        if (g_overviewBmp) {
            DeleteObject(g_overviewBmp);
            g_overviewBmp = NULL;
            g_overviewW = g_overviewH = 0;
        }
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---- 工作区总览合成 ----
static void
OverviewBuild()
{
    if (g_overviewBmp) {
        DeleteObject(g_overviewBmp);
        g_overviewBmp = NULL;
    }

    RECT workUnion = WorkAreaUnionGet();
    int sw = workUnion.right - workUnion.left;
    int sh = workUnion.bottom - workUnion.top;
    if (sw <= 0)
        sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    if (sh <= 0)
        sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    constexpr int kCols = 3, kRows = 3;
    int cellW = sw / kCols, cellH = sh / kRows;
    int totalW = cellW * kCols, totalH = cellH * kRows;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcComp = CreateCompatibleDC(hdcScreen);
    g_overviewBmp = CreateCompatibleBitmap(hdcScreen, totalW, totalH);
    g_overviewW = totalW;
    g_overviewH = totalH;
    HBITMAP hOldComp = (HBITMAP)SelectObject(hdcComp, g_overviewBmp);

    RECT rcBg = { 0, 0, totalW, totalH };
    FillRect(hdcComp, &rcBg, (HBRUSH)GetStockObject(WHITE_BRUSH));

    HFONT hFont = CreateFontW(20,
                              0,
                              0,
                              0,
                              FW_NORMAL,
                              FALSE,
                              FALSE,
                              FALSE,
                              DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY,
                              DEFAULT_PITCH,
                              L"Segoe UI");
    HFONT hOldFont = (HFONT)SelectObject(hdcComp, hFont);
    SetBkMode(hdcComp, TRANSPARENT);

    int hasAny = 0;
    for (int slot = 1; slot <= 9; slot++) {
        int col = (slot - 1) % kCols, row = (slot - 1) / kCols;
        int ox = col * cellW, oy = row * cellH;
        Snapshot& snap = g_snapshots[slot];

        RECT rcCell = { ox, oy, ox + cellW, oy + cellH };
        if (snap.screenBmp) {
            HDC hdcSrc = CreateCompatibleDC(hdcScreen);
            HBITMAP hOldSrc = (HBITMAP)SelectObject(hdcSrc, snap.screenBmp);
            // 保持比例，靠左上角填充
            double sx = (double)cellW / snap.screenW;
            double sy = (double)cellH / snap.screenH;
            double scale = sx < sy ? sx : sy;
            int dw = (int)(snap.screenW * scale);
            int dh = (int)(snap.screenH * scale);
            SetStretchBltMode(hdcComp, COLORONCOLOR);
            StretchBlt(hdcComp,
                       ox,
                       oy,
                       dw,
                       dh,
                       hdcSrc,
                       0,
                       0,
                       snap.screenW,
                       snap.screenH,
                       SRCCOPY);
            SelectObject(hdcSrc, hOldSrc);
            DeleteDC(hdcSrc);
            hasAny++;
        } else {
            // 空快照：浅灰背景
            HBRUSH hBr = CreateSolidBrush(RGB(248, 248, 250));
            FillRect(hdcComp, &rcCell, hBr);
            DeleteObject(hBr);
        }

        // 网格线
        HPEN hPen = CreatePen(PS_SOLID, 3, RGB(160, 160, 160));
        HPEN hOldPen = (HPEN)SelectObject(hdcComp, hPen);
        MoveToEx(hdcComp, ox, oy, NULL);
        LineTo(hdcComp, ox + cellW, oy);
        LineTo(hdcComp, ox + cellW, oy + cellH);
        LineTo(hdcComp, ox, oy + cellH);
        LineTo(hdcComp, ox, oy);
        SelectObject(hdcComp, hOldPen);
        DeleteObject(hPen);
    }

    if (hasAny == 0) {
        SetTextColor(hdcComp, RGB(160, 160, 160));
        const wchar_t* hint
                = L"Ctrl+1~9 to save · Ctrl+N to switch · Alt+S to overview";
        SIZE ts;
        GetTextExtentPoint32W(hdcComp, hint, (int)wcslen(hint), &ts);
        TextOutW(hdcComp,
                 (totalW - ts.cx) / 2,
                 totalH / 2 - ts.cy / 2,
                 hint,
                 (int)wcslen(hint));
    }

    SelectObject(hdcComp, hOldFont);
    DeleteObject(hFont);
    SelectObject(hdcComp, hOldComp);
    DeleteDC(hdcComp);
    ReleaseDC(NULL, hdcScreen);
}

// ---- 预览窗口 ----
static void
OverviewShow(POINT mousePt)
{
    constexpr int kMaxPreviewW = 1650, kCols = 3, kRows = 3;
    int previewW = g_overviewW, previewH = g_overviewH;
    if (previewW > kMaxPreviewW) {
        previewH = previewH * kMaxPreviewW / previewW;
        previewW = kMaxPreviewW;
    }

    int curCol = (g_trayNumber - 1) % kCols;
    int curRow = (g_trayNumber - 1) / kCols;
    int x = mousePt.x - (int)((curCol + 0.5) / kCols * previewW);
    int y = mousePt.y - (int)((curRow + 0.5) / kRows * previewH);

    HMONITOR hMon = MonitorFromPoint(mousePt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);
    if (x + previewW > mi.rcWork.right)
        x = mi.rcWork.right - previewW;
    if (y + previewH > mi.rcWork.bottom)
        y = mi.rcWork.bottom - previewH;
    if (x < mi.rcWork.left)
        x = mi.rcWork.left;
    if (y < mi.rcWork.top)
        y = mi.rcWork.top;

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = PreviewWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = PREVIEW_CLASS;
        RegisterClassW(&wc);
        registered = true;
    }

    g_previewWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                   PREVIEW_CLASS,
                                   L"Overview",
                                   WS_POPUP,
                                   x,
                                   y,
                                   previewW,
                                   previewH,
                                   NULL,
                                   NULL,
                                   GetModuleHandle(NULL),
                                   NULL);
    if (g_previewWnd) {
        ShowWindow(g_previewWnd, SW_SHOWNOACTIVATE);
        UpdateWindow(g_previewWnd);
        g_previewTimer = SetTimer(g_previewWnd, 1, 8000, NULL);
    }
}

static void
CaptureAndShow()
{
    // 先销毁旧窗口，再截取当前快照确保总览是最新状态
    if (g_previewWnd && IsWindow(g_previewWnd))
        DestroyWindow(g_previewWnd);
    SnapCapture(g_trayNumber);
    OverviewBuild();
    POINT pt;
    GetCursorPos(&pt);
    OverviewShow(pt);
}

LRESULT CALLBACK
WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        RegisterHotkeys(hwnd);
        PostMessage(hwnd, WM_INIT_TRAY, 0, 0);
        return 0;
    }

    case WM_INIT_TRAY: {
        EnumWindows(EnumWindowCallback, 0);
        LOG("共 %d 个窗口\n", (int)g_windows.size());
        TrayIconUpdate();
        return 0;
    }

    case WM_DESTROY: {
        UnregisterHotkeys(hwnd);
        NOTIFYICONDATAW nid = {};
        nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
        nid.hWnd = hwnd;
        nid.uID = kIdTrayIcon;
        Shell_NotifyIconW(NIM_DELETE, &nid);
#ifndef RELEASE
        if (g_logFile) {
            fclose(g_logFile);
            g_logFile = NULL;
        }
#endif
        PostQuitMessage(0);
        return 0;
    }

    case WM_HOTKEY: {
        int id = (int)wParam;
        if (id == static_cast<int>(HotkeyId::Screenshot)) {
            CaptureAndShow();
            return 0;
        }
        int slot = id - static_cast<int>(HotkeyId::Digit);
        if (slot >= 0 && slot <= 9) {
            // 双击同一数字 → 回到上一个快照
            if (slot == g_trayNumber) {
                int prev = g_prevTrayNumber;
                if (prev != g_trayNumber)
                    Snap_Switch(prev);
            } else {
                Snap_Switch(slot);
            }
        } else {
            int dir = id - static_cast<int>(HotkeyId::ArrowUp);
            if (dir >= 0 && dir <= 3) {
                int target = kNavMap[g_trayNumber][dir];
                if (target != g_trayNumber)
                    Snap_Switch(target);
            }
        }
        return 0;
    }

    case WM_TRAYICON: {
        if (lParam == WM_RBUTTONUP) {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu,
                        MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0),
                        (UINT_PTR)MenuId::AutoStart,
                        L"开机启动");
            AppendMenuW(hMenu, MF_STRING, (UINT_PTR)MenuId::Exit, L"退出");

            POINT pt;
            GetCursorPos(&pt);
            // 必须先 SetForegroundWindow，否则 TrackPopupMenu 弹出后
            // 点击菜单外区域不会自动关闭（Windows 前台窗口规则）
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(hMenu,
                                     TPM_RETURNCMD | TPM_NONOTIFY,
                                     pt.x,
                                     pt.y,
                                     0,
                                     hwnd,
                                     NULL);
            DestroyMenu(hMenu);

            if (cmd == (int)MenuId::AutoStart) {
                SetAutoStart(!IsAutoStartEnabled());
            } else if (cmd == (int)MenuId::Exit) {
                DestroyWindow(hwnd);
            }
        }
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---- 创建隐藏窗口（用于接收消息） ----
// 使用 WS_POPUP 而非 HWND_MESSAGE 父窗口，因为 Shell_NotifyIcon 需要
// 一个能接收回调消息的真实窗口句柄 WS_POPUP 创建 0×0 的不可见窗口
BOOL
MessageWindowCreate(HINSTANCE hInstance)
{
    const wchar_t* CLASS_NAME = L"WW_TrayWindow";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    g_hWnd = CreateWindowExW(0,
                             CLASS_NAME,
                             L"WW",
                             WS_POPUP,
                             0,
                             0,
                             0,
                             0,
                             NULL,
                             NULL,
                             hInstance,
                             NULL);
    return g_hWnd != NULL;
}

int
main()
{
    // 单实例保护：命名互斥体跨进程可见，第二个实例检测到已存在直接退出
    HANDLE hMutex = CreateMutexW(NULL, FALSE, L"WW_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG("已有实例在运行，退出\n");
        return 0;
    }

    SetProcessDPIAware();
#ifndef RELEASE
    SetConsoleOutputCP(CP_UTF8);
#endif
    HINSTANCE hInst = GetModuleHandle(NULL);

#ifndef RELEASE
    // 初始化文件日志：输出到 exe 同目录下的 ww.log
    // 使用共享读写模式打开，允许外部（如测试脚本）同时读取日志
    wchar_t logPath[MAX_PATH];
    GetModuleFileNameW(NULL, logPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(logPath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
        wcscat_s(logPath, MAX_PATH, L"ww.log");
        HANDLE hFile = CreateFileW(logPath,
                                   FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL,
                                   OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL,
                                   NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            int fd = _open_osfhandle((intptr_t)hFile, 0);
            if (fd != -1)
                g_logFile = _fdopen(fd, "a");
        }
    }
#endif

    VirtualDesktopManagerInit();

    if (!MessageWindowCreate(hInst)) {
        LOG("创建消息窗口失败\n");
        VirtualDesktopManagerCleanup();
        return 1;
    }

    LOG("\n=== 托盘图标已创建 ===\n");
    LOG("Ctrl+0~9 切换九宫格 | Ctrl+Alt+方向键 在格子间移动 | "
        "Alt+S 截屏预览 | 右键托盘选择数字或退出\n\n");

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    VirtualDesktopManagerCleanup();
#ifndef RELEASE
    if (g_logFile)
        fclose(g_logFile);
#endif
    return 0;
}
