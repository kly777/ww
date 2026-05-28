#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000007  // Windows 10 1709+

#include <dwmapi.h>
#include <ole2.h>      // StringFromCLSID
#include <shobjidl.h>  // IVirtualDesktopManager
#include <stdio.h>
#include <string.h>
#include <windows.h>

// ---- 宽字符转 UTF-8 ----
int WideToUtf8(const wchar_t* src, char* dst, int dstSize) {
    return WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstSize, NULL, NULL);
}

// ---- GUID 转字符串 ----
void GuidToString(const GUID& guid, char* out, int outSize) {
    wchar_t* wstr = NULL;
    if (SUCCEEDED(StringFromCLSID(guid, &wstr)) && wstr) {
        WideToUtf8(wstr, out, outSize);
        CoTaskMemFree(wstr);
    } else {
        snprintf(out, outSize, "未知");
    }
}

// ---- COM 全局对象 ----
static IVirtualDesktopManager* g_pDesktopManager = NULL;

// ---- 初始化 ----
BOOL InitVirtualDesktopManager() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        printf("COM 初始化失败: 0x%08lX\n", hr);
        return FALSE;
    }

    hr = CoCreateInstance(CLSID_VirtualDesktopManager, NULL,
                          CLSCTX_INPROC_SERVER, IID_IVirtualDesktopManager,
                          (void**)&g_pDesktopManager);

    if (FAILED(hr) || !g_pDesktopManager) {
        printf("VirtualDesktopManager 创建失败 (需要 Windows 10+): 0x%08lX\n",
               hr);
        return FALSE;
    }
    return TRUE;
}

// ---- 清理 ----
void CleanupVirtualDesktopManager() {
    if (g_pDesktopManager) {
        g_pDesktopManager->Release();
        g_pDesktopManager = NULL;
    }
    CoUninitialize();
}

// ---- 窗口枚举回调 ----
BOOL CALLBACK EnumWindowCallback(HWND hwnd, LPARAM lParam) {
    wchar_t windowTitle[256];
    wchar_t className[256];

    GetWindowTextW(hwnd, windowTitle, 256);
    GetClassNameW(hwnd, className, 256);

    if (IsWindowVisible(hwnd) && wcslen(windowTitle) > 0) {
        // 排除不在虚拟桌面上的窗口（桌面 GUID 为零或获取不到）
        if (g_pDesktopManager) {
            GUID desktopId;
            if (FAILED(
                    g_pDesktopManager->GetWindowDesktopId(hwnd, &desktopId)) ||
                IsEqualGUID(desktopId, GUID_NULL)) {
                return TRUE;  // 跳过该窗口，继续枚举
            }
        }

        char titleUtf8[512];
        char classUtf8[512];
        WideToUtf8(windowTitle, titleUtf8, 512);
        WideToUtf8(className, classUtf8, 512);

        printf("==================== 窗口详细信息 ====================\n");
        printf("窗口句柄: 0x%p\n", hwnd);
        printf("窗口标题: %s\n", titleUtf8);
        printf("窗口类名: %s\n", classUtf8);

        // ---- 虚拟桌面信息 ----
        printf("--- 虚拟桌面 ---\n");
        if (g_pDesktopManager) {
            // 是否在当前桌面
            BOOL onCurrent = FALSE;
            g_pDesktopManager->IsWindowOnCurrentVirtualDesktop(hwnd,
                                                               &onCurrent);
            printf("在当前虚拟桌面: %s\n", onCurrent ? "是" : "否");

            // 窗口所在桌面 GUID
            GUID windowDesktopId;
            if (SUCCEEDED(g_pDesktopManager->GetWindowDesktopId(
                    hwnd, &windowDesktopId))) {
                char guidStr[128];
                GuidToString(windowDesktopId, guidStr, sizeof(guidStr));
                printf("所在桌面: %s\n", guidStr);
            } else {
                printf("所在桌面: 获取失败\n");
            }

            // 当前桌面 GUID
            GUID currentDesktopId;
            if (SUCCEEDED(g_pDesktopManager->GetWindowDesktopId(
                    NULL, &currentDesktopId))) {
                char guidStr[128];
                GuidToString(currentDesktopId, guidStr, sizeof(guidStr));
                printf("当前桌面: %s\n", guidStr);
            }
        } else {
            printf("虚拟桌面API不可用\n");
        }

        // 窗口状态
        BOOL isVisible = IsWindowVisible(hwnd);
        BOOL isEnabled = IsWindowEnabled(hwnd);
        BOOL isIconic = IsIconic(hwnd);
        BOOL isZoomed = IsZoomed(hwnd);
        BOOL isActive = (GetForegroundWindow() == hwnd);

        printf("--- 窗口状态 ---\n");
        printf("可见: %s\n", isVisible ? "是" : "否");
        printf("可用: %s\n", isEnabled ? "是" : "否");
        printf("最小化: %s\n", isIconic ? "是" : "否");
        printf("最大化: %s\n", isZoomed ? "是" : "否");
        printf("前台窗口: %s\n", isActive ? "是" : "否");

        // 窗口位置和大小
        RECT rect;
        GetWindowRect(hwnd, &rect);
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;

        printf("--- 位置和大小 ---\n");
        printf("位置: (%d, %d) - (%d, %d)\n", rect.left, rect.top, rect.right,
               rect.bottom);
        printf("大小: %d x %d\n", width, height);

        // 客户区大小
        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        printf("客户区大小: %d x %d\n", clientRect.right - clientRect.left,
               clientRect.bottom - clientRect.top);
    }

    return TRUE;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    if (!InitVirtualDesktopManager()) {
        printf("无法初始化虚拟桌面API，继续枚举窗口...\n\n");
    }

    printf("开始枚举所有窗口...\n\n");
    EnumWindows(EnumWindowCallback, 0);

    CleanupVirtualDesktopManager();
    return 0;
}
