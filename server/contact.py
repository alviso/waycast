#!/usr/bin/env python3
"""Waycast contact intake — tiny stdlib service behind nginx.

  POST /contact   {email, message, [name]}  -> append to a JSONL file
  GET  /admin     -> HTML table of submissions (newest first)
                     (basic-auth is enforced by nginx, not here)
  GET  /healthz   -> ok + count

Deliberately minimal: no database, no deps. Submissions land in one
append-only file the operator reads via /admin. Size- and rate-limited
so a public form can't be used to flood disk.
"""

import html
import json
import os
import pathlib
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DATA = pathlib.Path(os.environ.get("CONTACT_FILE", "/data/contact.jsonl"))
DATA.parent.mkdir(parents=True, exist_ok=True)
MAX_BODY = 8 * 1024
_last = {}  # ip -> last-post monotonic time (simple throttle)


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code, body, ctype="text/plain; charset=utf-8"):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path == "/healthz":
            n = sum(1 for _ in DATA.open()) if DATA.exists() else 0
            return self._send(200, f"ok\nsubmissions {n}\n")
        if self.path.rstrip("/") == "/admin":
            return self._admin()
        self._send(404, "not found\n")

    def do_POST(self):
        if self.path != "/contact":
            return self._send(404, "not found\n")
        ip = self.headers.get("X-Real-IP", self.client_address[0])
        now = time.monotonic()
        if now - _last.get(ip, 0) < 5:      # 1 msg / 5 s / IP
            return self._send(429, "slow down\n")
        try:
            n = int(self.headers.get("Content-Length", 0))
            if n <= 0 or n > MAX_BODY:
                return self._send(413, "too big\n")
            data = json.loads(self.rfile.read(n) or b"{}")
        except Exception:
            return self._send(400, "bad request\n")

        email = str(data.get("email", "")).strip()[:200]
        msg = str(data.get("message", "")).strip()[:2000]
        name = str(data.get("name", "")).strip()[:120]
        if "@" not in email or not msg:
            return self._send(400, "need email + message\n")

        rec = {"t": time.strftime("%Y-%m-%d %H:%M:%S"),
               "ip": ip, "name": name, "email": email, "message": msg}
        with DATA.open("a") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
        _last[ip] = now
        print(f"[contact] {email}: {msg[:60]}", flush=True)
        self._send(200, '{"ok":true}', "application/json")

    def _admin(self):
        rows = []
        if DATA.exists():
            for line in reversed(DATA.read_text().splitlines()):
                try:
                    r = json.loads(line)
                except Exception:
                    continue
                e = lambda k: html.escape(str(r.get(k, "")))
                rows.append(
                    f"<tr><td class=t>{e('t')}</td><td>{e('name')}</td>"
                    f"<td><a href='mailto:{e('email')}'>{e('email')}</a></td>"
                    f"<td>{e('message')}</td><td class=ip>{e('ip')}</td></tr>")
        page = (
            "<!doctype html><meta charset=utf-8><title>Waycast · contact</title>"
            "<style>body{background:#101318;color:#E7ECF2;font:14px/1.5 "
            "-apple-system,Segoe UI,Roboto,sans-serif;margin:24px}"
            "h1{font-size:18px}h1 b{color:#0CA678}"
            "table{border-collapse:collapse;width:100%;margin-top:14px}"
            "td,th{border-bottom:1px solid #262C36;padding:8px 10px;"
            "text-align:left;vertical-align:top}th{color:#8B95A3;font-size:12px;"
            "text-transform:uppercase;letter-spacing:.05em}"
            ".t,.ip{color:#8B95A3;white-space:nowrap;font:12px ui-monospace,Menlo}"
            "td:nth-child(4){max-width:480px}a{color:#F59F00}"
            ".empty{color:#5C6672;margin-top:20px}</style>"
            f"<h1><b>Waycast</b> · contact inbox ({len(rows)})</h1>"
            + ("<table><tr><th>when</th><th>name</th><th>email</th>"
               "<th>message</th><th>ip</th></tr>" + "".join(rows) + "</table>"
               if rows else "<div class=empty>No messages yet.</div>"))
        self._send(200, page, "text/html; charset=utf-8")

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(os.environ.get("PORT", 8484))
    ThreadingHTTPServer(("", port), H).serve_forever()
