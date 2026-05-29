#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000007  // Windows 10 1709+

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <shobjidl.h>
#include <ole2.h>

#define MAX_WINDOWS 256

// ---- 窗口信息结构体 ----
struct WindowInfo {
    HWND hwnd;                  // 窗口句柄
    UINT  zOrder;               // Z-order 序号（0 为最前）
    char  title[256];           // 窗口标题 (UTF-8)
    char  className[256];       // 窗口类名 (UTF-8)

    // 窗口状态
    BOOL  isVisible;
    BOOL  isEnabled;
    BOOL  isIconic;
    BOOL  isZoomed;
    BOOL  isActive;             // 是否为前台窗口

    // 位置和大小
    RECT  windowRect;           // 窗口矩形
    RECT  clientRect;           // 客户区矩形

    // 虚拟桌面
    BOOL  onCurrentDesktop;
    GUID  desktopId;
};

// ---- 全局 ----
static WindowInfo g_windows[MAX_WINDOWS];
static int g_windowCount = 0;
static UINT g_zOrderCounter = 0;
static IVirtualDesktopManager* g_pDesktopManager = NULL;

// ---- 工具函数 ----
int WideToUtf8(const wchar_t* src, char* dst, int dstSize) {
    return WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstSize, NULL, NULL);
}

void GuidToString(const GUID& guid, char* out, int outSize) {
    wchar_t* wstr = NULL;
    if (SUCCEEDED(StringFromCLSID(guid, &wstr)) && wstr) {
        WideToUtf8(wstr, out, outSize);
        CoTaskMemFree(wstr);
    } else {
        snprintf(out, outSize, "未知");
    }
}

// ---- COM 初始化 ----
BOOL InitVirtualDesktopManager() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return FALSE;

    hr = CoCreateInstance(CLSID_VirtualDesktopManager, NULL, CLSCTX_INPROC_SERVER,
                          IID_IVirtualDesktopManager, (void**)&g_pDesktopManager);
    return SUCCEEDED(hr) && g_pDesktopManager;
}

void CleanupVirtualDesktopManager() {
    if (g_pDesktopManager) { g_pDesktopManager->Release(); g_pDesktopManager = NULL; }
    CoUninitialize();
}

// ---- 填充 WindowInfo ----
void FillWindowInfo(WindowInfo& w, HWND hwnd) {
    memset(&w, 0, sizeof(w));
    w.hwnd = hwnd;
    w.zOrder = g_zOrderCounter++;

    // 标题和类名
    wchar_t wTitle[256], wClass[256];
    GetWindowTextW(hwnd, wTitle, 256);
    GetClassNameW(hwnd, wClass, 256);
    WideToUtf8(wTitle, w.title, sizeof(w.title));
    WideToUtf8(wClass, w.className, sizeof(w.className));

    // 状态
    w.isVisible = IsWindowVisible(hwnd);
    w.isEnabled = IsWindowEnabled(hwnd);
    w.isIconic  = IsIconic(hwnd);
    w.isZoomed  = IsZoomed(hwnd);
    w.isActive  = (GetForegroundWindow() == hwnd);

    // 位置和大小
    GetWindowRect(hwnd, &w.windowRect);
    GetClientRect(hwnd, &w.clientRect);

    // 虚拟桌面
    if (g_pDesktopManager) {
        g_pDesktopManager->IsWindowOnCurrentVirtualDesktop(hwnd, &w.onCurrentDesktop);
        g_pDesktopManager->GetWindowDesktopId(hwnd, &w.desktopId);
    }
}

// ---- 枚举回调 ----
BOOL CALLBACK EnumWindowCallback(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t title[256];
    GetWindowTextW(hwnd, title, 256);
    if (wcslen(title) == 0) return TRUE;

    // 排除不在虚拟桌面上的窗口
    if (g_pDesktopManager) {
        GUID desktopId;
        if (FAILED(g_pDesktopManager->GetWindowDesktopId(hwnd, &desktopId))
            || IsEqualGUID(desktopId, GUID_NULL)) {
            return TRUE;
        }
    }

    if (g_windowCount >= MAX_WINDOWS) return TRUE;

    FillWindowInfo(g_windows[g_windowCount], hwnd);
    g_windowCount++;
    return TRUE;
}

// ---- 输出单个窗口信息 ----
void PrintWindowInfo(const WindowInfo& w) {
    printf("==================== 窗口详细信息 ====================\n");
    printf("窗口句柄: 0x%p\n", w.hwnd);
    printf("Z-Order:  %u\n", w.zOrder);
    printf("窗口标题: %s\n", w.title);
    printf("窗口类名: %s\n", w.className);

    printf("--- 虚拟桌面 ---\n");
    printf("在当前虚拟桌面: %s\n", w.onCurrentDesktop ? "是" : "否");
    char guidStr[128];
    GuidToString(w.desktopId, guidStr, sizeof(guidStr));
    printf("所在桌面: %s\n", guidStr);

    printf("--- 窗口状态 ---\n");
    printf("可见: %s\n", w.isVisible ? "是" : "否");
    printf("可用: %s\n", w.isEnabled ? "是" : "否");
    printf("最小化: %s\n", w.isIconic ? "是" : "否");
    printf("最大化: %s\n", w.isZoomed ? "是" : "否");
    printf("前台窗口: %s\n", w.isActive ? "是" : "否");

    printf("--- 位置和大小 ---\n");
    printf("位置: (%d, %d) - (%d, %d)\n",
           w.windowRect.left, w.windowRect.top,
           w.windowRect.right, w.windowRect.bottom);
    printf("大小: %d x %d\n",
           w.windowRect.right - w.windowRect.left,
           w.windowRect.bottom - w.windowRect.top);
    printf("客户区大小: %d x %d\n",
           w.clientRect.right - w.clientRect.left,
           w.clientRect.bottom - w.clientRect.top);

    printf("==================== 结束 ====================\n\n");
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    InitVirtualDesktopManager();

    printf("开始枚举所有窗口...\n\n");
    EnumWindows(EnumWindowCallback, 0);

    printf("共 %d 个窗口\n\n", g_windowCount);

    for (int i = 0; i < g_windowCount; i++) {
        PrintWindowInfo(g_windows[i]);
    }

    CleanupVirtualDesktopManager();
    return 0;
}
