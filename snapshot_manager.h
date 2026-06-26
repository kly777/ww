#pragma once

#include <windows.h>

// 前向声明 — 实现文件负责完整定义
struct IVirtualDesktopManager;

#include <array>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

constexpr int kMaxWindows = 256;

// ---- 快照数据 ----

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
    RECT capUnion = { 0, 0, 0, 0 }; // 截屏时的 rcWork 并集
};

// ---- 虚拟桌面状态 ----

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

// ---- 工具函数 ----
// 由 snapshot_manager.cpp 实现，供 main.cpp 的 OverviewBuild 使用
RECT WorkAreaUnionGet();

// ---- SnapshotManager ----

class SnapshotManager {
public:
    // 托盘图标更新回调: 传入新的槽位号
    using TrayUpdater = std::function<void(int)>;

    SnapshotManager(TrayUpdater onTrayUpdate,
                    IVirtualDesktopManager* pDesktopMgr = nullptr);
    ~SnapshotManager();

    // 禁止拷贝
    SnapshotManager(const SnapshotManager&) = delete;
    SnapshotManager& operator=(const SnapshotManager&) = delete;

    // ---- 对外接口 ----
    void Initialize();  // 首次枚举窗口 + 通知托盘
    void SwitchTo(int slot); // 保存当前快照并切换到目标槽位

    int CurrentSlot() const { return m_trayNumber; }
    int PrevSlot() const { return m_prevTrayNumber; }

    const Snapshot& GetSnapshot(int slot) const { return m_snapshots[slot]; }
    const std::array<Snapshot, 10>& Snapshots() const { return m_snapshots; }

    void CaptureCurrent(); // 截屏当前槽位 (Alt+S 预览用)

private:
    TrayUpdater m_onTrayUpdate;
    IVirtualDesktopManager* m_pDesktopMgr;

    std::array<Snapshot, 10> m_snapshots;
    int m_trayNumber = 1;
    int m_prevTrayNumber = 0;

    // 窗口枚举缓冲区
    std::vector<WinInfo> m_windows;
    UINT m_zOrderCounter = 0;

    // 虚拟桌面状态持久化
    std::map<GUID, DesktopState, GuidLess> m_desktopStates;
    GUID m_currentDesktopId = GUID_NULL;

    // ---- 内部操作 ----
    void EnumerateWindows();
    void SaveCurrent();
    void Restore(int slot);
    void CaptureSlot(int slot);
    void SyncDesktopState();

    // EnumWindows 桥接
    static BOOL CALLBACK EnumCallback(HWND hwnd, LPARAM lParam);
    BOOL OnEnumWindow(HWND hwnd);
    BOOL ShouldSkip(HWND hwnd);
    void FillWindowInfo(WinInfo& w, HWND hwnd);
};
