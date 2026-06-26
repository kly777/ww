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
#include <cstdio>


#include "snapshot_manager.h"

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

// ---- 全局 ----
static SnapshotManager* g_snapMgr = nullptr;
static IVirtualDesktopManager* g_pDesktopManager = NULL;

static HWND g_hWnd = NULL;
static FILE* g_logFile = NULL; // 文件日志句柄，仅 #ifndef RELEASE 有效

// ---- 工作区总览预览 ----
static HWND g_previewWnd = NULL;
static UINT g_previewTimer = 0;

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

// ---- 快照操作已迁移至 SnapshotManager (snapshot_manager.cpp) ----
// (SnapRestore, CmdToStr, DesktopStateSync, CapCtx, CapMonitorProc,
//  CapDrawProc, WorkAreaUnionGet, SnapCapture, SnapSwitch)

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
TrayIconUpdate(int number)
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
    nid.hWnd = g_hWnd;
    nid.uID = kIdTrayIcon;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;

    nid.hIcon = MakeTrayIcon(number);
    swprintf(nid.szTip, 128, L"ww-%d", number);

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
            if (g_snapMgr)
                g_snapMgr->SwitchTo(slot);
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
        const Snapshot& snap
                = g_snapMgr ? g_snapMgr->GetSnapshot(slot) : Snapshot{};

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

    int trayNum = g_snapMgr ? g_snapMgr->CurrentSlot() : 1;
    int curCol = (trayNum - 1) % kCols;
    int curRow = (trayNum - 1) / kCols;
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
    if (!g_snapMgr)
        return;
    // 先销毁旧窗口，再截取当前快照确保总览是最新状态
    if (g_previewWnd && IsWindow(g_previewWnd))
        DestroyWindow(g_previewWnd);
    g_snapMgr->CaptureCurrent();
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
        if (g_snapMgr)
            g_snapMgr->Initialize();
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
        if (!g_snapMgr)
            return 0;
        int id = (int)wParam;
        if (id == static_cast<int>(HotkeyId::Screenshot)) {
            CaptureAndShow();
            return 0;
        }
        int slot = id - static_cast<int>(HotkeyId::Digit);
        if (slot >= 0 && slot <= 9) {
            int cur = g_snapMgr->CurrentSlot();
            if (slot == cur) {
                int prev = g_snapMgr->PrevSlot();
                if (prev != cur)
                    g_snapMgr->SwitchTo(prev);
            } else {
                g_snapMgr->SwitchTo(slot);
            }
        } else {
            int dir = id - static_cast<int>(HotkeyId::ArrowUp);
            if (dir >= 0 && dir <= 3) {
                int cur = g_snapMgr->CurrentSlot();
                int target = kNavMap[cur][dir];
                if (target != cur)
                    g_snapMgr->SwitchTo(target);
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

    // 创建快照管理器 — 注入托盘更新回调和虚拟桌面管理器
    g_snapMgr = new SnapshotManager([](int num) { TrayIconUpdate(num); },
                                    g_pDesktopManager);

    if (!MessageWindowCreate(hInst)) {
        LOG("创建消息窗口失败\n");
        delete g_snapMgr;
        g_snapMgr = nullptr;
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

    delete g_snapMgr;
    g_snapMgr = nullptr;
    VirtualDesktopManagerCleanup();
#ifndef RELEASE
    if (g_logFile)
        fclose(g_logFile);
#endif
    return 0;
}
