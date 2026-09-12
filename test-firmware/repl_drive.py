#!/usr/bin/env python3
"""Drive MicroPython REPL on stdin/stdout with prompt synchronization.
Usage: repl_drive.py <cmd> [args...] < script.lines
Sends each line after seeing '>>> '. Prints everything. Exits 0 when the
child exits (caller checks output), 1 on prompt timeout.
"""
import subprocess
import sys
import time

cmd = sys.argv[1:]
lines = [ln.rstrip("\n") for ln in sys.stdin if ln.strip() != ""]
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT)
out = b""
deadline = time.time() + 900


def drain():
    global out
    import select
    while select.select([p.stdout], [], [], 0.2)[0]:
        chunk = p.stdout.read1(65536) if hasattr(p.stdout, "read1") else p.stdout.read(65536)
        if not chunk:
            break
        out += chunk
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()


def wait_prompt():
    while time.time() < deadline:
        drain()
        if out.endswith(b">>> ") or out.endswith(b">>> \r\n"):
            return True
        if p.poll() is not None:
            drain()
            return False
        time.sleep(0.2)
    return False


ok = True
for ln in lines:
    if not wait_prompt():
        print("\nREPL-DRIVE: prompt timeout / child exited", flush=True)
        ok = False
        break
    out = b""
    p.stdin.write((ln + "\r").encode())
    p.stdin.flush()
    time.sleep(0.5)
drain()
try:
    p.wait(timeout=120)
except Exception:
    pass
sys.exit(0 if ok else 1)
