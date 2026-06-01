import os
import random
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Optional

import psutil
import win32api
import win32con
import win32gui
import pytest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WW_EXE = os.path.join(REPO_ROOT, "build", "ww.exe")
assert os.path.isfile(WW_EXE), f"ww.exe not found at {WW_EXE}"

VK_CONTROL = 0x11
VK_0 = 0x30
WAIT_PROCESS = 0.5
WAIT_HOTKEY = 0.5
WAIT_WINDOW = 0.5


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

@dataclass
class WinInfo:
    hwnd: int
    title: str
    cls: str
    left: int
    top: int
    right: int
    bottom: int
    show_cmd: int

    @property
    def width(self) -> int:
        return self.right - self.left

    @property
    def height(self) -> int:
        return self.bottom - self.top

    @property
    def rect(self):
        return self.left, self.top, self.right, self.bottom


def find_ww_process() -> Optional[psutil.Process]:
    for proc in psutil.process_iter(["pid", "name"]):
        try:
            if proc.info["name"] and proc.info["name"].lower() == "ww.exe":
                return proc
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    return None


def find_ww_hwnd() -> Optional[int]:
    """Find ww hidden message window by class name WW_TrayWindow."""
    found = [0]

    def callback(hwnd, _):
        try:
            cls = win32gui.GetClassName(hwnd)
        except Exception:
            return True
        if cls == "WW_TrayWindow":
            found[0] = hwnd
            return False
        return True

    for _ in range(3):
        try:
            win32gui.EnumWindows(callback, 0)
            break
        except Exception:
            time.sleep(0.3)
    return found[0] if found[0] else None


def kill_process(proc):
    if proc and proc.is_running():
        proc.kill()
        proc.wait(timeout=5)


def press_ctrl_number(num: int):
    assert 0 <= num <= 9
    vk = VK_0 + num
    win32api.keybd_event(VK_CONTROL, 0, 0, 0)
    time.sleep(0.05)
    win32api.keybd_event(vk, 0, 0, 0)
    time.sleep(0.05)
    win32api.keybd_event(vk, 0, win32con.KEYEVENTF_KEYUP, 0)
    time.sleep(0.05)
    win32api.keybd_event(VK_CONTROL, 0, win32con.KEYEVENTF_KEYUP, 0)
    time.sleep(WAIT_HOTKEY)


def enum_visible_windows(title_sub: str = "") -> list[WinInfo]:
    results: list[WinInfo] = []

    def callback(hwnd, _):
        if not win32gui.IsWindowVisible(hwnd):
            return True
        title = win32gui.GetWindowText(hwnd)
        if not title:
            return True
        if title_sub and title_sub.lower() not in title.lower():
            return True
        try:
            left, top, right, bottom = win32gui.GetWindowRect(hwnd)
        except Exception:
            return True
        placement = win32gui.GetWindowPlacement(hwnd)
        cls_name = win32gui.GetClassName(hwnd) or ""
        results.append(
            WinInfo(
                hwnd=hwnd,
                title=title,
                cls=cls_name,
                left=left,
                top=top,
                right=right,
                bottom=bottom,
                show_cmd=placement[1],
            )
        )
        return True

    win32gui.EnumWindows(callback, None)
    return results


def launch_notepad() -> subprocess.Popen:
    proc = subprocess.Popen(["notepad.exe"])
    time.sleep(WAIT_WINDOW)
    return proc


def close_notepad(proc=None, hwnd=None):
    if hwnd:
        win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
    if proc:
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


NOTEPAD_TITLES = [
    "无标题 - 记事本",
    "Untitled - Notepad",
    "*无标题 - 记事本",
    "*Untitled - Notepad",
]


def wait_for_notepad(wait_count=20) -> Optional[int]:
    for _ in range(wait_count):
        for win in enum_visible_windows():
            if win.cls == "Notepad":
                return win.hwnd
            for t in NOTEPAD_TITLES:
                if win.title.strip() == t:
                    return win.hwnd
        time.sleep(0.3)
    return None


# ---------------------------------------------------------------------------
# fixtures
# ---------------------------------------------------------------------------


@pytest.fixture
def ww_process():
    existing = find_ww_process()
    if existing:
        pytest.skip("ww.exe already running – skip to avoid conflict")

    proc = subprocess.Popen([WW_EXE])
    time.sleep(WAIT_PROCESS)
    hwnd = find_ww_hwnd()
    if hwnd is None:
        # try once more after a bit more time
        time.sleep(1.0)
        hwnd = find_ww_hwnd()
    assert hwnd is not None, "ww.exe failed to create WW_TrayWindow"
    yield proc
    # teardown
    try:
        p = psutil.Process(proc.pid)
        kill_process(p)
    except psutil.NoSuchProcess:
        pass


# ===================================================================
# tests
# ===================================================================

class TestLaunch:
    """Basic startup tests."""

    def test_startup_creates_message_window(self, ww_process):
        hwnd = find_ww_hwnd()
        assert hwnd is not None
        assert win32gui.IsWindow(hwnd)
        cls = win32gui.GetClassName(hwnd)
        assert cls == "WW_TrayWindow"

    def test_single_instance(self, ww_process):
        proc2 = subprocess.Popen([WW_EXE])
        time.sleep(WAIT_PROCESS)
        ret = proc2.poll()
        assert ret is not None, "second instance did not exit"

    def test_tray_icon_created(self, ww_process):
        proc = find_ww_process()
        assert proc is not None
        assert find_ww_hwnd() is not None


class TestSnapshotBasic:
    """Save / restore window positions and states."""

    def test_save_restore_position(self, ww_process):
        np_proc = launch_notepad()
        np_hwnd = wait_for_notepad()
        assert np_hwnd is not None, "notepad did not appear"

        rect_before = win32gui.GetWindowRect(np_hwnd)
        w = rect_before[2] - rect_before[0]
        h = rect_before[3] - rect_before[1]

        press_ctrl_number(0)

        new_left = rect_before[0] + 100
        new_top = rect_before[1] + 100
        win32gui.SetWindowPos(
            np_hwnd, 0, new_left, new_top, w, h, win32con.SWP_NOZORDER
        )
        time.sleep(0.3)

        press_ctrl_number(1)

        rect_after = win32gui.GetWindowRect(np_hwnd)
        assert rect_after == rect_before, (
            f"Expected {rect_before}, got {rect_after}"
        )

        close_notepad(proc=np_proc, hwnd=np_hwnd)

    def test_snapshot_toggle(self, ww_process):
        np_proc = launch_notepad()
        np_hwnd = wait_for_notepad()
        assert np_hwnd is not None

        rect_orig = win32gui.GetWindowRect(np_hwnd)
        w = rect_orig[2] - rect_orig[0]
        h = rect_orig[3] - rect_orig[1]

        press_ctrl_number(0)

        pos1 = (rect_orig[0] + 150, rect_orig[1] + 150)
        win32gui.SetWindowPos(
            np_hwnd, 0, pos1[0], pos1[1], w, h, win32con.SWP_NOZORDER
        )
        time.sleep(0.3)

        press_ctrl_number(1)

        rect_mid = win32gui.GetWindowRect(np_hwnd)
        assert rect_mid == rect_orig, f"Expected {rect_orig}, got {rect_mid}"

        press_ctrl_number(1)

        rect_toggled = win32gui.GetWindowRect(np_hwnd)
        assert rect_toggled[:2] == pos1, (
            f"Expected {pos1}, got {rect_toggled[:2]}"
        )

        close_notepad(proc=np_proc, hwnd=np_hwnd)

    def test_multiple_windows_snapshot(self, ww_process):
        notepads = []
        for _ in range(3):
            proc = launch_notepad()
            hwnd = wait_for_notepad()
            if hwnd:
                notepads.append((proc, hwnd))

        assert len(notepads) >= 2, f"need ≥2 notepads, got {len(notepads)}"

        positions_before = {}
        for proc, hwnd in notepads:
            positions_before[hwnd] = win32gui.GetWindowRect(hwnd)

        press_ctrl_number(0)

        offsets = [(100, 0), (0, 100), (100, 100)]
        for i, (proc, hwnd) in enumerate(notepads):
            r = positions_before[hwnd]
            dx, dy = offsets[i % len(offsets)]
            win32gui.SetWindowPos(
                hwnd, 0, r[0] + dx, r[1] + dy,
                r[2] - r[0], r[3] - r[1], win32con.SWP_NOZORDER
            )
        time.sleep(0.3)

        press_ctrl_number(1)

        for proc, hwnd in notepads:
            ra = win32gui.GetWindowRect(hwnd)
            rb = positions_before[hwnd]
            assert ra == rb, f"hwnd={hwnd}: expected {rb}, got {ra}"

        for proc, hwnd in notepads:
            close_notepad(proc=proc, hwnd=hwnd)

    def test_window_minimize_state(self, ww_process):
        np_proc = launch_notepad()
        np_hwnd = wait_for_notepad()
        assert np_hwnd is not None

        press_ctrl_number(0)

        win32gui.ShowWindow(np_hwnd, win32con.SW_MINIMIZE)
        time.sleep(0.5)
        assert win32gui.IsIconic(np_hwnd), "should be minimized"

        press_ctrl_number(1)
        time.sleep(0.5)

        assert not win32gui.IsIconic(np_hwnd), "should not be minimized after restore"

        close_notepad(proc=np_proc, hwnd=np_hwnd)

    def test_window_maximize_state(self, ww_process):
        np_proc = launch_notepad()
        np_hwnd = wait_for_notepad()
        assert np_hwnd is not None

        win32gui.ShowWindow(np_hwnd, win32con.SW_MAXIMIZE)
        time.sleep(0.5)
        assert win32gui.GetWindowPlacement(np_hwnd)[1] == win32con.SW_SHOWMAXIMIZED

        press_ctrl_number(0)

        win32gui.ShowWindow(np_hwnd, win32con.SW_RESTORE)
        time.sleep(0.5)
        assert win32gui.GetWindowPlacement(np_hwnd)[1] != win32con.SW_SHOWMAXIMIZED

        press_ctrl_number(1)

        assert win32gui.GetWindowPlacement(np_hwnd)[1] == win32con.SW_SHOWMAXIMIZED

        win32gui.ShowWindow(np_hwnd, win32con.SW_RESTORE)
        time.sleep(0.3)
        close_notepad(proc=np_proc, hwnd=np_hwnd)


class TestSnapshotWindows:
    """Snapshot window enumeration and filtering."""

    def test_skips_tool_window(self, ww_process):
        """ww's own hidden message window should not appear in snapshots."""
        wins = enum_visible_windows()
        ww_wins = [w for w in wins if w.cls == "WW_TrayWindow"]
        assert len(ww_wins) == 0, "WW_TrayWindow should not be enumerated"

    def test_notepad_appears_in_snapshot(self, ww_process):
        np_proc = launch_notepad()
        np_hwnd = wait_for_notepad()
        assert np_hwnd is not None
        rect = win32gui.GetWindowRect(np_hwnd)
        w = rect[2] - rect[0]
        h = rect[3] - rect[1]

        press_ctrl_number(0)

        win32gui.SetWindowPos(
            np_hwnd, 0, rect[0] + 50, rect[1] + 50, w, h, win32con.SWP_NOZORDER
        )
        time.sleep(0.3)

        press_ctrl_number(1)
        time.sleep(0.3)

        rect_after = win32gui.GetWindowRect(np_hwnd)
        assert rect_after == rect

        close_notepad(proc=np_proc, hwnd=np_hwnd)

    def test_multiple_slots_independent(self, ww_process):
        np_proc = launch_notepad()
        np_hwnd = wait_for_notepad()
        assert np_hwnd is not None

        rect_orig = win32gui.GetWindowRect(np_hwnd)
        w = rect_orig[2] - rect_orig[0]
        h = rect_orig[3] - rect_orig[1]

        press_ctrl_number(0)

        pos1 = (rect_orig[0] + 50, rect_orig[1] + 50)
        win32gui.SetWindowPos(np_hwnd, 0, pos1[0], pos1[1], w, h, 0)
        time.sleep(0.3)

        press_ctrl_number(1)

        pos2 = (rect_orig[0] + 200, rect_orig[1] + 200)
        win32gui.SetWindowPos(np_hwnd, 0, pos2[0], pos2[1], w, h, 0)
        time.sleep(0.3)

        press_ctrl_number(2)
        time.sleep(0.3)

        rect_after = win32gui.GetWindowRect(np_hwnd)
        assert rect_after == (pos2[0], pos2[1], pos2[0] + w, pos2[1] + h)

        press_ctrl_number(0)
        time.sleep(0.3)

        rect_toggle = win32gui.GetWindowRect(np_hwnd)
        assert rect_toggle == (pos1[0], pos1[1], pos1[0] + w, pos1[1] + h)

        close_notepad(proc=np_proc, hwnd=np_hwnd)


class TestRandomized:
    """Randomized multi-window save/restore with position, state and Z-order."""

    @staticmethod
    def _zorder_of(hwnds: list[int]) -> list[int]:
        s = set(hwnds)
        order: list[int] = []

        def cb(hwnd, _):
            if hwnd in s:
                order.append(hwnd)
            return True

        win32gui.EnumWindows(cb, 0)
        return order

    def test_randomized_save_restore(self, ww_process):
        random.seed(42)
        N = random.randint(2, 4)

        notepads = []
        for _ in range(N):
            p = launch_notepad()
            h = wait_for_notepad()
            if h:
                notepads.append((p, h))
        assert len(notepads) >= 2, f"need ≥2 notepads, got {len(notepads)}"
        time.sleep(0.5)

        hwnds = [h for _, h in notepads]

        original = {}
        for _, h in notepads:
            original[h] = win32gui.GetWindowPlacement(h)
        original_z = self._zorder_of(hwnds)

        press_ctrl_number(0)

        sw = win32api.GetSystemMetrics(win32con.SM_CXSCREEN)
        sh = win32api.GetSystemMetrics(win32con.SM_CYSCREEN)
        for _, h in notepads:
            w = random.randint(300, 600)
            ht = random.randint(200, 400)
            x = random.randint(0, max(0, sw - w))
            y = random.randint(0, max(0, sh - ht))
            r = random.randint(0, 2)
            state = [win32con.SW_SHOWNORMAL, win32con.SW_MINIMIZE,
                     win32con.SW_MAXIMIZE][r]
            p = win32gui.GetWindowPlacement(h)
            win32gui.SetWindowPlacement(h, (p[0], state, p[2], p[3],
                                            (x, y, x + w, y + ht)))
            time.sleep(0.3)

        random.shuffle(hwnds)
        for h in hwnds:
            win32gui.SetWindowPos(h, win32con.HWND_TOP, 0, 0, 0, 0,
                                  win32con.SWP_NOMOVE | win32con.SWP_NOSIZE |
                                  win32con.SWP_NOACTIVATE)
            time.sleep(0.05)

        press_ctrl_number(1)
        time.sleep(0.5)

        for _, h in notepads:
            actual = win32gui.GetWindowPlacement(h)
            exp = original[h]
            assert actual[1] == exp[1], f"showCmd mismatch hwnd={h}"
            for j in range(4):
                assert abs(actual[4][j] - exp[4][j]) <= 2, \
                    f"normalRect[{j}] mismatch hwnd={h}: expected {exp[4][j]}, got {actual[4][j]}"

        restored_z = self._zorder_of(hwnds)
        assert restored_z == original_z, \
            f"Z-order mismatch\n  orig: {original_z}\n  got:  {restored_z}"

        for p, h in notepads:
            close_notepad(proc=p, hwnd=h)

    def test_repeated_swap(self, ww_process):
        """Repeatedly save/restore between two slots.

        Flow:
          1. capture initial state S0, Ctrl+0 saves S0 to slot 1.
          2. randomize → S1, Ctrl+1 saves S1 to slot 0, restores S0.  Verify S0.
          3. randomize → S2, Ctrl+0 saves S2 to slot 1, restores S1.  Verify S1.
          4. randomize → S3, Ctrl+1 saves S3 to slot 0, restores S2.  Verify S2.
          …repeat for ROUNDS cycles.
        """
        random.seed(99)
        N = random.randint(4, 7)
        ROUNDS = 5

        notepads = []
        for _ in range(N):
            p = launch_notepad()
            h = wait_for_notepad()
            if h:
                notepads.append((p, h))
        assert len(notepads) >= 3, f"need ≥3 notepads, got {len(notepads)}"
        time.sleep(0.5)

        hwnds = [h for _, h in notepads]

        def capture():
            s = {}
            for _, h in notepads:
                s[h] = win32gui.GetWindowPlacement(h)
            return s, self._zorder_of(hwnds)

        all_states: list[tuple[dict, list[int]]] = [capture()]
        press_ctrl_number(0)

        sw = win32api.GetSystemMetrics(win32con.SM_CXSCREEN)
        sh = win32api.GetSystemMetrics(win32con.SM_CYSCREEN)

        for rnd in range(ROUNDS):
            for _, h in notepads:
                w = random.randint(300, 600)
                ht = random.randint(200, 400)
                x = random.randint(0, max(0, sw - w))
                y = random.randint(0, max(0, sh - ht))
                st_choice = random.randint(0, 2)
                state = [win32con.SW_SHOWNORMAL, win32con.SW_MINIMIZE,
                         win32con.SW_MAXIMIZE][st_choice]
                p = win32gui.GetWindowPlacement(h)
                win32gui.SetWindowPlacement(
                    h, (p[0], state, p[2], p[3], (x, y, x + w, y + ht)))
                time.sleep(0.2)

            random.shuffle(hwnds)
            for h in hwnds:
                win32gui.SetWindowPos(h, win32con.HWND_TOP, 0, 0, 0, 0,
                                      win32con.SWP_NOMOVE | win32con.SWP_NOSIZE |
                                      win32con.SWP_NOACTIVATE)
                time.sleep(0.05)

            all_states.append(capture())

            slot = 1 if rnd % 2 == 0 else 0
            press_ctrl_number(slot)

            exp_placements, exp_z = all_states[rnd]
            for _, h in notepads:
                actual = win32gui.GetWindowPlacement(h)
                exp = exp_placements[h]
                assert actual[1] == exp[1], \
                    f"rnd={rnd} showCmd mismatch hwnd={h}"
                for j in range(4):
                    assert abs(actual[4][j] - exp[4][j]) <= 2, \
                        f"rnd={rnd} normalRect[{j}] hwnd={h}: exp={exp[4][j]} got={actual[4][j]}"

            actual_z = self._zorder_of(hwnds)
            if rnd == 0:
                assert actual_z == exp_z, \
                    f"rnd=0 Z-order mismatch\n  exp: {exp_z}\n  got: {actual_z}"
            else:
                assert set(actual_z) == set(exp_z), \
                    f"rnd={rnd} Z-order window-set mismatch"

        for p, h in notepads:
            close_notepad(proc=p, hwnd=h)


class TestCleanup:
    """Resource cleanup."""

    def test_no_leaked_process_after_exit(self, ww_process):
        proc = psutil.Process(ww_process.pid)
        kill_process(proc)
        time.sleep(0.5)
        assert not proc.is_running(), "ww.exe still running after kill"

    def test_mutex_released_after_exit(self, ww_process):
        proc1 = psutil.Process(ww_process.pid)
        kill_process(proc1)
        time.sleep(0.5)

        proc2 = subprocess.Popen([WW_EXE])
        time.sleep(WAIT_PROCESS)
        hwnd = find_ww_hwnd()
        assert hwnd is not None, "second instance failed after cleanup"
        try:
            p = psutil.Process(proc2.pid)
            kill_process(p)
        except psutil.NoSuchProcess:
            pass
