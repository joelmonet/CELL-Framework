#!/usr/bin/env python3
"""Run native GUI contracts on a supplied test display with a ready window manager."""
import argparse
import ctypes
import ctypes.util
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--window-manager', default='openbox')
    parser.add_argument('--wm-log', type=Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command or not os.environ.get('DISPLAY'):
        parser.error('Supply a test DISPLAY (normally xvfb-run) and a command after --')
    library = ctypes.util.find_library('X11')
    if not library:
        raise RuntimeError('Native X11 library is unavailable')
    x = ctypes.CDLL(library)
    x.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x.XOpenDisplay.restype = ctypes.c_void_p
    x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x.XDefaultRootWindow.restype = ctypes.c_ulong
    x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x.XInternAtom.restype = ctypes.c_ulong
    x.XGetWindowProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
        ctypes.c_long, ctypes.c_long, ctypes.c_int, ctypes.c_ulong,
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_void_p)]
    x.XFree.argtypes = [ctypes.c_void_p]
    x.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x.XOpenDisplay(None)
    if not display:
        raise RuntimeError('Cannot connect to the supplied test display')

    def ready():
        atom = x.XInternAtom(display, b'_NET_SUPPORTING_WM_CHECK', 1)
        if not atom:
            return False
        actual, fmt = ctypes.c_ulong(), ctypes.c_int()
        count, remaining, data = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_void_p()
        result = x.XGetWindowProperty(display, x.XDefaultRootWindow(display), atom,
            0, 1, 0, 0, ctypes.byref(actual), ctypes.byref(fmt), ctypes.byref(count),
            ctypes.byref(remaining), ctypes.byref(data))
        try:
            return (result == 0 and fmt.value == 32 and count.value == 1 and
                    bool(data.value) and bool(ctypes.cast(data,
                    ctypes.POINTER(ctypes.c_ulong))[0]) and
                    bool(x.XInternAtom(display, b'WM_PROTOCOLS', 1)))
        finally:
            if data.value:
                x.XFree(data)

    manager = None
    args.wm_log.parent.mkdir(parents=True, exist_ok=True)
    try:
        with args.wm_log.open('w') as log:
            if not ready():
                manager = subprocess.Popen([args.window_manager, '--sm-disable'],
                    stdout=log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 10
                while not ready():
                    if manager.poll() is not None:
                        raise RuntimeError('Owned window manager failed to start; see ' + str(args.wm_log))
                    if time.monotonic() >= deadline:
                        raise RuntimeError('Owned window manager did not publish readiness')
                    time.sleep(.01)
            print('Native GUI contracts: window manager ready', flush=True)
            return subprocess.run(command).returncode
    finally:
        x.XCloseDisplay(display)
        # Only the process launched here is retired. An existing manager and
        # the caller-owned display are never signalled or reconfigured.
        if manager is not None and manager.poll() is None:
            manager.terminate()
            try:
                manager.wait(timeout=5)
            except subprocess.TimeoutExpired:
                manager.kill()
                manager.wait()


if __name__ == '__main__':
    raise SystemExit(main())
