import logging
import os
import random
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Optional

import psutil
import pytest
import win32api
import win32con
import win32gui

# ---------------------------------------------------------------------------
# paths
# ---------------------------------------------------------------------------
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_DIR = os.path.dirname(os.path.abspath(__file__))
WW_EXE = os.path.join(REPO_ROOT, "build", "ww_dev.exe")
assert os.path.isfile(WW_EXE), f"ww_dev.exe not found at {WW_EXE}"
LOG_FILE = os.path.join(TEST_DIR, "log")
TEST_LOG_FILE = os.path.join(TEST_DIR, "test.log")

# ---------------------------------------------------------------------------
# logging
# ---------------------------------------------------------------------------
ww_log_fh = open(LOG_FILE, "w", encoding="utf-8")

test_log = logging.getLogger("test_ww")
test_log.setLevel(logging.DEBUG)
_ch = logging.StreamHandler(sys.stdout)
_ch.setLevel(logging.INFO)
_fh = logging.FileHandler(TEST_LOG_FILE, mode="w", encoding="utf-8")
_fh.setLevel(logging.DEBUG)
_fmt = logging.Formatter("%(asctime)s [%(levelname)s] %(message)s")
_ch.setFormatter(_fmt)
_fh.setFormatter(_fmt)
test_log.addHandler(_ch)
test_log.addHandler(_fh)

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _get_placement(hwnd) -> tuple:
    return win32gui.GetWindowPlacement(hwnd)


def _set_placement(hwnd, state, rect):
    win32gui.SetWindowPlacement(hwnd, (0, state, (0, 0), (0, 0), rect))


def _is_minimized(hwnd) -> bool:
    return win32gui.IsIconic(hwnd) == 1


def _is_maximized(hwnd) -> bool:
    return _get_placement(hwnd)[1] == win32con.SW_SHOWMAXIMIZED


def _zorder_of(hwnds: list[int]) -> list[int]:
    s = set(hwnds)
    order: list[int] = []

    def cb(hwnd, _):
        if hwnd in s:
            order.append(hwnd)
        return True

    win32gui.EnumWindows(cb, 0)
    return order


def _describe(hwnd):
    p = _get_placement(hwnd)
    r = p[4]
    return f"hwnd={hwnd} ({r[0]},{r[1]},{r[2]},{r[3]}) {p[1]}"


def find_ww_hwnd() -> Optional[int]:
    found = [0]

    def cb(hwnd, _):
        try:
            if win32gui.GetClassName(hwnd) == "WW_TrayWindow":
                found[0] = hwnd
                return False
        except Exception:
            pass
        return True

    for _ in range(3):
        try:
            win32gui.EnumWindows(cb, 0)
            break
        except Exception:
            time.sleep(0.3)
    return found[0] if found[0] else None


def press_ctrl_number(num: int, post_delay: float = 1.5):
    assert 0 <= num <= 9
    test_log.info("Ctrl+%d", num)
    vk = 0x30 + num

    # 可选：先将 ww 窗口置前（可能提高热键响应）
    ww_hwnd = find_ww_hwnd()
    if ww_hwnd:
        try:
            win32gui.SetForegroundWindow(ww_hwnd)
            time.sleep(0.1)
        except:
            pass

    win32api.keybd_event(0x11, 0, 0, 0)          # Ctrl down
    time.sleep(0.1)
    win32api.keybd_event(vk, 0, 0, 0)             # Digit down
    time.sleep(0.1)
    win32api.keybd_event(vk, 0, win32con.KEYEVENTF_KEYUP, 0)  # Digit up
    time.sleep(0.1)
    win32api.keybd_event(0x11, 0, win32con.KEYEVENTF_KEYUP, 0) # Ctrl up
    time.sleep(post_delay)                        # 足够长的后置等待


# ---------------------------------------------------------------------------
# notepad helpers
# ---------------------------------------------------------------------------

def _enum_notepad_hwnds() -> set[int]:
    result: set[int] = set()

    def cb(hwnd, _):
        if (win32gui.IsWindowVisible(hwnd) and
                win32gui.GetWindowText(hwnd) and
                win32gui.GetClassName(hwnd) == "Notepad"):
            result.add(hwnd)
        return True

    win32gui.EnumWindows(cb, 0)
    return result


def create_notepad(timeout: float = 8.0) -> tuple[int, subprocess.Popen]:
    existing = _enum_notepad_hwnds()
    proc = subprocess.Popen(["notepad.exe"], stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + timeout
    while time.time() < deadline:
        current = _enum_notepad_hwnds()
        new_hwnds = current - existing
        if new_hwnds:
            hwnd = next(iter(new_hwnds))
            time.sleep(0.6)
            return hwnd, proc
        time.sleep(0.25)
    proc.kill()
    raise RuntimeError("notepad did not appear")


def close_notepad(hwnd, proc):
    if hwnd and win32gui.IsWindow(hwnd):
        win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()


def create_notepad_many(n: int) -> list[tuple[int, subprocess.Popen]]:
    result = []
    for i in range(n):
        hwnd, proc = create_notepad()
        r = win32gui.GetWindowRect(hwnd)
        w, h = r[2] - r[0], r[3] - r[1]
        x, y = 100 + i * 60, 100 + i * 60
        win32gui.SetWindowPos(hwnd, 0, x, y, w, h, 0)
        result.append((hwnd, proc))
    time.sleep(0.5)
    return result


def close_notepad_many(notepads):
    for hwnd, proc in notepads:
        close_notepad(hwnd, proc)


# ---------------------------------------------------------------------------
# fixture
# ---------------------------------------------------------------------------

@pytest.fixture
def ww_process():
    existing = None
    for proc in psutil.process_iter(["pid", "name"]):
        try:
            if (proc.info["name"] or "").lower() == "ww_dev.exe":
                existing = proc
                break
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    if existing:
        pytest.skip("ww_dev.exe already running")

    proc = subprocess.Popen(
        [WW_EXE], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace")

    for _ in range(10):
        hwnd = find_ww_hwnd()
        if hwnd:
            break
        time.sleep(0.3)
    else:
        proc.kill()
        raise RuntimeError("ww_dev.exe failed to create WW_TrayWindow")

    yield proc

    hwnd = find_ww_hwnd()
    if hwnd:
        win32gui.PostMessage(hwnd, win32con.WM_DESTROY, 0, 0)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)
    out = proc.stdout.read()
    if out:
        ww_log_fh.write(out)
        ww_log_fh.flush()


# ===================================================================
# tests
# ===================================================================

class TestLaunch:

    def test_startup_creates_message_window(self, ww_process):
        hwnd = find_ww_hwnd()
        assert hwnd is not None
        assert win32gui.IsWindow(hwnd)
        assert win32gui.GetClassName(hwnd) == "WW_TrayWindow"

    def test_single_instance(self, ww_process):
        p2 = subprocess.Popen([WW_EXE])
        time.sleep(0.5)
        assert p2.poll() is not None, "second instance did not exit"

    def test_tray_icon_created(self, ww_process):
        assert find_ww_hwnd() is not None


class TestSnapshotBasic:

    def test_save_restore_position(self, ww_process):
        hwnd, proc = create_notepad()
        rect = win32gui.GetWindowRect(hwnd)
        w, h = rect[2] - rect[0], rect[3] - rect[1]
        test_log.info("notepad @ (%d,%d) %dx%d", rect[0], rect[1], w, h)

        press_ctrl_number(0)
        win32gui.SetWindowPos(hwnd, 0, rect[0] + 100, rect[1] + 100,
                              w, h, win32con.SWP_NOZORDER)
        time.sleep(0.3)
        press_ctrl_number(1)

        assert win32gui.GetWindowRect(hwnd) == rect
        close_notepad(hwnd, proc)

    def test_snapshot_toggle(self, ww_process):
        hwnd, proc = create_notepad()
        rect = win32gui.GetWindowRect(hwnd)
        w, h = rect[2] - rect[0], rect[3] - rect[1]

        press_ctrl_number(0)

        pos1 = (rect[0] + 150, rect[1] + 150)
        win32gui.SetWindowPos(hwnd, 0, pos1[0], pos1[1], w, h,
                              win32con.SWP_NOZORDER)
        time.sleep(0.3)

        press_ctrl_number(1)
        assert win32gui.GetWindowRect(hwnd) == rect

        press_ctrl_number(1)
        assert win32gui.GetWindowRect(hwnd)[:2] == pos1

        close_notepad(hwnd, proc)

    def test_multiple_windows_snapshot(self, ww_process):
        nps = create_notepad_many(3)
        positions = {h: win32gui.GetWindowRect(h) for h, _ in nps}

        press_ctrl_number(0)

        offsets = [(100, 0), (0, 100), (100, 100)]
        for i, (h, _) in enumerate(nps):
            r = positions[h]
            dx, dy = offsets[i]
            win32gui.SetWindowPos(h, 0, r[0] + dx, r[1] + dy,
                                  r[2] - r[0], r[3] - r[1],
                                  win32con.SWP_NOZORDER)
        time.sleep(0.3)

        press_ctrl_number(1)

        for h, _ in nps:
            assert win32gui.GetWindowRect(h) == positions[h]

        close_notepad_many(nps)


class TestWindowState:
    """Minimized / maximized state preservation."""

    @pytest.mark.parametrize("target_state,switch_to,switch_back", [
        (win32con.SW_MINIMIZE,  "minimized",  "not iconic"),
        (win32con.SW_MAXIMIZE,  "maximized",  "not zoomed"),
    ])
    def test_window_state(self, ww_process, target_state, switch_to,
                          switch_back):
        is_in_state = (_is_minimized if target_state == win32con.SW_MINIMIZE
                       else _is_maximized)

        hwnd, proc = create_notepad()

        press_ctrl_number(0)

        win32gui.ShowWindow(hwnd, target_state)
        time.sleep(0.5)
        assert is_in_state(hwnd), f"should be {switch_to}"

        press_ctrl_number(1)

        assert not is_in_state(hwnd), f"should be {switch_back} after restore"

        close_notepad(hwnd, proc)


class TestSnapshotSlots:

    def test_skips_tool_window(self, ww_process):
        # ww's own WW_TrayWindow should be hidden (not visible)
        ww = find_ww_hwnd()
        assert ww is not None
        assert not win32gui.IsWindowVisible(ww), \
            "WW_TrayWindow should be hidden"

    def test_multiple_slots_independent(self, ww_process):
        """Different slots hold independent snapshots."""
        hwnd, proc = create_notepad()
        r0 = win32gui.GetWindowRect(hwnd)
        w, h = r0[2] - r0[0], r0[3] - r0[1]

        # save r0 to slot 1 via Ctrl+2 (tray=1 → saves to slot 1)
        press_ctrl_number(2)
        # slot1=r0, tray=2

        p1 = (r0[0] + 80, r0[1] + 80)
        win32gui.SetWindowPos(hwnd, 0, p1[0], p1[1], w, h, 0)
        time.sleep(0.3)

        # save p1 to slot 2 via Ctrl+3 (tray=2 → saves to slot 2)
        press_ctrl_number(3)
        # slot1=r0, slot2=p1, tray=3

        # move to random
        win32gui.SetWindowPos(hwnd, 0, r0[0] + 200, r0[1] + 200, w, h, 0)
        time.sleep(0.3)

        # restore slot 1 (r0)
        press_ctrl_number(1)
        assert win32gui.GetWindowRect(hwnd) == r0

        # move to another random
        win32gui.SetWindowPos(hwnd, 0, r0[0] + 300, r0[1] + 300, w, h, 0)
        time.sleep(0.3)

        # restore slot 2 (p1)
        press_ctrl_number(2)
        assert win32gui.GetWindowRect(hwnd) == (p1[0], p1[1],
                                                 p1[0] + w, p1[1] + h)

        close_notepad(hwnd, proc)


class TestRandomized:

    def test_randomized_save_restore(self, ww_process):
        random.seed(42)
        N = random.randint(2, 4)
        nps = create_notepad_many(N)
        hwnds = [h for h, _ in nps]
        test_log.info("created %d notepads", N)

        original = {h: _get_placement(h) for h in hwnds}
        original_z = _zorder_of(hwnds)

        press_ctrl_number(0)

        sw = win32api.GetSystemMetrics(win32con.SM_CXSCREEN)
        sh = win32api.GetSystemMetrics(win32con.SM_CYSCREEN)
        for h in hwnds:
            w = random.randint(300, 600)
            ht = random.randint(200, 400)
            x = random.randint(0, max(0, sw - w))
            y = random.randint(0, max(0, sh - ht))
            st = random.choice([win32con.SW_SHOWNORMAL,
                                win32con.SW_MINIMIZE,
                                win32con.SW_MAXIMIZE])
            _set_placement(h, st, (x, y, x + w, y + ht))
            time.sleep(0.3)

        random.shuffle(hwnds)
        for h in hwnds:
            win32gui.SetWindowPos(h, win32con.HWND_TOP, 0, 0, 0, 0,
                                  win32con.SWP_NOMOVE | win32con.SWP_NOSIZE |
                                  win32con.SWP_NOACTIVATE)
            time.sleep(0.05)
        time.sleep(0.5)
        press_ctrl_number(1)

        for h in hwnds:
            actual = _get_placement(h)
            exp = original[h]
            assert actual[1] == exp[1], f"showCmd mismatch hwnd={h}"
            for j in range(4):
                assert abs(actual[4][j] - exp[4][j]) <= 2, \
                    f"rect[{j}] hwnd={h}: exp={exp[4][j]} got={actual[4][j]}"

        assert _zorder_of(hwnds) == original_z, "Z-order mismatch"

        close_notepad_many(nps)

class TestCleanup:

    def test_no_leaked_process_after_exit(self, ww_process):
        win32gui.PostMessage(find_ww_hwnd(), win32con.WM_DESTROY, 0, 0)
        ww_process.wait(timeout=5)
        assert ww_process.poll() is not None

    def test_mutex_released_after_exit(self, ww_process):
        win32gui.PostMessage(find_ww_hwnd(), win32con.WM_DESTROY, 0, 0)
        ww_process.wait(timeout=5)

        p2 = subprocess.Popen([WW_EXE])
        time.sleep(0.5)
        hwnd = find_ww_hwnd()
        assert hwnd is not None, "second instance failed after cleanup"
        win32gui.PostMessage(hwnd, win32con.WM_DESTROY, 0, 0)
        p2.wait(timeout=5)
