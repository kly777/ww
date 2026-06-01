// 请求 Windows 10+ API (IVirtualDesktopManager 等)
// 不加这三个宏，windows.h 不会暴露 SHCreateItemFromParsingName 等接口
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000007

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
enum class HotkeyId : int { Base = 0 };
enum class MenuId : int { AutoStart = 1001, Exit = 1000 };

#ifdef RELEASE
#define LOG(fmt, ...) ((void)0)
#else
#define LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#endif

struct WinInfo {
    HWND hwnd;
    std::string title;  // 仅用于日志
    RECT rect;
    UINT showCmd;  // SW_SHOWNORMAL / SW_MINIMIZE / SW_MAXIMIZE
    UINT zOrder;
};

struct Snapshot {
    BOOL hasData = FALSE;
    std::vector<WinInfo> windows;
};

// ---- 全局 ----
static std::vector<WinInfo> g_windows;
static UINT g_zOrderCounter = 0;
static IVirtualDesktopManager* g_pDesktopManager = NULL;
static int g_trayNumber = 1;  // 托盘显示的数字 0-9
static int g_prevTrayNumber = 0;

static std::array<Snapshot, 10> g_snapshots;  // 当前桌面的快照

// 每个虚拟桌面独立维护 10 槽快照 + 托盘状态
struct GuidLess {
    bool operator()(const GUID& a, const GUID& b) const {
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

// ---- 工具函数 ----
std::string WideToUtf8(const wchar_t* src) {
    int len = WideCharToMultiByte(CP_UTF8, 0, src, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return {};
    std::string result(len - 1, '\0');  // len 含 null terminator
    WideCharToMultiByte(CP_UTF8, 0, src, -1, &result[0], len, NULL, NULL);
    return result;
}

// IVirtualDesktopManager 要求 STA 线程模型，所以用
// COINIT_APARTMENTTHREADED MTA 下 CoCreateInstance 会返回
// CO_E_NOTINITIALIZED
BOOL InitVirtualDesktopManager() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return FALSE;
    hr = CoCreateInstance(CLSID_VirtualDesktopManager, NULL,
                          CLSCTX_INPROC_SERVER, IID_IVirtualDesktopManager,
                          (void**)&g_pDesktopManager);
    return SUCCEEDED(hr) && g_pDesktopManager;
}

void CleanupVirtualDesktopManager() {
    if (g_pDesktopManager) {
        g_pDesktopManager->Release();
        g_pDesktopManager = NULL;
    }
    CoUninitialize();
}

// ---- 捕捉窗口状态 ----
void FillWindowInfo(WinInfo& w, HWND hwnd) {
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
    WINDOWPLACEMENT wp = {sizeof(WINDOWPLACEMENT)};
    GetWindowPlacement(hwnd, &wp);
    w.rect = wp.rcNormalPosition;
    LOG("[枚举] \"%s\" iconic=%d zoomed=%d rect=(%ld,%ld,%ld,%ld) %ldx%ld\n",
        w.title.c_str(), iconic, zoomed, w.rect.left, w.rect.top, w.rect.right,
        w.rect.bottom, w.rect.right - w.rect.left, w.rect.bottom - w.rect.top);
}

// ---- 窗口过滤 ----
BOOL ShouldSkipWindow(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[256], wclass[256];
    GetWindowTextW(hwnd, title, 256);
    if (wcslen(title) == 0) return TRUE;
    GetClassNameW(hwnd, wclass, 256);
    // 跳过桌面窗口本身 (Progman)
    if (_wcsicmp(wclass, L"Progman") == 0) return TRUE;
    // 如果存在其他虚拟桌面，只枚举当前桌面的窗口
    if (g_pDesktopManager) {
        GUID desktopId;
        if (FAILED(g_pDesktopManager->GetWindowDesktopId(hwnd, &desktopId)) ||
            IsEqualGUID(desktopId, GUID_NULL))
            return TRUE;
    }
    return FALSE;
}

BOOL CALLBACK EnumWindowCallback(HWND hwnd, LPARAM lParam) {
    if (ShouldSkipWindow(hwnd)) return TRUE;
    if (g_windows.size() >= kMaxWindows) return TRUE;
    WinInfo wi;
    FillWindowInfo(wi, hwnd);
    g_windows.push_back(wi);
    return TRUE;
}

void SaveSnapshot(int num, const std::vector<WinInfo>& windows) {
    if (num < 0 || num > 9) return;
    Snapshot& snap = g_snapshots[num];
    snap.windows = windows;
    snap.hasData = TRUE;
    LOG("[快照] 保存 %d 个窗口到数字 %d\n", (int)snap.windows.size(), num);
    for (size_t i = 0; i < windows.size(); i++) {
        const auto& w = windows[i];
        LOG("[保存] [%zu] \"%s\" showCmd=%u rect=(%ld,%ld,%ld,%ld) %ldx%ld\n",
            i, w.title.c_str(), w.showCmd, w.rect.left, w.rect.top,
            w.rect.right, w.rect.bottom, w.rect.right - w.rect.left,
            w.rect.bottom - w.rect.top);
    }
}

// ---- 显示器可见性检查 ----
// 多显→单显切换时，保存的窗口位置可能完全脱离当前显示器（比如副屏
// 上的窗口坐标 x>1920，副屏拔掉后该坐标对应区域不存在任何显示器）
BOOL IsRectOnScreen(const RECT& rect) {
    // MONITOR_DEFAULTTONULL: 矩形完全脱离所有显示器时返回 NULL
    return MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != NULL;
}

void EnsureRectVisible(RECT& rect, int width, int height) {
    if (IsRectOnScreen(rect)) return;
    // 窗口完全脱离所有显示器，移到主显示器居中 用 rcWork 而非 rcMonitor
    // 避开任务栏区域
    HMONITOR hPrimary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(MONITORINFO)};
    GetMonitorInfo(hPrimary, &mi);
    const RECT& work = mi.rcWork;
    rect.left = work.left + ((work.right - work.left) - width) / 2;
    rect.top = work.top + ((work.bottom - work.top) - height) / 2;
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    LOG("[修复] 窗口移到主显示器 (%ld,%ld,%ld,%ld)\n", rect.left, rect.top,
        rect.right, rect.bottom);
}

// ---- 恢复快照 ----
void RestoreSnapshot(int num) {
    if (num < 0 || num > 9) return;
    Snapshot& snap = g_snapshots[num];
    if (!snap.hasData) {
        LOG("[快照] 数字 %d 无快照，跳过恢复\n", num);
        return;
    }

    // 临时禁用窗口最小化/还原动画，让批量恢复像瞬间切换而非逐个动画
    ANIMATIONINFO ai = {sizeof(ANIMATIONINFO)};
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
        auto it =
            std::find_if(snap.windows.begin(), snap.windows.end(),
                         [&](const WinInfo& sw) { return sw.hwnd == w.hwnd; });
        if (it == snap.windows.end()) {
            ShowWindow(w.hwnd, SW_MINIMIZE);
        }
    }

    LOG("[快照] 从数字 %d 恢复 %d 个窗口\n", num, (int)wins.size());

    // Version 2: 使用 SetWindowPlacement 直接设置位置和显示状态
    // 第一步：恢复窗口状态（位置 + 最小化/最大化/还原）
    //
    // 将位置和 Z 序分成两步的原因：SetWindowPlacement 能原子地设置位置
    // 和显示状态但不控制 Z 序；DeferWindowPos 能批量调 Z 序但不支持
    // 显示状态变更（SW_MINIMIZE / SW_MAXIMIZE）
    //
    // 最小化→最大化跨进程窗口时，直接 SetWindowPlacement(SW_MAXIMIZE)
    // 可能渲染异常（只显示还原尺寸的左上角，其余透明），所以拆成两步
    // 先 SW_SHOWNOACTIVATE 还原，再 ShowWindow 最大化
    // for (size_t i = 0; i < wins.size(); i++) {
    //     const auto& w = wins[i];
    //     if (!IsWindow(w.hwnd)) {
    //         LOG("[恢复] [%zu] 窗口已销毁，跳过", i);
    //         continue;
    //     }
    //     RECT r = w.rect;
    //     long ww = r.right - r.left;
    //     long wh = r.bottom - r.top;
    //     EnsureRectVisible(r, ww, wh);

    //     BOOL iconic = IsIconic(w.hwnd);
    //     const char* showCmdStr = w.showCmd == SW_MAXIMIZE   ? "最大化"
    //                              : w.showCmd == SW_MINIMIZE ? "最小化"
    //                                                         : "正常";
    //     LOG("[恢复] [%zu] \"%s\" iconic=%d -> %s rect=(%ld,%ld,%ld,%ld) "
    //         "%ldx%ld",
    //         i, w.title.c_str(), iconic, showCmdStr, r.left, r.top, r.right,
    //         r.bottom, ww, wh);

    //     WINDOWPLACEMENT wp = {sizeof(WINDOWPLACEMENT)};
    //     wp.rcNormalPosition = r;
    //     if (w.showCmd != SW_MINIMIZE && iconic) {
    //         LOG("[恢复] [%zu] 两步还原: SW_SHOWNOACTIVATE -> %s", i,
    //             showCmdStr);
    //         wp.showCmd = SW_SHOWNOACTIVATE;
    //         SetWindowPlacement(w.hwnd, &wp);
    //         // ShowWindow(w.hwnd, w.showCmd);
    //     } else {
    //         wp.showCmd = w.showCmd;
    //         SetWindowPlacement(w.hwnd, &wp);
    //     }
    // }
    //
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

    std::sort(wins.begin(), wins.end(), [](const WinInfo& a, const WinInfo& b) {
        return a.zOrder < b.zOrder;
    });
    // Now: 先还原，再应用目标状态
    for (const auto& w : wins) {
        if (!IsWindow(w.hwnd)) continue;

        // 如果窗口当前是最小化，先还原
        if (IsIconic(w.hwnd) && w.showCmd != SW_MINIMIZE) {
            ShowWindow(w.hwnd, SW_RESTORE);
        }

        // 然后设置目标状态
        if (w.showCmd == SW_MAXIMIZE) {
            ShowWindow(w.hwnd, SW_SHOWMAXIMIZED);
        } else if (w.showCmd == SW_MINIMIZE) {
            ShowWindow(w.hwnd, SW_SHOWMINIMIZED);
        } else {
            // 正常状态：可能需要调整位置
            WINDOWPLACEMENT wp = {sizeof(WINDOWPLACEMENT)};
            wp.showCmd = SW_SHOWNORMAL;
            wp.rcNormalPosition = w.rect;
            SetWindowPlacement(w.hwnd, &wp);
        }

    }

    // 第二步：按 zOrder 恢复 Z 序
    // 排序后从 HWND_BOTTOM 开始逐个往上叠，恢复原始前后关系

    if (!wins.empty()) {
        HDWP hdwp = BeginDeferWindowPos((int)wins.size());
        if (hdwp) {
            HWND after = HWND_TOP;
            for (const auto& w : wins) {
                if (!IsWindow(w.hwnd)) continue;
                // SWP_NOMOVE | SWP_NOSIZE: 位置和大小已由 SetWindowPlacement
                // 设置好了，这里只修 Z 序
                // SWP_NOACTIVATE: 不改变当前活动窗口，避免抢焦点
                UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
                hdwp = DeferWindowPos(hdwp, w.hwnd, after, 0, 0, 0, 0, flags);
                if (!hdwp) break;
                after = w.hwnd;
            }
            if (hdwp) EndDeferWindowPos(hdwp);
        }
    }

    // 恢复动画设置
    if (origAnimate) {
        ai.iMinAnimate = origAnimate;
        SystemParametersInfo(SPI_SETANIMATION, sizeof(ai), &ai, 0);
    }

    LOG("[恢复] 完成\n");
}

// ---- 切换工作区 ----
void UpdateTrayIcon();
// 通过已枚举窗口探测桌面 GUID 并同步桌面状态
// EnumWindows 回调中 ShouldSkipWindow 已用 GetWindowDesktopId 过滤，
// 故 g_windows 中每个窗口都有合法桌面 GUID，比 GetForegroundWindow 可靠
static void SyncDesktopState() {
    GUID newId = GUID_NULL;
    if (g_pDesktopManager && !g_windows.empty())
        g_pDesktopManager->GetWindowDesktopId(g_windows[0].hwnd, &newId);

    // 无法确定桌面 GUID（g_windows 为空或虚拟桌面管理器不可用）
    if (IsEqualGUID(newId, GUID_NULL)) return;

    // 首次运行
    if (IsEqualGUID(g_currentDesktopId, GUID_NULL)) {
        g_currentDesktopId = newId;
        return;
    }
    if (IsEqualGUID(g_currentDesktopId, newId)) return;

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
    UpdateTrayIcon();
}

// 再次按同一数字 → 回到上一个快照，实现 Ctrl+N 双击在最近两个工作区间切换
void SwitchSnapshot(int slot) {
    if (slot < 0 || slot > 9) return;

    g_windows.clear();
    g_zOrderCounter = 0;
    EnumWindows(EnumWindowCallback, 0);

    SyncDesktopState();

    if (slot == g_trayNumber) {
        slot = g_prevTrayNumber;
        if (slot == g_trayNumber) return;
    }

    SaveSnapshot(g_trayNumber, g_windows);

    g_prevTrayNumber = g_trayNumber;
    g_trayNumber = slot;

    RestoreSnapshot(slot);

    UpdateTrayIcon();
    LOG("[切换] %d -> %d\n", g_prevTrayNumber, slot);
}

// ---- 生成托盘图标 ----
HICON MakeTrayIcon(int number) {
    int w = 32, h = 32;

    HDC hdc = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(hdc);

    // biHeight 为负值 = top-down DIB，此时位图数据从顶行开始排列
    // 简化后面 alpha 通道填充的寻址（bits[0] 就是第一行第一个像素）
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HBITMAP hBmpColor =
        CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(memDC, hBmpColor);

    static const COLORREF kColors[] = {
        RGB(180, 50, 50),   // 0 红
        RGB(30, 100, 210),  // 1 蓝
        RGB(40, 150, 70),   // 2 绿
        RGB(200, 140, 20),  // 3 橙
        RGB(130, 60, 180),  // 4 紫
        RGB(20, 150, 150),  // 5 青
        RGB(200, 80, 140),  // 6 粉
        RGB(80, 80, 80),    // 7 灰
        RGB(30, 140, 210),  // 8 天蓝
        RGB(180, 160, 30),  // 9 金
    };
    RECT rc = {0, 0, w, h};

    // ---- 绘制 ----
    HBRUSH hBrBg = CreateSolidBrush(kColors[number]);
    FillRect(memDC, &rc, hBrBg);

    HPEN hPn = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HBRUSH hBrNull = (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN hOldPn = (HPEN)SelectObject(memDC, hPn);
    HBRUSH hOldBr = (HBRUSH)SelectObject(memDC, hBrNull);
    Rectangle(memDC, 0, 0, w, h);

    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));
    wchar_t num[2] = {(wchar_t)(L'0' + number), 0};
    HFONT hFont = CreateFontW(34, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT hOldFont = (HFONT)SelectObject(memDC, hFont);
    SIZE sz;
    GetTextExtentPoint32W(memDC, num, 1, &sz);
    TEXTMETRIC tm;
    GetTextMetrics(memDC, &tm);
    int y = (h - tm.tmAscent) / 2 - 5;
    TextOutW(memDC, (w - sz.cx) / 2, y, num, 1);

    // GDI 对象在选入 DC 时不能删除 必须先逐一 SelectObject 恢复原始
    // 对象（按入栈反序），之后再 DeleteObject 才安全
    SelectObject(memDC, hOldFont);
    SelectObject(memDC, hOldBr);
    SelectObject(memDC, hOldPn);
    SelectObject(memDC, hOldBmp);

    DeleteObject(hFont);
    DeleteObject(hPn);
    DeleteObject(hBrBg);

    // 填充 alpha 通道为不透明：CreateDIBSection 不会初始化像素数据，
    // 背景填充只写了 RGB 没写 A，这里补上
    if (bits) {
        for (int i = 0; i < w * h; i++) ((BYTE*)bits)[i * 4 + 3] = 0xFF;
    }

    // 掩码位图：全白代表图标完全不透明 CreateIconIndirect 同时需要
    // 颜色位图和掩码合成最终图标
    HBITMAP hBmpMask = CreateBitmap(w, h, 1, 1, NULL);
    HDC maskDC = CreateCompatibleDC(hdc);
    HBITMAP hOldMask = (HBITMAP)SelectObject(maskDC, hBmpMask);
    FillRect(maskDC, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SelectObject(maskDC, hOldMask);
    DeleteDC(maskDC);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = hBmpColor;
    ii.hbmMask = hBmpMask;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hBmpMask);
    DeleteObject(hBmpColor);
    DeleteDC(memDC);
    ReleaseDC(NULL, hdc);
    return hIcon;
}

static BOOL g_trayAdded = FALSE;
// ---- 更新托盘图标 ----
void UpdateTrayIcon() {
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
        if (nid.hIcon) DestroyIcon(nid.hIcon);
        g_trayAdded = TRUE;
    } else {
        if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
            LOG("[!] NIM_MODIFY 失败: %lu\n", GetLastError());
        if (nid.hIcon) DestroyIcon(nid.hIcon);
    }
}

void RegisterHotkeys(HWND hwnd) {
    // MOD_NOREPEAT: 按住不放只触发一次
    for (int i = 0; i <= 9; i++) {
        if (!RegisterHotKey(hwnd, static_cast<int>(HotkeyId::Base) + i,
                            MOD_CONTROL | MOD_NOREPEAT, '0' + i)) {
            LOG("[!] 注册热键 Ctrl+%d 失败 (错误码: %lu)\n", i, GetLastError());
        }
    }
}

void UnregisterHotkeys(HWND hwnd) {
    for (int i = 0; i <= 9; i++) {
        UnregisterHotKey(hwnd, static_cast<int>(HotkeyId::Base) + i);
    }
}

// ---- 开机启动 ----
// 通过 HKCU\...\Run 注册表项实现，系统启动时自动拉起 ww.exe
BOOL IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;
    wchar_t path[MAX_PATH];
    DWORD size = sizeof(path);
    DWORD type;
    LSTATUS ret =
        RegQueryValueExW(hKey, L"WW", NULL, &type, (BYTE*)path, &size);
    RegCloseKey(hKey);
    return ret == ERROR_SUCCESS;
}

void SetAutoStart(BOOL enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        RegSetValueExW(hKey, L"WW", 0, REG_SZ, (BYTE*)path,
                       (DWORD)(wcslen(path) + 1) * sizeof(wchar_t));
    } else {
        RegDeleteValueW(hKey, L"WW");
    }
    RegCloseKey(hKey);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            RegisterHotkeys(hwnd);
            PostMessage(hwnd, WM_INIT_TRAY, 0, 0);
            return 0;
        }

        case WM_INIT_TRAY: {
            EnumWindows(EnumWindowCallback, 0);
            LOG("共 %d 个窗口\n", (int)g_windows.size());
            UpdateTrayIcon();
            return 0;
        }

        case WM_DESTROY: {
            UnregisterHotkeys(hwnd);
            NOTIFYICONDATAW nid = {};
            nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
            nid.hWnd = hwnd;
            nid.uID = kIdTrayIcon;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
            return 0;
        }

        case WM_HOTKEY: {
            int key = (int)wParam - static_cast<int>(HotkeyId::Base);
            if (key >= 0 && key <= 9) SwitchSnapshot(key);
            return 0;
        }

        case WM_TRAYICON: {
            if (lParam == WM_RBUTTONUP) {
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu,
                            MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0),
                            (UINT_PTR)MenuId::AutoStart, L"开机启动");
                AppendMenuW(hMenu, MF_STRING, (UINT_PTR)MenuId::Exit, L"退出");

                POINT pt;
                GetCursorPos(&pt);
                // 必须先 SetForegroundWindow，否则 TrackPopupMenu 弹出后
                // 点击菜单外区域不会自动关闭（Windows 前台窗口规则）
                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                                         pt.x, pt.y, 0, hwnd, NULL);
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
BOOL CreateMessageWindow(HINSTANCE hInstance) {
    const wchar_t* CLASS_NAME = L"WW_TrayWindow";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    g_hWnd = CreateWindowExW(0, CLASS_NAME, L"WW", WS_POPUP, 0, 0, 0, 0, NULL,
                             NULL, hInstance, NULL);
    return g_hWnd != NULL;
}

int main() {
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

    InitVirtualDesktopManager();

    if (!CreateMessageWindow(hInst)) {
        LOG("创建消息窗口失败\n");
        CleanupVirtualDesktopManager();
        return 1;
    }

    LOG("\n=== 托盘图标已创建 ===\n");
    LOG("Ctrl+0~Ctrl+9 切换托盘数字 | 右键托盘图标选择数字或退出\n\n");

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CleanupVirtualDesktopManager();
    return 0;
}
