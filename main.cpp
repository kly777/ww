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
#include <string>
#include <vector>

constexpr int kMaxWindows = 256;
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_INIT_TRAY = WM_APP + 2;  // 延迟初始化托盘
constexpr UINT kIdTrayIcon = 1;
enum class HotkeyId : int { Base = 0 };
enum class MenuId : int { AutoStart = 1001, Exit = 1000 };

#ifdef RELEASE
#define LOG(fmt, ...) ((void)0)
#else
#define LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#endif

// ---- 窗口 / 快照 ----
// WinInfo 存 hwnd，一次运行期间不变，恢复时直接按句柄定位，无需标题/类名/路径匹配
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
static int g_trayNumber = 1;                  // 托盘显示的数字 0-9
static int g_prevTrayNumber = 0;              // 上一个数字，Ctrl+N 再按时切回
static BOOL g_trayAdded = FALSE;              // 是否已 NIM_ADD
static std::array<Snapshot, 10> g_snapshots;  // 每个数字的快照
static HWND g_hWnd = NULL;
static HINSTANCE g_hInst = NULL;

// ---- 工具函数 ----
std::string WideToUtf8(const wchar_t* src) {
    int len = WideCharToMultiByte(CP_UTF8, 0, src, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return {};
    std::string result(len - 1, '\0');  // len includes null terminator
    WideCharToMultiByte(CP_UTF8, 0, src, -1, &result[0], len, NULL, NULL);
    return result;
}

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
    // 问题：最小化窗口的 GetWindowRect 返回 (-32000,-32000) 等垃圾坐标
    // 解决：GetWindowPlacement 的 rcNormalPosition 始终返回正常（非最小化）位置
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
    if (_wcsicmp(wclass, L"Progman") == 0) return TRUE;
    if (g_pDesktopManager) {
        GUID desktopId;
        if (FAILED(g_pDesktopManager->GetWindowDesktopId(hwnd, &desktopId)) ||
            IsEqualGUID(desktopId, GUID_NULL))
            return TRUE;
    }
    return FALSE;
}

// ---- 枚举回调 ----
BOOL CALLBACK EnumWindowCallback(HWND hwnd, LPARAM lParam) {
    if (ShouldSkipWindow(hwnd)) return TRUE;
    if (g_windows.size() >= kMaxWindows) return TRUE;
    WinInfo wi;
    FillWindowInfo(wi, hwnd);
    g_windows.push_back(wi);
    return TRUE;
}

// ---- 保存快照 ----
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

// ---- 恢复快照 ----
void RestoreSnapshot(int num) {
    if (num < 0 || num > 9) return;
    Snapshot& snap = g_snapshots[num];
    if (!snap.hasData) {
        LOG("[快照] 数字 %d 无快照，跳过恢复\n", num);
        return;
    }

    // 最小化当前窗口中不属于目标快照的窗口
    for (const auto& w : g_windows) {
        auto it = std::find_if(snap.windows.begin(),
                               snap.windows.end(),
                               [&](const WinInfo& sw) {
                                   return sw.hwnd == w.hwnd;
                               });
        if (it == snap.windows.end()) {
            ShowWindow(w.hwnd, SW_MINIMIZE);
        }
    }

    auto& wins = snap.windows;
    LOG("[快照] 从数字 %d 恢复 %d 个窗口\n", num, (int)wins.size());

    // 第一步: 恢复窗口状态(位置+最小化/最大化/还原), 避免 ShowWindow
    //         即时生效打乱 Z 序, 统一用 SetWindowPlacement 原子操作
    for (const auto& w : wins) {
        if (!IsWindow(w.hwnd)) continue;
        WINDOWPLACEMENT wp = {sizeof(WINDOWPLACEMENT)};
        wp.showCmd = w.showCmd;
        wp.rcNormalPosition = w.rect;
        // ptMinPosition / ptMaxPosition 置零，由系统自己计算
        SetWindowPlacement(w.hwnd, &wp);
    }

    // 第二步: 按 zOrder 恢复 Z 序
    std::sort(wins.begin(), wins.end(),
              [](const WinInfo& a, const WinInfo& b) {
                  return a.zOrder < b.zOrder;
              });

    if (!wins.empty()) {
        HDWP hdwp = BeginDeferWindowPos((int)wins.size());
        if (hdwp) {
            HWND after = HWND_BOTTOM;
            for (const auto& w : wins) {
                if (!IsWindow(w.hwnd)) continue;
                // 位置和状态已由 SetWindowPlacement 设置, 这里只修 Z 序
                UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
                hdwp = DeferWindowPos(hdwp, w.hwnd, after, 0, 0, 0, 0, flags);
                if (!hdwp) break;
                after = w.hwnd;
            }
            if (hdwp) EndDeferWindowPos(hdwp);
        }
    }

    LOG("[恢复] 完成\n");
}

// ---- 切换工作区 ----
void UpdateTrayIcon();
void SwitchSnapshot(int slot) {
    if (slot < 0 || slot > 9) return;
    // 再次按同一数字 → 回到上一个 snapshot
    if (slot == g_trayNumber) {
        slot = g_prevTrayNumber;
        if (slot == g_trayNumber) return;
    }

    // 先重新枚举当前窗口状态
    g_windows.clear();
    g_zOrderCounter = 0;
    EnumWindows(EnumWindowCallback, 0);

    // 保存当前状态到旧数字的快照
    SaveSnapshot(g_trayNumber, g_windows);

    // 切换到新数字
    g_prevTrayNumber = g_trayNumber;
    g_trayNumber = slot;

    // 恢复新数字的快照
    RestoreSnapshot(slot);

    UpdateTrayIcon();
    LOG("[切换] %d -> %d\n", g_prevTrayNumber, slot);
}

// ---- 动态生成托盘图标 ----
HICON MakeTrayIcon(int number) {
    int w = 32, h = 32;

    HDC hdc = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(hdc);

    // 颜色位图 (32-bit, top-down)
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

    HBITMAP hOld = (HBITMAP)SelectObject(memDC, hBmpColor);

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
    HBRUSH hBrBg = CreateSolidBrush(kColors[number]);
    FillRect(memDC, &rc, hBrBg);
    DeleteObject(hBrBg);

    // 白色正方形边框
    int margin = 0;
    HPEN hPn = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HBRUSH hBrNull = (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN hPnOld = (HPEN)SelectObject(memDC, hPn);
    HBRUSH hBrOld = (HBRUSH)SelectObject(memDC, hBrNull);
    Rectangle(memDC, margin, margin, w - margin, h - margin);
    SelectObject(memDC, hPnOld);
    SelectObject(memDC, hBrOld);
    DeleteObject(hPn);

    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));
    wchar_t num[2] = {(wchar_t)(L'0' + number), 0};
    HFONT hFont = CreateFontW(34, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT hFontOld = (HFONT)SelectObject(memDC, hFont);
    SIZE sz;
    GetTextExtentPoint32W(memDC, num, 1, &sz);
    TEXTMETRIC tm;
    GetTextMetrics(memDC, &tm);
    int y = (h - tm.tmAscent) / 2 - 5;
    TextOutW(memDC, (w - sz.cx) / 2, y, num, 1);
    SelectObject(memDC, hFontOld);
    DeleteObject(hFont);
    SelectObject(memDC, hOld);

    if (bits) {
        for (int i = 0; i < w * h; i++) ((BYTE*)bits)[i * 4 + 3] = 0xFF;
    }

    // 掩码: 全白 = 全部不透明
    HBITMAP hBmpMask = CreateBitmap(w, h, 1, 1, NULL);
    HDC maskDC = CreateCompatibleDC(hdc);
    HBITMAP hOldM = (HBITMAP)SelectObject(maskDC, hBmpMask);
    FillRect(maskDC, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SelectObject(maskDC, hOldM);
    DeleteDC(maskDC);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = hBmpColor;
    ii.hbmMask = hBmpMask;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hBmpColor);
    DeleteObject(hBmpMask);
    DeleteDC(memDC);
    ReleaseDC(NULL, hdc);
    return hIcon;
}

// ---- 更新托盘图标 ----
void UpdateTrayIcon() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hWnd;
    nid.uID = kIdTrayIcon;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;

    if (!g_trayAdded) {
        nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
        nid.hIcon = MakeTrayIcon(g_trayNumber);
        swprintf(nid.szTip, 128, L"ww-%d", g_trayNumber);
        Shell_NotifyIconW(NIM_ADD, &nid);
        if (nid.hIcon) DestroyIcon(nid.hIcon);
        g_trayAdded = TRUE;
    } else {
        nid.hIcon = MakeTrayIcon(g_trayNumber);
        swprintf(nid.szTip, 128, L"ww-%d", g_trayNumber);
        if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
            LOG("[!] NIM_MODIFY 失败: %lu\n", GetLastError());
        if (nid.hIcon) DestroyIcon(nid.hIcon);
    }
}

// ---- 注册热键 ----
void RegisterHotkeys(HWND hwnd) {
    // Ctrl+0 ~ Ctrl+9
    for (int i = 0; i <= 9; i++) {
        RegisterHotKey(hwnd, static_cast<int>(HotkeyId::Base) + i,
                       MOD_CONTROL | MOD_NOREPEAT, '0' + i);
    }
}

void UnregisterHotkeys(HWND hwnd) {
    for (int i = 0; i <= 9; i++) {
        UnregisterHotKey(hwnd, static_cast<int>(HotkeyId::Base) + i);
    }
}

// ---- 开机启动 ----
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

// ---- 窗口过程 ----
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            RegisterHotkeys(hwnd);
            PostMessage(hwnd, WM_INIT_TRAY, 0, 0);  // 延迟到消息循环启动
            return 0;
        }

        case WM_INIT_TRAY: {
            UpdateTrayIcon();
            return 0;
        }

        case WM_DESTROY: {
            UnregisterHotkeys(hwnd);
            NOTIFYICONDATAW nid = {};
            nid.cbSize = sizeof(nid);
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
                // 右键菜单
                HMENU hMenu = CreatePopupMenu();
                // AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(hMenu,
                            MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0),
                            (UINT_PTR)MenuId::AutoStart, L"开机启动");
                AppendMenuW(hMenu, MF_STRING, (UINT_PTR)MenuId::Exit, L"退出");

                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);  // 确保菜单能正常关闭
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
    SetProcessDPIAware();  // 修复高 DPI 模糊
#ifndef RELEASE
    SetConsoleOutputCP(CP_UTF8);
#endif
    g_hInst = GetModuleHandle(NULL);

    // COM 初始化
    InitVirtualDesktopManager();

    // 枚举窗口
    EnumWindows(EnumWindowCallback, 0);
    LOG("共 %d 个窗口\n", (int)g_windows.size());

    // 托盘 + 热键
    if (!CreateMessageWindow(g_hInst)) {
        LOG("创建消息窗口失败\n");
        CleanupVirtualDesktopManager();
        return 1;
    }

    LOG("\n=== 托盘图标已创建 ===\n");
    LOG("Ctrl+0~Ctrl+9 切换托盘数字 | 右键托盘图标选择数字或退出\n\n");

    // 消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CleanupVirtualDesktopManager();
    return 0;
}
