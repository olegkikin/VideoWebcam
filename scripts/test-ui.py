"""Exercise a hidden app window using Win32 APIs. No third-party packages required."""
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import subprocess
import time
import sys

root = Path(__file__).resolve().parent.parent
package = root / 'dist' / (sys.argv[1] if len(sys.argv) > 1 else 'VideoWebcam')
settings = package / 'settings.ini'
previous_settings = settings.read_bytes() if settings.exists() else None
user = c.WinDLL('user32', use_last_error=True)
user.FindWindowW.argtypes = [w.LPCWSTR, w.LPCWSTR]
user.FindWindowW.restype = w.HWND
user.GetDlgItem.argtypes = [w.HWND, c.c_int]
user.GetDlgItem.restype = w.HWND
user.IsWindowEnabled.argtypes = [w.HWND]
user.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user.SendMessageW.restype = c.c_ssize_t
user.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]

assert not user.FindWindowW('VideoWebcam.Window', None), 'Close VideoWebcam before the UI test.'
startup = subprocess.STARTUPINFO()
startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
process = subprocess.Popen([str(package / 'VideoWebcam.exe'),
                            str(root / 'test-output/media with spaces/red fixture.png')], startupinfo=startup)
window = None
try:
    for _ in range(100):
        window = user.FindWindowW('VideoWebcam.Window', None)
        if window and user.IsWindowEnabled(user.GetDlgItem(window, 102)):
            break
        assert process.poll() is None, 'App exited during startup.'
        time.sleep(.1)
    assert window and user.IsWindowEnabled(user.GetDlgItem(window, 102)), 'Preview did not become ready.'
    def button_text(number):
        text = c.create_unicode_buffer(128)
        user.GetWindowTextW(user.GetDlgItem(window, number), text, len(text))
        return text.value
    user.SendMessageW(window, 0x111, 102, 0)
    assert button_text(102) == 'Stop camera', 'Start camera did not activate.'
    assert user.SendMessageW(user.GetDlgItem(window,109),0x146,0,0) == 3, 'Resolution choices missing.'
    assert user.SendMessageW(user.GetDlgItem(window,110),0x146,0,0) == 5, 'FPS choices missing.'
    # Exercise a format change while broadcasting, including a different aspect ratio.
    for resolution, fps in [(0,0),(2,4),(1,3)]:
        for control, selection in [(109,resolution),(110,fps)]:
            user.SendMessageW(user.GetDlgItem(window,control),0x14E,selection,0)
            user.SendMessageW(window,0x111,control | (1 << 16),0)
        for _ in range(100):
            if user.IsWindowEnabled(user.GetDlgItem(window,103)):
                break
            time.sleep(.05)
        assert user.IsWindowEnabled(user.GetDlgItem(window,103)), 'Playback failed after output format change.'
        assert button_text(102) == 'Stop camera', 'Changing output unexpectedly stopped broadcasting.'
    user.SendMessageW(window, 0x111, 103, 0)
    assert button_text(103) == 'Resume', 'Pause did not activate.'
    user.SendMessageW(window, 0x111, 103, 0)
    user.SendMessageW(window, 0x111, 106, 0)
    assert button_text(106) == 'Framing: fill / crop', 'Framing toggle failed.'
    user.SendMessageW(window, 0x111, 107, 0)
    assert button_text(107) == 'Mirror: on', 'Mirror toggle failed.'
    time.sleep(.7)
    assert user.IsWindowEnabled(user.GetDlgItem(window, 102)), 'Playback failed after switching options.'
    user.SendMessageW(window, 0x111, 102, 0)
    assert button_text(102) == 'Start camera', 'Stop camera did not deactivate.'
    user.PostMessageW(window, 0x10, 0, 0)
    assert process.wait(timeout=5) == 0, 'App did not exit cleanly.'
    print('UI control smoke test passed: resolution/FPS changes while live, load, start/stop, pause/resume, framing, mirror, and clean exit.')
finally:
    if process.poll() is None:
        if window:
            user.PostMessageW(window, 0x10, 0, 0)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    if previous_settings is None:
        settings.unlink(missing_ok=True)
    else:
        settings.write_bytes(previous_settings)
