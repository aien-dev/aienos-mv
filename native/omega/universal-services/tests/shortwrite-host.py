#!/usr/bin/env python3
"""Host descriptor orchestration only; USV1 behavior assertions live in Omega."""
import fcntl
import os
import subprocess
import sys
from pathlib import Path

exe, stderr_path, output_path = sys.argv[1:]
rfd, wfd = os.pipe()
try:
    fcntl.fcntl(wfd, fcntl.F_SETPIPE_SZ, 4096)
    flags = fcntl.fcntl(wfd, fcntl.F_GETFL)
    fcntl.fcntl(wfd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
    with open(os.devnull, "rb") as nullin, open(stderr_path, "wb") as err:
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
    Path(output_path).write_bytes(data)
    if result.returncode:
        raise SystemExit(result.returncode)
finally:
    if wfd >= 0:
        os.close(wfd)
    os.close(rfd)
