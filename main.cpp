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

// ---- 窗口 / 快照结构体 ----
// 合并了原 WindowInfo 和 SnapWindow，统一为 WinInfo
// zOrder: 用于保存/恢复窗口堆叠顺序（EnumWindows 从上到下枚举，0=前台）
struct WinInfo {
    std::string title;
    std::string className;
    std::string processPath;
    RECT rect;
    UINT showCmd;  // SW_SHOWNORMAL / SW_MINIMIZE / SW_MAXIMIZE
    UINT zOrder;
};

struct Snapshot {
    BOOL hasData = FALSE;
    std::vector<WinInfo> windows;
};

// ---- 全局 ----
static std::vector<WinInfo> g_windows;  // 用 vector 替代原始数组，自动管理内存
static UINT g_zOrderCounter = 0;
static IVirtualDesktopManager* g_pDesktopManager = NULL;
static int g_trayNumber = 1;      // 托盘显示的数字 0-9
static BOOL g_trayAdded = FALSE;  // 是否已 NIM_ADD
static std::array<Snapshot, 10> g_snapshots;  // 每个数字的快照
static HWND g_hWnd = NULL;
static HINSTANCE g_hInst = NULL;

// ---- 工具函数 ----
// 原来的 WideToUtf8 写入 char* 缓冲区，改为返回 std::string，避免缓冲区溢出风险
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

// 获取窗口所属进程的路径
std::string GetProcessPath(HWND hwnd) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hp) return {};
    wchar_t pp[MAX_PATH];
    DWORD sz = MAX_PATH;
    std::string path;
    if (QueryFullProcessImageNameW(hp, 0, pp, &sz))
        path = WideToUtf8(pp);
    CloseHandle(hp);
    return path;
}

// ---- 填充 WinInfo ----
void FillWindowInfo(WinInfo& w, HWND hwnd) {
    w.zOrder = g_zOrderCounter++;

    wchar_t wTitle[256], wClass[256];
    GetWindowTextW(hwnd, wTitle, 256);
    GetClassNameW(hwnd, wClass, 256);
    w.title = WideToUtf8(wTitle);
    w.className = WideToUtf8(wClass);

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
    LOG("[枚举] \"%s\" iconic=%d zoomed=%d rect=(%ld,%ld,%ld,%ld) %dx%d\n",
        w.title.c_str(), iconic, zoomed,
        w.rect.left, w.rect.top, w.rect.right,
        w.rect.bottom,
        w.rect.right - w.rect.left,
        w.rect.bottom - w.rect.top);

    w.processPath = GetProcessPath(hwnd);
}

// ---- 窗口过滤（保存和恢复共用）----
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

// ---- 枚举回调（保存快照用）----
BOOL CALLBACK EnumWindowCallback(HWND hwnd, LPARAM lParam) {
    if (ShouldSkipWindow(hwnd)) return TRUE;
    if (g_windows.size() >= MAX_WINDOWS) return TRUE;
    WinInfo wi;
    FillWindowInfo(wi, hwnd);
    g_windows.push_back(wi);
    return TRUE;
}

// ---- 快照：保存/恢复窗口状态 ----
// 保存时直接拷贝 g_windows 向量（WinInfo 已含所有必要字段）
void SaveSnapshot(int num, const std::vector<WinInfo>& windows) {
    if (num < 0 || num > 9) return;
    Snapshot& snap = g_snapshots[num];
    snap.windows = windows;
    snap.hasData = TRUE;
    LOG("[快照] 保存 %d 个窗口到数字 %d\n", (int)snap.windows.size(), num);
    for (size_t i = 0; i < windows.size(); i++) {
        const auto& w = windows[i];
        LOG("[保存] [%zu] \"%s\" showCmd=%u rect=(%ld,%ld,%ld,%ld) %dx%d\n",
            i, w.title.c_str(), w.showCmd, w.rect.left, w.rect.top,
            w.rect.right, w.rect.bottom,
            w.rect.right - w.rect.left, w.rect.bottom - w.rect.top);
    }
}

// ---- 快照恢复时用的临时结构 ----
struct CurWin {
    HWND hwnd;
    std::string title;
    std::string className;
    std::string processPath;
};

// CollectCurWindows: 恢复时枚举当前窗口，与 EnumWindowCallback 共用 ShouldSkipWindow
BOOL CALLBACK CollectCurWindows(HWND hwnd, LPARAM lParam) {
    auto* wins = (std::vector<CurWin>*)lParam;
    if (ShouldSkipWindow(hwnd)) return TRUE;
    if (wins->size() >= MAX_WINDOWS) return TRUE;

    CurWin cw;
    wchar_t wt[256], wc[256];
    GetWindowTextW(hwnd, wt, 256);
    GetClassNameW(hwnd, wc, 256);
    cw.title = WideToUtf8(wt);
    cw.className = WideToUtf8(wc);
    cw.processPath = GetProcessPath(hwnd);
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
    LOG("[恢复] 当前可见窗口 %d 个\n", (int)curWindows.size());

    std::vector<bool> matched(curWindows.size(), false);

    // 收集匹配窗口，按 zOrder 排序后批量恢复位置/Z轴
    struct MatchEntry {
        HWND hwnd;
        UINT zOrder;
        RECT rect;
        UINT showCmd;
    };
    std::vector<MatchEntry> restored;

    for (size_t i = 0; i < snap.windows.size(); i++) {
        WinInfo& sw = snap.windows[i];
        LOG("[恢复] 快照[%zu]: \"%s\" zOrder=%u showCmd=%u rect=(%ld,%ld,%ld,%ld)\n",
            i, sw.title.c_str(), sw.zOrder, sw.showCmd, sw.rect.left,
            sw.rect.top, sw.rect.right, sw.rect.bottom);
        for (size_t j = 0; j < curWindows.size(); j++) {
            if (sw.title == curWindows[j].title &&
                sw.className == curWindows[j].className &&
                sw.processPath == curWindows[j].processPath) {
                matched[j] = true;
                restored.push_back(
                    {curWindows[j].hwnd, sw.zOrder, sw.rect, sw.showCmd});
                break;
            }
        }
    }

    // zOrder 排序说明：
    // EnumWindows 从上到下（前台→后台）枚举，zOrder 小=前台，大=后台
    // 升序排列后从 HWND_BOTTOM 逐层堆叠：小 zOrder（前台）最后放 → 留在顶层
    std::sort(restored.begin(), restored.end(),
              [](const MatchEntry& a, const MatchEntry& b) {
                  return a.zOrder < b.zOrder;
              });

    // 用 DeferWindowPos 一次原子操作完成所有窗口的位置、大小、显示、Z 轴设置
    // 避免 SetWindowPlacement + SetWindowPos 两次重绘导致的闪烁
    if (!restored.empty()) {
        HDWP hdwp = BeginDeferWindowPos((int)restored.size());
        if (hdwp) {
            HWND after = HWND_BOTTOM;
            for (const auto& e : restored) {
                UINT flags = SWP_NOACTIVATE;
                int x = 0, y = 0, w = 0, h = 0;
                if (e.showCmd == SW_MINIMIZE) {
                    flags |= SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW;
                } else if (e.showCmd == SW_MAXIMIZE) {
                    flags |= SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW;
                } else {
                    x = e.rect.left; y = e.rect.top;
                    w = e.rect.right - e.rect.left;
                    h = e.rect.bottom - e.rect.top;
                    flags |= SWP_SHOWWINDOW;
                }
                hdwp = DeferWindowPos(hdwp, e.hwnd, after, x, y, w, h, flags);
                if (!hdwp) break;
                after = e.hwnd;
            }
            if (hdwp) EndDeferWindowPos(hdwp);
        }
    }

    // DeferWindowPos 无法最大化，单独调用 ShowWindow（此时位置/Z轴已就位）
    for (const auto& e : restored) {
        if (e.showCmd == SW_MAXIMIZE)
            ShowWindow(e.hwnd, SW_MAXIMIZE);
        else if (e.showCmd == SW_MINIMIZE)
            ShowWindow(e.hwnd, SW_MINIMIZE);
    }

    // 快照中没有匹配到的窗口 → 最小化
    for (size_t j = 0; j < curWindows.size(); j++) {
        if (!matched[j]) {
            LOG("[恢复] 最小化未匹配: \"%s\" hwnd=0x%p\n",
                curWindows[j].title.c_str(), curWindows[j].hwnd);
            ShowWindow(curWindows[j].hwnd, SW_MINIMIZE);
        }
    }

    LOG("[恢复] 完成\n");
}

// ---- 切换数字（保存旧快照 + 恢复新快照）----
// 切换前先枚举当前窗口状态并保存到旧槽位，再从新槽位恢复
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

// Makefile 中需加 -static 静态链接 libstdc++/libgcc，避免 clock_gettime64 符号缺失
// g++ -static -mwindows -DRELEASE -O2 -s -o ww.exe main.cpp -lole32 -luuid -lshell32 -lgdi32
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

    // 初始快照保存到数字 1（默认从 1 开始，g_trayNumber 初始化为 1）
    SaveSnapshot(1, g_windows);

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
