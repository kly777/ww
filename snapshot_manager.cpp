// 请求 Windows 10+ API (IVirtualDesktopManager)
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000007

#include "snapshot_manager.h"

#include <algorithm>
#include <cstdio>
#include <unordered_set>
#include <shobjidl.h> // IVirtualDesktopManager 完整定义

// ---- 日志宏 (snapshot_manager 独立版本，不依赖 g_logFile) ----
#ifdef RELEASE
#define LOG(fmt, ...) ((void)0)
#else
#define LOG(fmt, ...)                                                          \
    do {                                                                       \
        printf(fmt, ##__VA_ARGS__);                                            \
        fflush(stdout);                                                        \
    } while (0)
#endif

// ===================================================================
// SnapshotManager
// ===================================================================

SnapshotManager::SnapshotManager(TrayUpdater onTrayUpdate,
                                 IVirtualDesktopManager* pDesktopMgr)
  : m_onTrayUpdate(std::move(onTrayUpdate))
  , m_pDesktopMgr(pDesktopMgr)
{
}

SnapshotManager::~SnapshotManager()
{
    for (auto& snap : m_snapshots) {
        if (snap.screenBmp) {
            DeleteObject(snap.screenBmp);
            snap.screenBmp = NULL;
        }
    }
}

void
SnapshotManager::Initialize()
{
    EnumerateWindows();
    LOG("共 %d 个窗口\n", (int)m_windows.size());
    m_onTrayUpdate(m_trayNumber);
}

// ---- 核心操作: 切换槽位 ----

void
SnapshotManager::SwitchTo(int slot)
{
    if (slot < 0 || slot > 9)
        return;
    if (slot == m_trayNumber)
        return;

    DWORD t0 = GetTickCount();

    EnumerateWindows();
    DWORD t1 = GetTickCount();
    LOG("[计时] 枚举窗口: %lu ms, 共 %zu 个\n", t1 - t0, m_windows.size());

    SyncDesktopState();
    DWORD t2 = GetTickCount();
    LOG("[计时] 桌面同步: %lu ms\n", t2 - t1);

    SaveCurrent();
    DWORD t3 = GetTickCount();
    LOG("[计时] 保存快照: %lu ms\n", t3 - t2);

    CaptureSlot(m_trayNumber);
    DWORD t4 = GetTickCount();
    LOG("[计时] 截图: %lu ms\n", t4 - t3);

    m_prevTrayNumber = m_trayNumber;
    m_trayNumber = slot;

    Restore(slot);
    DWORD t5 = GetTickCount();
    LOG("[计时] 恢复窗口: %lu ms\n", t5 - t4);

    m_onTrayUpdate(m_trayNumber);
    DWORD t6 = GetTickCount();
    LOG("[计时] 托盘图标: %lu ms\n", t6 - t5);

    LOG("[切换] %d -> %d (总耗时 %lu ms)\n", m_prevTrayNumber, slot, t6 - t0);
}

void
SnapshotManager::CaptureCurrent()
{
    CaptureSlot(m_trayNumber);
}

// ===================================================================
// 窗口枚举
// ===================================================================

void
SnapshotManager::EnumerateWindows()
{
    m_windows.clear();
    m_zOrderCounter = 0;
    EnumWindows(EnumCallback, (LPARAM)this);
}

BOOL CALLBACK
SnapshotManager::EnumCallback(HWND hwnd, LPARAM lParam)
{
    auto* self = (SnapshotManager*)lParam;
    return self->OnEnumWindow(hwnd);
}

BOOL
SnapshotManager::OnEnumWindow(HWND hwnd)
{
    if (ShouldSkip(hwnd))
        return TRUE;
    if (m_windows.size() >= kMaxWindows)
        return TRUE;
    WinInfo wi;
    FillWindowInfo(wi, hwnd);
    m_windows.push_back(wi);
    return TRUE;
}

BOOL
SnapshotManager::ShouldSkip(HWND hwnd)
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
    if (m_pDesktopMgr) {
        GUID desktopId;
        if (FAILED(m_pDesktopMgr->GetWindowDesktopId(hwnd, &desktopId))
            || IsEqualGUID(desktopId, GUID_NULL))
            return TRUE;
    }
    return FALSE;
}

void
SnapshotManager::FillWindowInfo(WinInfo& w, HWND hwnd)
{
    w.hwnd = hwnd;
    w.zOrder = m_zOrderCounter++;

    wchar_t wt[256];
    GetWindowTextW(hwnd, wt, 256);
    int len = WideCharToMultiByte(CP_UTF8, 0, wt, -1, NULL, 0, NULL, NULL);
    if (len > 0) {
        w.title.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, wt, -1, &w.title[0], len, NULL, NULL);
    }

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

// ===================================================================
// 保存当前快照
// ===================================================================

void
SnapshotManager::SaveCurrent()
{
    int num = m_trayNumber;
    Snapshot& snap = m_snapshots[num];
    snap.windows = m_windows;
    LOG("[快照] 保存 %d 个窗口到数字 %d\n", (int)snap.windows.size(), num);
    for (size_t i = 0; i < m_windows.size(); i++) {
        const auto& w = m_windows[i];
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

// ===================================================================
// 恢复快照
// ===================================================================

static const char*
CmdToStr(UINT cmd)
{
    return cmd == SW_MAXIMIZE   ? "最大化"
           : cmd == SW_MINIMIZE ? "最小化"
                                : "正常";
}

// ---- 显示器可见性检查 ----
// 多显→单显切换时，保存的窗口位置可能完全脱离当前显示器（比如副屏
// 上的窗口坐标 x>1920，副屏拔掉后该坐标对应区域不存在任何显示器）
static BOOL
RectIsOnScreen(const RECT& rect)
{
    // MONITOR_DEFAULTTONULL: 矩形完全脱离所有显示器时返回 NULL
    return MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != NULL;
}

static void
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

void
SnapshotManager::Restore(int num)
{
    if (num < 0 || num > 9)
        return;
    Snapshot& snap = m_snapshots[num];

    DWORD t0 = GetTickCount();

    auto& wins = snap.windows;

    // 窗口 ≤ 3 时跳过动画禁用：SystemParametersInfo 是系统级广播，
    // 对少量窗口来说广播开销比动画本身还大
    const bool skipAnim = wins.size() <= 3;
    ANIMATIONINFO ai = { sizeof(ANIMATIONINFO) };
    BOOL origAnimate = FALSE;
    if (!skipAnim) {
        if (SystemParametersInfo(SPI_GETANIMATION, sizeof(ai), &ai, 0))
            origAnimate = ai.iMinAnimate;
        if (origAnimate) {
            ai.iMinAnimate = 0;
            SystemParametersInfo(SPI_SETANIMATION, sizeof(ai), &ai, 0);
        }
    }
    DWORD t1 = GetTickCount();
    LOG("[计时]   动画禁用: %lu ms (skip=%d)\n", t1 - t0, skipAnim);

    // 将当前可见窗口中"不在目标快照里"的全部最小化
    // 用 unordered_set 替代 find_if 嵌套循环，O(N+M) 替代 O(N×M)
    std::unordered_set<HWND> snapSet;
    snapSet.reserve(wins.size());
    for (const auto& sw : wins)
        snapSet.insert(sw.hwnd);
    for (const auto& w : m_windows) {
        if (snapSet.find(w.hwnd) == snapSet.end())
            ShowWindow(w.hwnd, SW_MINIMIZE);
    }
    DWORD t2 = GetTickCount();
    LOG("[计时]   最小化无关窗口: %lu ms\n", t2 - t1);

#ifndef RELEASE
    LOG("[快照] 从数字 %d 恢复 %d 个窗口\n", num, (int)wins.size());
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

    // 第一步：恢复窗口状态（位置 + 最小化/最大化/还原）
    //
    // 将位置和 Z 序分成两步的原因：SetWindowPlacement 能原子地设置位置
    // 和显示状态但不控制 Z 序；DeferWindowPos 能批量调 Z 序但不支持
    // 显示状态变更（SW_MINIMIZE / SW_MAXIMIZE）
    //
    // 最小化→最大化跨进程窗口时，直接 SetWindowPlacement(SW_MAXIMIZE)
    // 可能渲染异常（只显示还原尺寸的左上角，其余透明），所以拆成两步
    // 先 SW_SHOWNOACTIVATE 还原，再 SW_SHOWMAXIMIZED 最大化
    //
    // 优化：先检测窗口是否已处于目标状态，命中则跳过 SetWindowPlacement。
    // 两次切回同一槽位时大部分窗口都未变动，白调 SetWindowPlacement
    // 是最大的单步开销（跨进程 SendMessage 阻塞等待目标窗口处理）
    int placed = 0, skipped = 0;
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
        BOOL zoomed = IsZoomed(w.hwnd);
        UINT curCmd;
        if (iconic)
            curCmd = SW_MINIMIZE;
        else if (zoomed)
            curCmd = SW_MAXIMIZE;
        else
            curCmd = SW_SHOWNORMAL;

        // 当前状态与目标一致 → 跳过
        if (curCmd == w.showCmd) {
            skipped++;
            continue;
        }
        placed++;

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
            // 两步还原：第一步只需现身 → ShowWindowAsync 非阻塞
            // 第二步需要设位置 → SetWindowPlacement 同步
            LOG("[恢复] [%zu] 两步还原: SW_SHOWNOACTIVATE -> %s\n",
                i,
                showCmdStr);
            ShowWindowAsync(w.hwnd, SW_SHOWNOACTIVATE);
            wp.showCmd = w.showCmd;
            SetWindowPlacement(w.hwnd, &wp);
        } else if (w.showCmd == SW_MAXIMIZE && !iconic) {
            // 已在另一显示器最大化；先还原到目标位置再最大化，实现跨屏移动
            LOG("[恢复] [%zu] 跨屏最大化: SW_SHOWNOACTIVATE -> SW_MAXIMIZE\n",
                i);
            ShowWindowAsync(w.hwnd, SW_SHOWNOACTIVATE);
            wp.showCmd = SW_MAXIMIZE;
            SetWindowPlacement(w.hwnd, &wp);
        } else if (w.showCmd == SW_MINIMIZE) {
            // 只需最小化，不关心位置 → ShowWindowAsync 非阻塞
            ShowWindowAsync(w.hwnd, SW_MINIMIZE);
        } else {
            wp.showCmd = w.showCmd;
            SetWindowPlacement(w.hwnd, &wp);
        }
    }
    LOG("[计时]   实际 SetWindowPlacement: %d 窗口, 跳过 %d\n", placed, skipped);
    DWORD t3 = GetTickCount();
    LOG("[计时]   恢复位置/状态 (%zu 窗口): %lu ms\n", wins.size(), t3 - t2);

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
    }
    DWORD t4 = GetTickCount();
    LOG("[计时]   恢复 Z 序: %lu ms\n", t4 - t3);

    // 恢复动画设置
    if (origAnimate) {
        ai.iMinAnimate = origAnimate;
        SystemParametersInfo(SPI_SETANIMATION, sizeof(ai), &ai, 0);
    }
    DWORD t5 = GetTickCount();
    LOG("[计时]   动画恢复: %lu ms\n", t5 - t4);

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
    LOG("[计时]   验证对比: %lu ms\n", GetTickCount() - t5);
#endif

    LOG("[恢复] 完成 (总耗时 %lu ms)\n", GetTickCount() - t0);
}

// ===================================================================
// 桌面状态同步
// ===================================================================
// 通过已枚举窗口探测桌面 GUID 并同步桌面状态
// EnumWindows 回调中 ShouldSkip 已用 GetWindowDesktopId 过滤，
// 故 m_windows 中每个窗口都有合法桌面 GUID，比 GetForegroundWindow 可靠

void
SnapshotManager::SyncDesktopState()
{
    GUID newId = GUID_NULL;
    if (m_pDesktopMgr && !m_windows.empty())
        m_pDesktopMgr->GetWindowDesktopId(m_windows[0].hwnd, &newId);

    if (IsEqualGUID(newId, GUID_NULL))
        return;

    if (IsEqualGUID(m_currentDesktopId, GUID_NULL)) {
        m_currentDesktopId = newId;
        return;
    }
    if (IsEqualGUID(m_currentDesktopId, newId))
        return;

    // 保存当前桌面状态
    DesktopState& oldState = m_desktopStates[m_currentDesktopId];
    oldState.snapshots = m_snapshots;
    oldState.trayNumber = m_trayNumber;
    oldState.prevTrayNumber = m_prevTrayNumber;

    LOG("[桌面] 离开桌面 (tray=%d)\n", m_trayNumber);

    // 加载新桌面状态
    auto it = m_desktopStates.find(newId);
    if (it != m_desktopStates.end()) {
        m_snapshots = it->second.snapshots;
        m_trayNumber = it->second.trayNumber;
        m_prevTrayNumber = it->second.prevTrayNumber;
        LOG("[桌面] 进入已知桌面 (tray=%d)\n", m_trayNumber);
    } else {
        m_snapshots = {};
        m_trayNumber = 0;
        m_prevTrayNumber = 0;
        LOG("[桌面] 进入新桌面\n");
    }
    m_currentDesktopId = newId;
    m_onTrayUpdate(m_trayNumber);
}

// ===================================================================
// 截屏
// ===================================================================

struct CapCtx {
    HDC hdcMem;
    HDC hdcScreen;
    int bmpW, bmpH;
    RECT workUnion; // 所有 rcWork 的并集
};

static BOOL CALLBACK
CapMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    CapCtx* c = (CapCtx*)lp;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hMon, &mi))
        return TRUE;
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

RECT
WorkAreaUnionGet()
{
    CapCtx ctx = { NULL, NULL };
    ctx.workUnion = { 0, 0, 0, 0 };
    EnumDisplayMonitors(NULL, NULL, CapMonitorProc, (LPARAM)&ctx);
    return ctx.workUnion;
}

void
SnapshotManager::CaptureSlot(int slot)
{
    Snapshot& snap = m_snapshots[slot];
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
