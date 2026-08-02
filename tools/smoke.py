#!/usr/bin/env python3
"""Post-flash smoke test. Run it after EVERY flash.

This exists because it did not. Features were each verified once by hand and
then left; when the camera stopped working, there was no way to tell which of a
dozen changes had done it, and the answer had to be guessed at. Sixty seconds
here buys the name of the commit that broke something.

    python3 tools/smoke.py [--port /dev/cu.usbserial-110] [--host 192.168.1.249]

Exits non-zero if any required check fails. Camera failure is reported but does
not fail the run — it is a known hardware fault on this board (GPIO 4 held low).
"""
import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: use the IDF venv python")

try:
    from urllib.request import urlopen, Request
except ImportError:
    urlopen = None

P = argparse.ArgumentParser()
P.add_argument("--port", default="/dev/cu.usbserial-110")
P.add_argument("--host", default="")
P.add_argument("--boot", type=float, default=26.0,
               help="seconds to wait for boot; the camera probe alone costs ~18s when it fails")
A = P.parse_args()

fails, warns = [], []


def check(name, ok, detail="", required=True):
    mark = "ok  " if ok else ("FAIL" if required else "warn")
    print(f"  [{mark}] {name}" + (f"  {detail}" if detail else ""))
    if not ok:
        (fails if required else warns).append(name)


print(f"opening {A.port}")
s = serial.Serial(A.port, 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)

boot = b""
t0 = time.time()
while time.time() - t0 < A.boot:
    boot += s.read(8192)
btxt = boot.decode("utf-8", "replace")


def run(cmd, wait=2.5):
    s.reset_input_buffer()
    s.write((cmd + "\r\n").encode())
    time.sleep(wait)
    out = s.read(1 << 16).decode("utf-8", "replace")
    # Strip the prompt rather than dropping lines that contain it: `cat` on a
    # file with no trailing newline puts the content and the next prompt on the
    # same line, and discarding that line loses the very thing being checked.
    out = out.replace("sesame-agent>", "")
    return "\n".join(l for l in out.splitlines()
                     if l.strip() and not l.startswith(("I (", "W (", "E (", "D ("))
                     and cmd not in l)


print("\nboot")
check("no panic or reboot loop", "rst:0x" not in btxt.split("Rebooting")[-1]
      or "Rebooting" not in btxt, required=True)
check("no stack overflow", "stack overflow" not in btxt.lower())
check("filesystem mounted", "/sesame mounted" in btxt)
check("commands registered", "commands registered" in btxt)
check("wifi online", "net: online" in btxt or "setup portal" in btxt)
check("web server up", "web: web interface up" in btxt, required=False)
check("camera", "camera: cam init ok" in btxt,
      "GPIO 4 held low — known hardware fault" if "cam init ok" not in btxt else "",
      required=False)

print("\nconsole")
out = run("help")
check("help lists commands", "sysinfo" in out and "py" in out)

out = run("sysinfo")
check("sysinfo reports chip", "ESP32-S3" in out)
check("memory bars render", "█" in out or "·" in out)

out = run("df")
check("filesystem has space", "KB free" in out)

# NOTE ON TRANSPORTS. The console is line-based: linenoise hands over one line
# at a time, so a real newline ENDS the command and multi-line input cannot be
# typed here. That is a property of the terminal, not a bug in the parser.
# Multi-line `write` and `py` are exercised over HTTP below, where the whole
# request arrives as a single string — which is how the agent sends them too.
print("\npython (single line, console)")
out = run('py print(6*7)', 3)
check("evaluates", "42" in out, out.strip()[:40])
out = run('py import time; print("tick", time.ticks_ms() > 0)', 3)
check("time module", "True" in out, out.strip()[:40])

print("\nfilesystem (console)")
run('write /sesame/_smoke.txt hello from the smoke test', 3)
out = run("cat /sesame/_smoke.txt")
check("write stores the body", "hello from the smoke test" in out, repr(out[:40]))
out = run("ls")
check("file appears in ls", "_smoke.txt" in out)
run("rm /sesame/_smoke.txt")

print("\nhardware")
out = run("gpio get 5 up")
check("gpio reads with a pull", "(pull-up)" in out, out.strip()[:30])
out = run("i2c scan", 4)
check("i2c scan completes", "device" in out, out.strip().splitlines()[-1][:50] if out else "")

print("\nnetwork")
out = run("wifi status")
m = re.search(r"ip\s+(\d+\.\d+\.\d+\.\d+)", out)
ip = m.group(1) if m else ""
check("has an address", bool(ip) and ip != "0.0.0.0", ip)

out = run("ssh")
check("ssh server running", "port 22" in out, required=False)

s.close()

host = A.host or ip
if host and host != "0.0.0.0" and urlopen:
    print(f"\nhttp ({host})")
    try:
        # A browser-sized header set: this is what caught the 1KB header limit
        # that made curl work and Chrome fail.
        req = Request(f"http://{host}/", headers={
            "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
                          "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
            "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"
                      "image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7",
            "Accept-Language": "en-GB,en-US;q=0.9,en;q=0.8",
            "sec-ch-ua": '"Google Chrome";v="131", "Chromium";v="131", "Not_A Brand";v="24"',
            "sec-ch-ua-platform": '"macOS"',
            "Sec-Fetch-Dest": "document", "Sec-Fetch-Mode": "navigate",
            "Sec-Fetch-Site": "none", "Sec-Fetch-User": "?1",
        })
        body = urlopen(req, timeout=15).read()
        check("page loads with browser headers", len(body) > 5000, f"{len(body)} bytes")
    except Exception as e:
        check("page loads with browser headers", False, str(e)[:60])

    try:
        body = urlopen(f"http://{host}/api/status", timeout=10).read().decode()
        check("status endpoint", '"link"' in body)
    except Exception as e:
        check("status endpoint", False, str(e)[:60])

    try:
        # Percent-encoded, the way the browser sends it. This is the exact bug
        # that made every download fail with "outside the working directory".
        urlopen(f"http://{host}/file?p=%2Fsesame%2Fnope.txt", timeout=10)
        check("encoded path reaches the handler", False, "expected 404")
    except Exception as e:
        check("encoded path reaches the handler", "404" in str(e), str(e)[:40])

    # Multi-line raw commands, delivered as one string the way the agent and the
    # browser send them. This is the shape that used to create a file literally
    # named "primes.py\nfor" and cost the agent twelve tool calls of workarounds.
    import json
    script = 'write /sesame/_smoke.py\nfor i in range(3):\n    print("n", i)\n'
    try:
        req = Request(f"http://{host}/api/exec", method="POST",
                      data=json.dumps({"command": script}).encode(),
                      headers={"Content-Type": "application/json"})
        urlopen(req, timeout=20).read()
        req = Request(f"http://{host}/api/exec", method="POST",
                      data=json.dumps({"command": "cat /sesame/_smoke.py"}).encode(),
                      headers={"Content-Type": "application/json"})
        body = urlopen(req, timeout=20).read().decode()
        check("multi-line write keeps newlines", body.count("\\n") >= 2 or body.count("\n") >= 2,
              repr(body[:44]))
        check("multi-line write keeps indentation", "    print" in body)
        req = Request(f"http://{host}/api/exec", method="POST",
                      data=json.dumps({"command": "py -f /sesame/_smoke.py"}).encode(),
                      headers={"Content-Type": "application/json"})
        body = urlopen(req, timeout=20).read().decode()
        check("runs the written script", "n 0" in body and "n 2" in body, repr(body[:44]))
        req = Request(f"http://{host}/api/exec", method="POST",
                      data=json.dumps({"command": "rm /sesame/_smoke.py"}).encode(),
                      headers={"Content-Type": "application/json"})
        urlopen(req, timeout=10).read()
    except Exception as e:
        check("multi-line over http", False, str(e)[:60])

print()
if fails:
    print(f"FAILED: {len(fails)} — " + ", ".join(fails))
    sys.exit(1)
print("all required checks passed" + (f" ({len(warns)} warning: {', '.join(warns)})" if warns else ""))
