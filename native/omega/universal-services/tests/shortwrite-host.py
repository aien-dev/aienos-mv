#!/usr/bin/env python3
"""Host descriptor orchestration only; USV1 behavior assertions live in Omega."""
import fcntl
import os
import subprocess
import sys

exe, receipt = sys.argv[1:]
rfd, wfd = os.pipe()
try:
    fcntl.fcntl(wfd, fcntl.F_SETPIPE_SZ, 4096)
    flags = fcntl.fcntl(wfd, fcntl.F_GETFL)
    fcntl.fcntl(wfd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
    with open(os.devnull, "rb") as nullin, open(receipt, "wb") as err:
        result = subprocess.run([exe], stdin=nullin, stdout=wfd, stderr=err, pass_fds=(wfd,), check=False)
    os.close(wfd)
    wfd = -1
    data = bytearray()
    while True:
        try:
            part = os.read(rfd, 65536)
        except BlockingIOError:
            continue
        if not part:
            break
        data.extend(part)
    if result.returncode != 0:
        raise SystemExit(f"Omega short-write assertion exited {result.returncode}")
    if data != b"A" * 4096:
        raise SystemExit(f"pipe prefix mismatch: {len(data)} bytes, expected 4096 A bytes")
finally:
    if wfd >= 0:
        os.close(wfd)
    os.close(rfd)
print("PASS nonblocking 4096-byte pipe: 4096-byte committed prefix; Omega observed short write then -3")
