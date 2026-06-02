"""Tests for ww.exe — Windows workspace manager.

Hotkeys are sent via PostMessage(WM_HOTKEY) to the hidden ww window,
which is faster and more reliable than keyboard simulation.
"""
import logging
import os
import random
import subprocess
import sys
import time

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
WW_LOG = os.path.join(os.path.dirname(WW_EXE), "ww.log")

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
# constants matching main.cpp
# ---------------------------------------------------------------------------
WM_TRAYICON = win32con.WM_APP + 1
WM_INIT_TRAY = win32con.WM_APP + 2
WM_HOTKEY = 0x0312
HOTKEY_DIGIT = 0       # HotkeyId::Digit

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _get_placement(hwnd):
    return win32gui.GetWindowPlacement(hwnd)


def _set_placement(hwnd, state, rect):
    win32gui.SetWindowPlacement(hwnd, (0, state, (0, 0), (0, 0), rect))


def _is_minimized(hwnd) -> bool:
    return win32gui.IsIconic(hwnd) == 1


def _is_maximized(hwnd) -> bool:
    return _get_placement(hwnd)[1] == win32con.SW_SHOWMAXIMIZED


def _rect_of(hwnd) -> tuple[int, int, int, int]:
    return win32gui.GetWindowRect(hwnd)


def _zorder_of(hwnds: list[int]) -> list[int]:
    s = set(hwnds)
    order: list[int] = []

    def cb(hwnd, _):
        if hwnd in s:
            order.append(hwnd)
        return True

    win32gui.EnumWindows(cb, 0)
    return order


def find_ww_hwnd() -> int | None:
    found = [0]

    def cb(hwnd, _):
        try:
            if win32gui.GetClassName(hwnd) == "WW_TrayWindow":
                found[0] = hwnd
                return False
        except Exception:
            pass
        return True

    for _ in range(5):
        try:
            win32gui.EnumWindows(cb, 0)
            break
        except Exception:
            time.sleep(0.15)
    return found[0] if found[0] else None


def switch_to(ww_hwnd: int, slot: int, delay: float = 0.25):
    """Send Ctrl+<slot> to ww via PostMessage(WM_HOTKEY)."""
    assert 0 <= slot <= 9
    test_log.info("switch to %d", slot)
    win32gui.PostMessage(ww_hwnd, WM_HOTKEY, HOTKEY_DIGIT + slot, 0)
    time.sleep(delay)


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
    proc = subprocess.Popen(
        ["notepad.exe"], stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + timeout
    while time.time() < deadline:
        current = _enum_notepad_hwnds()
        new_hwnds = current - existing
        if new_hwnds:
            hwnd = next(iter(new_hwnds))
            # 等待窗口完全就绪：连续两次检测 IsWindowVisible 均为 True，
            # 间隔 0.15s，避免 EnumWindows 在窗口闪烁间隙跳过它
            for _ in range(30):
                if win32gui.IsWindowVisible(hwnd):
                    time.sleep(0.15)
                    if win32gui.IsWindowVisible(hwnd):
                        break
                time.sleep(0.08)
            time.sleep(0.2)
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
        win32gui.SetWindowPos(hwnd, 0, x, y, w, h,
                              win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        result.append((hwnd, proc))
    time.sleep(0.2)
    return result


def close_notepad_many(notepads):
    for hwnd, proc in notepads:
        close_notepad(hwnd, proc)


# ---------------------------------------------------------------------------
# log verification helpers  (ww.log uses shared access now)
# ---------------------------------------------------------------------------

def _read_ww_log(tail: int = 300) -> str:
    if not os.path.isfile(WW_LOG):
        return ""
    for _ in range(5):
        try:
            with open(WW_LOG, "r", encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
            return "".join(lines[-tail:])
        except PermissionError:
            time.sleep(0.05)
    return ""


def _log_has_warnings() -> bool:
    return "[验证] [!]" in _read_ww_log()


# ---------------------------------------------------------------------------
# fixture
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session", autouse=True)
def _warmup_notepad():
    """预热：启动并关闭一个 notepad，让后续 notepad 启动更快。
    避免第一个 notepad 因加载慢导致 ww 的 EnumWindows 错过它。"""
    hwnd, proc = create_notepad(timeout=10.0)
    close_notepad(hwnd, proc)
    time.sleep(0.3)


@pytest.fixture(scope="function")
def ww():
    """Start ww_dev.exe, yield (proc, hwnd).  Skip if already running."""
    for proc in psutil.process_iter(["pid", "name"]):
        try:
            if (proc.info["name"] or "").lower() == "ww_dev.exe":
                pytest.skip("ww_dev.exe already running")
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass

    proc = subprocess.Popen(
        [WW_EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    hwnd = None
    for _ in range(7):
        hwnd = find_ww_hwnd()
        if hwnd:
            break
        time.sleep(0.15)
    if hwnd is None:
        proc.kill()
        raise RuntimeError("ww_dev.exe did not create window")

    yield proc, hwnd

    # --- teardown ---
    if win32gui.IsWindow(hwnd):
        win32gui.PostMessage(hwnd, win32con.WM_DESTROY, 0, 0)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


# ===================================================================
# tests
# ===================================================================

class TestLaunch:
    def test_startup(self, ww):
        _proc, hwnd = ww
        assert hwnd is not None
        assert win32gui.IsWindow(hwnd)
        assert win32gui.GetClassName(hwnd) == "WW_TrayWindow"

    def test_single_instance(self, ww):
        p2 = subprocess.Popen([WW_EXE])
        time.sleep(0.25)
        assert p2.poll() is not None, "second instance did not exit"

    def test_log_file(self, ww):
        """ww.log created with shared access."""
        _proc, hwnd = ww
        switch_to(hwnd, 9)
        # _read_ww_log retries on PermissionError
        text = _read_ww_log()
        assert len(text) > 0, "ww.log is empty or unreadable"
        # these diag lines came with the sort-fix
        assert "[快照数据]" in text or "Ctrl+0~9" in text


class TestBasic:
    """Core save/restore with notepad windows."""

    def test_position(self, ww):
        """Save → move → restore: position must revert."""
        _proc, hwnd = ww
        np_hwnd, np_proc = create_notepad()
        orig = _rect_of(np_hwnd)

        switch_to(hwnd, 0)                     # save→1, restore empty 0 → min
        assert _is_minimized(np_hwnd), "slot 0 empty → should minimize"

        # move while minimized (affects rcNormalPosition)
        w, h = orig[2] - orig[0], orig[3] - orig[1]
        new_pos = (orig[0] + 100, orig[1] + 100)
        win32gui.SetWindowPos(np_hwnd, 0, new_pos[0], new_pos[1], w, h,
                              win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        time.sleep(0.1)

        switch_to(hwnd, 1)                     # save→0, restore 1 → orig

        assert not _is_minimized(np_hwnd), "should be restored"
        cur = _rect_of(np_hwnd)
        assert cur == orig, f"pos mismatch: {orig} vs {cur}"
        close_notepad(np_hwnd, np_proc)

    def test_toggle(self, ww):
        """Ctrl+N twice on same slot toggles between two states."""
        _proc, hwnd = ww
        np_hwnd, np_proc = create_notepad()
        orig = _rect_of(np_hwnd)

        # first press: save→1, tray goes to 0
        switch_to(hwnd, 0)
        # move while minimized — must use SetWindowPlacement because
        # SetWindowPos on a minimized window only moves the icon,
        # not rcNormalPosition
        w, h = orig[2] - orig[0], orig[3] - orig[1]
        pos_a = (orig[0] + 120, orig[1] + 120)
        _set_placement(np_hwnd, win32con.SW_MINIMIZE,
                       (pos_a[0], pos_a[1], pos_a[0] + w, pos_a[1] + h))
        time.sleep(0.1)

        # second press: save→0, toggle back to 1 → restores orig (normal)
        switch_to(hwnd, 1)
        assert _rect_of(np_hwnd) == orig
        assert not _is_minimized(np_hwnd)

        # third press: toggles back to 0 → restores pos_a, but MINIMIZED
        # (slot 0 was saved while window was minimized)
        switch_to(hwnd, 1)
        assert _is_minimized(np_hwnd), "window should be minimized (saved that way)"
        # rcNormalPosition should be pos_a even while minimized
        wp = _get_placement(np_hwnd)
        np_rect = wp[4]
        assert np_rect[:2] == pos_a, f"normal pos mismatch: {np_rect[:2]} vs {pos_a}"

        close_notepad(np_hwnd, np_proc)

    def test_multiple_windows(self, ww):
        _proc, hwnd = ww
        nps = create_notepad_many(3)
        orig_pos = {h: _rect_of(h) for h, _ in nps}

        switch_to(hwnd, 0)                     # save→1, all min to slot 0

        # offset each
        offsets = [(80, 0), (0, 80), (80, 80)]
        for i, (h, _) in enumerate(nps):
            r = orig_pos[h]
            dx, dy = offsets[i]
            win32gui.SetWindowPos(h, 0, r[0] + dx, r[1] + dy,
                                  r[2] - r[0], r[3] - r[1],
                                  win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        time.sleep(0.1)

        switch_to(hwnd, 1)                     # restore slot 1

        for h, _ in nps:
            assert _rect_of(h) == orig_pos[h], \
                f"window not restored to original position"

        close_notepad_many(nps)


class TestWindowState:
    @pytest.mark.parametrize("target,desc", [
        (win32con.SW_MINIMIZE, "min"),
        (win32con.SW_MAXIMIZE, "max"),
    ])
    def test_state(self, ww, target, desc):
        is_in = (_is_minimized if target == win32con.SW_MINIMIZE
                 else _is_maximized)
        _proc, hwnd = ww
        np_hwnd, np_proc = create_notepad()

        switch_to(hwnd, 0)                     # save normal → slot 1
        win32gui.ShowWindow(np_hwnd, target)
        time.sleep(0.3)
        assert is_in(np_hwnd), f"should be {desc}"

        switch_to(hwnd, 1)                     # toggle: save min→0, restore 1
        # slot 1 has the ORIGINAL state (normal), so window should be normal
        assert not is_in(np_hwnd), f"restore should revert from {desc}"

        close_notepad(np_hwnd, np_proc)


class TestMultiSlot:
    def test_independent(self, ww):
        """Each slot stores independent layout."""
        _proc, hwnd = ww
        np_hwnd, np_proc = create_notepad()
        r0 = _rect_of(np_hwnd)
        w, h = r0[2] - r0[0], r0[3] - r0[1]

        # slot 1 ← r0   (via Ctrl+2: save→1, tray=2)
        switch_to(hwnd, 2)
        assert _is_minimized(np_hwnd), "slot 2 empty → minimized"

        # move to p1 while minimized → must use SetWindowPlacement
        p1 = (r0[0] + 60, r0[1] + 60)
        _set_placement(np_hwnd, win32con.SW_MINIMIZE,
                       (p1[0], p1[1], p1[0] + w, p1[1] + h))
        time.sleep(0.1)
        switch_to(hwnd, 3)                     # saves p1→2
        assert _is_minimized(np_hwnd), "slot 3 empty → minimized"

        # move far away while minimized
        far = (r0[0] + 300, r0[1] + 300)
        _set_placement(np_hwnd, win32con.SW_MINIMIZE,
                       (far[0], far[1], far[0] + w, far[1] + h))
        time.sleep(0.1)

        # restore slot 1 (r0)
        switch_to(hwnd, 1)                     # save far→3, restore 1
        assert _rect_of(np_hwnd) == r0

        # restore slot 2 (p1) — saved while minimized, so window stays minimized
        switch_to(hwnd, 2)                     # save r0→1, restore 2
        assert _is_minimized(np_hwnd), "slot 2 saved minimized → should stay minimized"
        wp = _get_placement(np_hwnd)
        np_rect = wp[4]
        assert np_rect == (p1[0], p1[1], p1[0] + w, p1[1] + h), \
            f"normal pos mismatch: {np_rect} vs {p1}"

        close_notepad(np_hwnd, np_proc)


class TestEmptySlot:
    def test_minimizes_all(self, ww):
        """Switch to unused slot → all tracked windows minimized."""
        _proc, hwnd = ww
        nps = create_notepad_many(2)
        hwnds = [h for h, _ in nps]

        switch_to(hwnd, 9)
        for h in hwnds:
            assert _is_minimized(h), f"hwnd={h} not minimized"
        close_notepad_many(nps)

    def test_roundtrip(self, ww):
        """Empty → back to saved slot restores correctly."""
        _proc, hwnd = ww
        np_hwnd, np_proc = create_notepad()
        orig = _rect_of(np_hwnd)

        switch_to(hwnd, 2)                     # save→1, goto 2 (empty)→min
        assert _is_minimized(np_hwnd)

        switch_to(hwnd, 9)                     # save→2, goto 9 (empty)
        assert _is_minimized(np_hwnd)

        switch_to(hwnd, 1)                     # save→9, restore 1
        assert not _is_minimized(np_hwnd)
        assert _rect_of(np_hwnd) == orig

        close_notepad(np_hwnd, np_proc)


class TestSortFix:
    """sort-on-copy fix: snapshot data preserved across repeated restores."""

    def test_repeated_restore(self, ww):
        """Restore slot 0 through multiple hops; position must match original."""
        _proc, hwnd = ww
        np_hwnd, np_proc = create_notepad()
        orig = _rect_of(np_hwnd)

        switch_to(hwnd, 0)                     # save→1, restore 0 (empty)
        time.sleep(0.15)

        # chain through empty slots
        for s in [3, 5, 7]:
            switch_to(hwnd, s)
        # now tray=7, all minimized

        switch_to(hwnd, 0)                     # save→7, restore 0 → empty → still min
        # last restore was from 0 (empty) so np is still minimized.
        # We need to get back to slot 1 which has the original save.
        switch_to(hwnd, 1)                     # save→0, restore 1 → orig
        time.sleep(0.2)

        assert not _is_minimized(np_hwnd)
        assert _rect_of(np_hwnd) == orig

        close_notepad(np_hwnd, np_proc)

    def test_zorder(self, ww):
        """Z-order survives save/restore."""
        _proc, hwnd = ww
        nps = create_notepad_many(3)
        hwnds = [h for h, _ in nps]
        orig_z = _zorder_of(hwnds)

        switch_to(hwnd, 0)                     # save→1, all min

        # shuffle Z while minimized
        for h in reversed(hwnds):
            win32gui.SetWindowPos(h, win32con.HWND_TOP, 0, 0, 0, 0,
                                  win32con.SWP_NOMOVE | win32con.SWP_NOSIZE |
                                  win32con.SWP_NOACTIVATE)
            time.sleep(0.03)
        time.sleep(0.15)

        switch_to(hwnd, 1)                     # save→0, restore 1
        new_z = _zorder_of(hwnds)
        assert new_z == orig_z, f"Z-order mismatch: {orig_z} vs {new_z}"

        close_notepad_many(nps)


class TestRandomized:
    def test_random(self, ww):
        random.seed(42)
        _proc, hwnd = ww
        N = random.randint(2, 4)
        nps = create_notepad_many(N)
        hwnds = [h for h, _ in nps]
        test_log.info("%d notepads", N)

        orig_placement = {h: _get_placement(h) for h in hwnds}
        orig_z = _zorder_of(hwnds)

        switch_to(hwnd, 0)                     # save→1

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
            time.sleep(0.1)

        # shuffle Z
        random.shuffle(hwnds)
        for h in hwnds:
            win32gui.SetWindowPos(h, win32con.HWND_TOP, 0, 0, 0, 0,
                                  win32con.SWP_NOMOVE | win32con.SWP_NOSIZE |
                                  win32con.SWP_NOACTIVATE)
            time.sleep(0.03)
        time.sleep(0.2)

        switch_to(hwnd, 1)                     # save→0, restore 1

        for h in hwnds:
            act = _get_placement(h)
            exp = orig_placement[h]
            assert act[1] == exp[1], f"showCmd mismatch hwnd={h}"
            for j in range(4):
                assert abs(act[4][j] - exp[4][j]) <= 2, \
                    f"rect[{j}] hwnd={h}: exp={exp[4][j]} act={act[4][j]}"

        assert _zorder_of(hwnds) == orig_z

        close_notepad_many(nps)


class TestLogDiagnostics:
    """Verify [快照数据] dump and [验证] section in ww.log."""

    def test_snapshot_dump(self, ww):
        _proc, hwnd = ww
        np_hwnd, np_proc = create_notepad()
        switch_to(hwnd, 2)                     # save→1, restore 2 (empty)
        switch_to(hwnd, 1)                     # save→2, restore 1
        time.sleep(0.15)
        text = _read_ww_log()
        assert "[快照数据]" in text, "missing snapshot data dump"
        assert "[验证] 开始对比" in text, "missing verification header"
        close_notepad(np_hwnd, np_proc)

    def test_no_warnings(self, ww):
        """Correct save/restore should produce zero [!] warnings."""
        _proc, hwnd = ww
        np_hwnd, np_proc = create_notepad()
        switch_to(hwnd, 0)
        switch_to(hwnd, 1)
        time.sleep(0.15)
        assert not _log_has_warnings(), \
            "unexpected [!] — save/restore should match"
        close_notepad(np_hwnd, np_proc)


class TestCleanup:
    def test_process_exits(self, ww):
        proc, hwnd = ww
        win32gui.PostMessage(hwnd, win32con.WM_DESTROY, 0, 0)
        proc.wait(timeout=5)
        assert proc.poll() is not None

    def test_mutex_released(self, ww):
        proc, hwnd = ww
        win32gui.PostMessage(hwnd, win32con.WM_DESTROY, 0, 0)
        proc.wait(timeout=5)

        p2 = subprocess.Popen([WW_EXE])
        time.sleep(0.3)
        h2 = find_ww_hwnd()
        assert h2 is not None, "second instance should start"
        win32gui.PostMessage(h2, win32con.WM_DESTROY, 0, 0)
        p2.wait(timeout=5)
