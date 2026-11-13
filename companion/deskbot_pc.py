#!/usr/bin/env python3
"""
DeskBot PC helper  -  lets your DeskBot control this computer over WiFi.

* Windows, Python 3.8+, no extra packages needed.
* Only a fixed list of actions exists (see ACTIONS).  No free-form commands, no typing, no mouse.
* Only devices on your own network (private IP addresses) are answered, and every request needs the token.

Run:   python deskbot_pc.py
Set your token once:   setx DESKBOT_TOKEN "your-api-token"     (same value as API_TOKEN in secrets.h)
Windows may ask to allow Python through the firewall: allow it on PRIVATE networks only.
"""
import ctypes
import hmac
import ipaddress
import json
import os
import platform
import socket
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

TOKEN = os.environ.get("DESKBOT_TOKEN", "").strip().strip('"')   # tolerate stray spaces / quotes
PORT = int(os.environ.get("DESKBOT_PORT", "8765"))
ONLY_IP = os.environ.get("DESKBOT_IP", "")          # optional: accept this one address only
IS_WIN = platform.system() == "Windows"
DRY = (not IS_WIN) or os.environ.get("DESKBOT_DRY") == "1"   # on other systems: log only

# ---------------- actions ----------------
VK = {"playpause": 0xB3, "next": 0xB0, "prev": 0xB1, "stop": 0xB2,
      "mute": 0xAD, "vol_down": 0xAE, "vol_up": 0xAF}
REPEAT = {"vol_up": 5, "vol_down": 5}               # one key press = 2 %, so 5 presses = 10 %


def press_key(vk, times=1):
    u = ctypes.windll.user32
    for _ in range(times):
        u.keybd_event(vk, 0, 1, 0)                  # key down (extended key)
        u.keybd_event(vk, 0, 3, 0)                  # key up
        time.sleep(0.01)


def do_lock():
    ctypes.windll.user32.LockWorkStation()


def do_sleep():
    # note: if hibernation is enabled in Windows this hibernates instead of sleeping
    subprocess.Popen(["rundll32.exe", "powrprof.dll,SetSuspendState", "0,1,0"])


ACTIONS = {name: (lambda v=vk, n=REPEAT.get(name, 1): press_key(v, n)) for name, vk in VK.items()}
ACTIONS["lock"] = do_lock
ACTIONS["sleep"] = do_sleep

# ---------------- server ----------------
fails = {}                                           # ip -> (count, locked_until)


def allowed_ip(ip):
    if ONLY_IP:
        return ip == ONLY_IP
    try:
        a = ipaddress.ip_address(ip)
        return a.is_private or a.is_loopback
    except ValueError:
        return False


class Handler(BaseHTTPRequestHandler):
    server_version = "DeskBotPC"

    def log_message(self, fmt, *args):
        print("[%s] %s %s" % (time.strftime("%H:%M:%S"), self.client_address[0], fmt % args))

    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        ip = self.client_address[0]
        if not allowed_ip(ip):
            return self.reply(403, {"ok": 0, "err": "forbidden"})
        count, until = fails.get(ip, (0, 0))
        if until > time.time():
            return self.reply(429, {"ok": 0, "err": "locked"})
        u = urlparse(self.path)
        q = parse_qs(u.query)
        sent = self.headers.get("X-Token") or (q.get("token") or [""])[0]
        if not TOKEN or not hmac.compare_digest(sent.encode(), TOKEN.encode()):
            count += 1
            print("   TOKEN MISMATCH: DeskBot sent %d characters, this helper expects %d" % (len(sent), len(TOKEN)))
            if count >= 5:
                print("   5 wrong tries: blocking %s for 30 seconds" % ip)
            fails[ip] = (0, time.time() + 30) if count >= 5 else (count, 0)
            return self.reply(401, {"ok": 0, "err": "token"})
        fails.pop(ip, None)

        if u.path == "/status":
            return self.reply(200, {"ok": 1, "host": socket.gethostname(), "os": platform.system(),
                                    "dry": DRY, "actions": sorted(ACTIONS)})
        if u.path == "/cmd":
            name = (q.get("do") or [""])[0]
            if name not in ACTIONS:
                return self.reply(400, {"ok": 0, "err": "unknown action"})
            if DRY:
                print("   (dry run) would do:", name)
            else:
                try:
                    ACTIONS[name]()
                except Exception as e:                       # never crash the helper
                    return self.reply(500, {"ok": 0, "err": str(e)})
            return self.reply(200, {"ok": 1, "did": name})
        self.reply(404, {"ok": 0, "err": "not found"})


def main():
    if not TOKEN:
        sys.exit("Set the token first:  setx DESKBOT_TOKEN \"your-api-token\"  (then open a new terminal)")
    ip = "?"
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
        s.close()
    except OSError:
        pass
    print("DeskBot PC helper listening on %s:%d  (%s)" % (ip, PORT, "DRY RUN" if DRY else "live"))
    print("Put this in secrets.h:   #define PC_HOST \"%s\"" % ip)
    try:
        ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        print("bye")


if __name__ == "__main__":
    main()
