"""Run the real controlled WGC fixture on an unselected Windows desktop.

QT_QPA_PLATFORM=offscreen alone does not hide native Win32 test windows.
This runner never switches the user's desktop. Qt and vcpkg DLLs must be on PATH.
"""
import argparse
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

parser = argparse.ArgumentParser()
parser.add_argument("--build", default="build")
parser.add_argument("--log", default="temp/proof-of-work/worklist-2026-09-13-1250/item-03/native-probe.txt")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
log = (root / args.log).resolve()
log.parent.mkdir(parents=True, exist_ok=True)
probe = root / args.build / 'bin-streaming-validation/StreamingCaptureProbe.exe'
if not probe.exists():
    raise SystemExit('Build StreamingCaptureProbe first')
ctest = str(probe)
command = [ctest, '--fixture', '--output', str(log.parent), '--log', str(log)]
os.environ["QT_QPA_PLATFORM"] = "offscreen"
if sys.platform != "win32":
    raise SystemExit(subprocess.call(command))

class StartupInfo(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR),
                ("lpDesktop", wintypes.LPWSTR), ("lpTitle", wintypes.LPWSTR),
                ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
                ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD),
                ("dwXCountChars", wintypes.DWORD), ("dwYCountChars", wintypes.DWORD),
                ("dwFillAttribute", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wintypes.HANDLE),
                ("hStdOutput", wintypes.HANDLE), ("hStdError", wintypes.HANDLE)]

class ProcessInfo(ctypes.Structure):
    _fields_ = [("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
                ("dwProcessId", wintypes.DWORD), ("dwThreadId", wintypes.DWORD)]

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32.CreateDesktopW.argtypes = [wintypes.LPCWSTR, ctypes.c_void_p, ctypes.c_void_p,
                                  wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
user32.CreateDesktopW.restype = wintypes.HANDLE
user32.CloseDesktop.argtypes = [wintypes.HANDLE]
kernel32.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p,
                                   ctypes.c_void_p, wintypes.BOOL, wintypes.DWORD,
                                   ctypes.c_void_p, wintypes.LPCWSTR,
                                   ctypes.POINTER(StartupInfo), ctypes.POINTER(ProcessInfo)]
kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

name = "Deskflow-check-" + uuid.uuid4().hex
desktop = user32.CreateDesktopW(name, None, None, 0, 0x10000000, None)
if not desktop:
    raise ctypes.WinError(ctypes.get_last_error())
info = ProcessInfo()
try:
    startup = StartupInfo()
    startup.cb = ctypes.sizeof(startup)
    startup.lpDesktop = "WinSta0\\" + name
    startup.dwFlags = 1
    startup.wShowWindow = 0
    line = ctypes.create_unicode_buffer(subprocess.list2cmdline(command))
    if not kernel32.CreateProcessW(ctest, line, None, None, False, 0x08000000, None,
                                   str(root), ctypes.byref(startup), ctypes.byref(info)):
        raise ctypes.WinError(ctypes.get_last_error())
    print("Running controlled capture fixture on unselected desktop " + name, flush=True)
    kernel32.WaitForSingleObject(info.hProcess, 0xFFFFFFFF)
    code = wintypes.DWORD()
    if not kernel32.GetExitCodeProcess(info.hProcess, ctypes.byref(code)):
        raise ctypes.WinError(ctypes.get_last_error())
    print(log.read_text(encoding="utf-8", errors="replace"), end="")
    raise SystemExit(code.value)
finally:
    if info.hThread:
        kernel32.CloseHandle(info.hThread)
    if info.hProcess:
        kernel32.CloseHandle(info.hProcess)
    user32.CloseDesktop(desktop)
