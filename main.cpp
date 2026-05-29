#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000007

#include <ole2.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdio.h>
#include <windows.h>

#include <array>
#include <string>
#include <vector>

#define MAX_WINDOWS 256
#define WM_TRAYICON (WM_APP + 1)
#define WM_INIT_TRAY (WM_APP + 2)  // 延迟初始化托盘
#define ID_TRAYICON 1
#define ID_HOTKEY_BASE 100

#ifdef RELEASE
#define LOG(fmt, ...) ((void)0)
#else
#define LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#endif

// ---- 窗口信息结构体 ----
struct WindowInfo {
    HWND hwnd;
    UINT zOrder;
    std::string title;
    std::string className;
    std::string processPath;
    UINT showCmd;  // SW_SHOWNORMAL / SW_MINIMIZE / SW_MAXIMIZE
    BOOL isVisible, isEnabled, isIconic, isZoomed, isActive;
    RECT windowRect, clientRect;
    BOOL onCurrentDesktop;
    GUID desktopId;
};

// ---- 快照结构体 ----
struct SnapWindow {
    std::string title;
    std::string className;
    std::string processPath;
    RECT rect;
    UINT showCmd;
};

struct Snapshot {
    BOOL hasData = FALSE;
    std::vector<SnapWindow> windows;
};

// ---- 全局 ----
static std::vector<WindowInfo> g_windows;
static UINT g_zOrderCounter = 0;
static IVirtualDesktopManager* g_pDesktopManager = NULL;
static int g_trayNumber = 0;      // 托盘显示的数字 0-9
static BOOL g_trayAdded = FALSE;  // 是否已 NIM_ADD
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

std::string GuidToString(const GUID& guid) {
    wchar_t* wstr = NULL;
    if (SUCCEEDED(StringFromCLSID(guid, &wstr)) && wstr) {
        std::string result = WideToUtf8(wstr);
        CoTaskMemFree(wstr);
        return result;
    }
    return "未知";
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

// ---- 填充 WindowInfo ----
void FillWindowInfo(WindowInfo& w, HWND hwnd) {
    w.hwnd = hwnd;
    w.zOrder = g_zOrderCounter++;

    wchar_t wTitle[256], wClass[256];
    GetWindowTextW(hwnd, wTitle, 256);
    GetClassNameW(hwnd, wClass, 256);
    w.title = WideToUtf8(wTitle);
    w.className = WideToUtf8(wClass);

    w.isVisible = IsWindowVisible(hwnd);
    w.isEnabled = IsWindowEnabled(hwnd);
    w.isIconic = IsIconic(hwnd);
    w.isZoomed = IsZoomed(hwnd);
    w.isActive = (GetForegroundWindow() == hwnd);

    // showCmd
    if (w.isIconic)
        w.showCmd = SW_MINIMIZE;
    else if (w.isZoomed)
        w.showCmd = SW_MAXIMIZE;
    else
        w.showCmd = SW_SHOWNORMAL;

    GetWindowRect(hwnd, &w.windowRect);
    GetClientRect(hwnd, &w.clientRect);

    // 进程路径（用于快照匹配）
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (hp) {
        wchar_t pp[MAX_PATH];
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(hp, 0, pp, &sz))
            w.processPath = WideToUtf8(pp);
        CloseHandle(hp);
    }

    if (g_pDesktopManager) {
        g_pDesktopManager->IsWindowOnCurrentVirtualDesktop(hwnd,
                                                           &w.onCurrentDesktop);
        g_pDesktopManager->GetWindowDesktopId(hwnd, &w.desktopId);
    }
}

// ---- 枚举回调 ----
BOOL CALLBACK EnumWindowCallback(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[256];
    GetWindowTextW(hwnd, title, 256);
    if (wcslen(title) == 0) return TRUE;
    if (g_pDesktopManager) {
        GUID desktopId;
        if (FAILED(g_pDesktopManager->GetWindowDesktopId(hwnd, &desktopId)) ||
            IsEqualGUID(desktopId, GUID_NULL))
            return TRUE;
    }
    if (g_windows.size() >= MAX_WINDOWS) return TRUE;
    WindowInfo wi;
    FillWindowInfo(wi, hwnd);
    g_windows.push_back(wi);
    return TRUE;
}

// ---- 输出 ----
void PrintWindowInfo(const WindowInfo& w) {
    LOG("==================== 窗口详细信息 ====================\n");
    LOG("窗口句柄: 0x%p\n", w.hwnd);
    LOG("Z-Order:  %u\n", w.zOrder);
    LOG("窗口标题: %s\n", w.title.c_str());
    LOG("窗口类名: %s\n", w.className.c_str());

    LOG("--- 虚拟桌面 ---\n");
    LOG("在当前虚拟桌面: %s\n", w.onCurrentDesktop ? "是" : "否");
    std::string guidStr = GuidToString(w.desktopId);
    LOG("所在桌面: %s\n", guidStr.c_str());

    LOG("--- 窗口状态 ---\n");
    LOG("可见: %s\n", w.isVisible ? "是" : "否");
    LOG("可用: %s\n", w.isEnabled ? "是" : "否");
    LOG("最小化: %s\n", w.isIconic ? "是" : "否");
    LOG("最大化: %s\n", w.isZoomed ? "是" : "否");
    LOG("前台窗口: %s\n", w.isActive ? "是" : "否");

    LOG("--- 位置和大小 ---\n");
    LOG("位置: (%ld, %ld) - (%ld, %ld)\n", w.windowRect.left, w.windowRect.top,
        w.windowRect.right, w.windowRect.bottom);
    LOG("大小: %ld x %ld\n", w.windowRect.right - w.windowRect.left,
        w.windowRect.bottom - w.windowRect.top);
    LOG("客户区大小: %ld x %ld\n", w.clientRect.right - w.clientRect.left,
        w.clientRect.bottom - w.clientRect.top);
    LOG("==================== 结束 ====================\n\n");
}

// ---- 快照：保存/恢复窗口状态 ----
void SaveSnapshot(int num, const std::vector<WindowInfo>& windows) {
    if (num < 0 || num > 9) return;
    Snapshot& snap = g_snapshots[num];
    int count = (int)windows.size();
    snap.windows.resize(count);
    for (int i = 0; i < count; i++) {
        SnapWindow& sw = snap.windows[i];
        sw.title = windows[i].title;
        sw.className = windows[i].className;
        sw.processPath = windows[i].processPath;
        sw.rect = windows[i].windowRect;
        sw.showCmd = windows[i].showCmd;
    }
    snap.hasData = TRUE;
    LOG("[快照] 保存 %d 个窗口到数字 %d\n", (int)snap.windows.size(), num);
}

// ---- 快照恢复时用的临时结构 ----
struct CurWin {
    HWND hwnd;
    std::string title;
    std::string className;
    std::string processPath;
};

BOOL CALLBACK CollectCurWindows(HWND hwnd, LPARAM lParam) {
    auto* wins = (std::vector<CurWin>*)lParam;
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t wt[256], wc[256];
    GetWindowTextW(hwnd, wt, 256);
    if (wcslen(wt) == 0) return TRUE;
    GetClassNameW(hwnd, wc, 256);
    if (wins->size() >= MAX_WINDOWS) return TRUE;

    CurWin cw;
    cw.title = WideToUtf8(wt);
    cw.className = WideToUtf8(wc);

    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (hp) {
        wchar_t pp[MAX_PATH];
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(hp, 0, pp, &sz))
            cw.processPath = WideToUtf8(pp);
        CloseHandle(hp);
    }
    cw.hwnd = hwnd;
    wins->push_back(cw);
    return TRUE;
}

void RestoreSnapshot(int num) {
    if (num < 0 || num > 9) return;
    Snapshot& snap = g_snapshots[num];
    if (!snap.hasData) {
        LOG("[快照] 数字 %d 无快照，跳过恢复\n", num);
        return;
    }

    LOG("[快照] 从数字 %d 恢复 %d 个窗口\n", num, (int)snap.windows.size());

    std::vector<CurWin> curWindows;
    curWindows.reserve(MAX_WINDOWS);
    EnumWindows(CollectCurWindows, (LPARAM)&curWindows);

    // 匹配并恢复
    for (size_t i = 0; i < snap.windows.size(); i++) {
        SnapWindow& sw = snap.windows[i];
        for (size_t j = 0; j < curWindows.size(); j++) {
            if (sw.title == curWindows[j].title &&
                sw.className == curWindows[j].className &&
                sw.processPath == curWindows[j].processPath) {
                HWND hwnd = curWindows[j].hwnd;
                // 先恢复状态（非最小化/最大化则用 SW_RESTORE）
                UINT cmd = sw.showCmd;
                if (cmd == SW_SHOWNORMAL) cmd = SW_RESTORE;
                ShowWindow(hwnd, cmd);

                // 恢复到保存的位置和大小
                int w = sw.rect.right - sw.rect.left;
                int h = sw.rect.bottom - sw.rect.top;
                SetWindowPos(hwnd, NULL, sw.rect.left, sw.rect.top, w, h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                break;
            }
        }
    }
}

// ---- 切换数字（保存旧快照 + 恢复新快照）----
void UpdateTrayIcon();  // 前置声明
void SwitchToNumber(int newNum) {
    if (newNum < 0 || newNum > 9 || newNum == g_trayNumber) return;

    // 先重新枚举当前窗口状态
    g_windows.clear();
    g_zOrderCounter = 0;
    EnumWindows(EnumWindowCallback, 0);

    // 保存当前状态到旧数字的快照
    SaveSnapshot(g_trayNumber, g_windows);

    // 切换到新数字
    int oldNum = g_trayNumber;
    g_trayNumber = newNum;

    // 恢复新数字的快照
    RestoreSnapshot(newNum);

    UpdateTrayIcon();
    LOG("[切换] %d -> %d\n", oldNum, newNum);
}

// ---- 动态生成带数字的托盘图标 ----
HICON CreateNumberIcon(int number) {
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

    // 蓝色背景
    RECT rc = {0, 0, w, h};
    HBRUSH hBrBg = CreateSolidBrush(RGB(30, 100, 210));
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

    // 白色数字
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

    // 设置 alpha 通道为 255 (不透明)
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
    nid.uID = ID_TRAYICON;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;

    if (!g_trayAdded) {
        nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
        nid.hIcon = CreateNumberIcon(g_trayNumber);
        swprintf(nid.szTip, 128, L"数字: %d", g_trayNumber);
        Shell_NotifyIconW(NIM_ADD, &nid);
        if (nid.hIcon) DestroyIcon(nid.hIcon);
        g_trayAdded = TRUE;
    } else {
        nid.hIcon = CreateNumberIcon(g_trayNumber);
        swprintf(nid.szTip, 128, L"数字: %d", g_trayNumber);
        if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
            LOG("[!] NIM_MODIFY 失败: %lu\n", GetLastError());
        if (nid.hIcon) DestroyIcon(nid.hIcon);
    }
}

// ---- 注册热键 ----
void RegisterHotkeys(HWND hwnd) {
    // Ctrl+0 ~ Ctrl+9
    for (int i = 0; i <= 9; i++) {
        RegisterHotKey(hwnd, ID_HOTKEY_BASE + i, MOD_CONTROL | MOD_NOREPEAT,
                       '0' + i);
    }
}

void UnregisterHotkeys(HWND hwnd) {
    for (int i = 0; i <= 9; i++) {
        UnregisterHotKey(hwnd, ID_HOTKEY_BASE + i);
    }
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
            nid.uID = ID_TRAYICON;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
            return 0;
        }

        case WM_HOTKEY: {
            int key = (int)wParam - ID_HOTKEY_BASE;
            if (key >= 0 && key <= 9) SwitchToNumber(key);
            return 0;
        }

        case WM_TRAYICON: {
            if (lParam == WM_RBUTTONUP) {
                // 右键菜单
                HMENU hMenu = CreatePopupMenu();
                for (int i = 0; i <= 9; i++) {
                    wchar_t item[32];
                    swprintf(item, 32, L"数字 %d", i);
                    AppendMenuW(hMenu, MF_STRING, ID_HOTKEY_BASE + i, item);
                }
                AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(hMenu, MF_STRING, 1000, L"退出");

                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);  // 确保菜单能正常关闭
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                                         pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);

                if (cmd == 1000) {
                    DestroyWindow(hwnd);
                } else if (cmd >= ID_HOTKEY_BASE && cmd <= ID_HOTKEY_BASE + 9) {
                    SwitchToNumber(cmd - ID_HOTKEY_BASE);
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

// ---- 主函数 ----
int main() {
    SetProcessDPIAware();  // 修复高 DPI 模糊
#ifndef RELEASE
    SetConsoleOutputCP(CP_UTF8);
#endif
    g_hInst = GetModuleHandle(NULL);

    // COM 初始化
    InitVirtualDesktopManager();

    // 枚举窗口
    LOG("开始枚举所有窗口...\n\n");
    EnumWindows(EnumWindowCallback, 0);
    LOG("共 %d 个窗口\n\n", (int)g_windows.size());
    for (size_t i = 0; i < g_windows.size(); i++) {
        PrintWindowInfo(g_windows[i]);
    }

    // 初始快照保存到数字 0
    SaveSnapshot(0, g_windows);

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
