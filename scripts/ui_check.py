#!/usr/bin/env python3
"""Drives the visualizer in headless Firefox to check the UI and take screenshots.

Development helper, not needed to build or run the project. It talks to
Firefox through its built-in Marionette remote protocol using only the Python
standard library, so no Selenium or geckodriver is required.

Usage: scripts/ui_check.py http://127.0.0.1:8100 [output_dir]

For every scenario below it opens a deep link, waits until the page has
compiled, reports any UI error shown in the error banner, evaluates a small
check expression in the page, and saves a screenshot.
"""

import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

# (name, url fragment, JavaScript check that must return true, wait for GCC inspector?)
SCENARIOS = [
    ("lab-minic", "part=lab", "(() => { const stages = document.querySelectorAll('#part-lab .stage-btn'); if (stages.length !== 6) return false; stages[4].click(); return document.querySelectorAll('#part-lab .lab-event').length > 0 && !document.getElementById('part-lab').hidden && document.getElementById('part-c').hidden; })()", False,
     "(() => { const opt = document.querySelector('#part-lab select[aria-label=Optimization]'); opt.value = '1'; opt.dispatchEvent(new Event('change')); document.querySelector('#part-lab .btn.primary').click(); return true; })()"),
    ("lab-expression", "part=lab&compiler=expression", "(() => { const stages = document.querySelectorAll('#part-lab .stage-btn'); if (stages.length !== 4) return false; document.querySelectorAll('#part-lab .stage-panel .toolbar button')[1].click(); if (!document.querySelector('#part-lab .ln.marked')) return false; stages[3].click(); return document.querySelector('#part-lab details pre').textContent.includes('PUSH 11'); })()", False,
     "(document.querySelector('#part-lab .btn.primary').click(), true)"),
    ("lab-error", "part=lab&compiler=expression", "document.querySelector('#part-lab .stage-btn.fail')?.textContent.includes('AST') && document.querySelectorAll('#part-lab .stage-btn.skip').length === 2", False,
     "(() => { const source = document.querySelector('#part-lab textarea'); source.value = 'unknown()'; source.dispatchEvent(new Event('input')); document.querySelector('#part-lab .btn.primary').click(); return true; })()"),
    ("lab-replay", "part=lab&compiler=expression", "(() => { document.querySelectorAll('#part-lab .stage-btn')[3].click(); const title = () => document.querySelector('#part-lab h3').textContent; const before = title(); document.querySelectorAll('#part-lab .stage-panel .toolbar button')[1].click(); if (title() === before) return false; document.querySelectorAll('#part-lab .stage-panel .toolbar button')[0].click(); if (title() !== before) return false; const slider = document.querySelector('#part-lab input[type=range]'); slider.value = 0; slider.dispatchEvent(new Event('input')); if (!document.querySelectorAll('#part-lab .stage-panel .toolbar button')[0].disabled) return false; document.querySelector('#part-lab textarea').dispatchEvent(new Event('input')); return document.querySelectorAll('#part-lab .stage-btn, #part-lab .ln.marked').length === 0; })()", False,
     "(document.querySelector('#part-lab .btn.primary').click(), true)"),
    ("c-tokens", "part=c&example=01_minimal.c&stage=tokens", "document.querySelectorAll('.chip').length > 20", False),
    ("c-ast", "part=c&example=01_minimal.c&stage=ast", "document.querySelectorAll('.tree-node').length >= 10", False),
    ("c-semantic-errors", "part=c&example=err_semantic.c", "document.querySelectorAll('.diag').length == 6 && document.querySelector('.stage-btn.fail').textContent.includes('Semantic')", False),
    ("c-syntax-error", "part=c&example=err_syntax.c", "document.querySelector('.stage-btn.fail').textContent.includes('Parser') && document.querySelectorAll('.stage-btn.skip').length == 6", False),
    ("c-ir", "part=c&example=03_control_flow.c&stage=ir", "document.querySelectorAll('table.grid tbody tr').length > 10", False),
    ("c-optimizer", "part=c&example=05_optimizer.c&stage=opt&opt=1", "document.querySelectorAll('.log-entry').length > 10", False),
    ("c-assembly", "part=c&example=04_functions.c&stage=asm", "document.querySelectorAll('.asm-group').length > 20", False),
    ("c-elf", "part=c&example=04_functions.c&stage=link&sub=elf", "document.querySelectorAll('.elf-file').length == 2", True),
    ("c-gcc-tools", "part=c&example=04_functions.c&stage=link&sub=tools&tool=readelf -r", "document.querySelector('.subtab-body pre.code').textContent.includes('R_X86_64')", True),
    ("c-compare", "part=c&example=01_minimal.c&stage=link&sub=compare", "document.querySelectorAll('.subtab-body .asm-list').length == 3", True),
    ("c-run", "part=c&example=04_functions.c&stage=run", "document.querySelector('.stage-body pre.code').textContent.includes('120')", False),
]


class Marionette:
    def __init__(self, port):
        deadline = time.time() + 60
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=60)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.3)
        self.buf = b""
        self.next_id = 0
        self._read()  # server hello

    def _read(self):
        while b":" not in self.buf:
            self.buf += self.sock.recv(65536)
        length, _, rest = self.buf.partition(b":")
        length = int(length)
        while len(rest) < length:
            rest += self.sock.recv(1 << 20)
        self.buf = rest[length:]
        return json.loads(rest[:length])

    def send(self, name, params=None):
        self.next_id += 1
        data = json.dumps([0, self.next_id, name, params or {}]).encode()
        self.sock.sendall(str(len(data)).encode() + b":" + data)
        reply = self._read()
        if reply[2]:
            raise RuntimeError(f"{name}: {reply[2]}")
        return reply[3]

    def js(self, script):
        return self.send("WebDriver:ExecuteScript", {"script": "return (" + script + ");", "args": []})["value"]


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    base = sys.argv[1].rstrip("/")
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "docs/screenshots"
    only = sys.argv[3:] if len(sys.argv) > 3 else None
    os.makedirs(out_dir, exist_ok=True)
    firefox = shutil.which("firefox")
    if not firefox:
        raise SystemExit("firefox not found")
    port = 2900 + os.getpid() % 500
    profile = tempfile.mkdtemp(prefix="cv-ff-")
    with open(os.path.join(profile, "user.js"), "w") as f:
        f.write(f'user_pref("marionette.port", {port});\n')
    proc = subprocess.Popen([firefox, "--headless", "--marionette", "--no-remote", "--profile", profile,
                             "--width", "1500", "--height", "950"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    failures = 0
    try:
        m = Marionette(port)
        m.send("WebDriver:NewSession", {"capabilities": {}})
        for scenario in SCENARIOS:
            name, fragment, check, wait_gcc = scenario[:4]
            action = scenario[4] if len(scenario) > 4 else None
            if only and name not in only:
                continue
            m.send("WebDriver:Navigate", {"url": "about:blank"})
            m.send("WebDriver:Navigate", {"url": f"{base}/#{fragment}"})
            deadline = time.time() + 40
            ready = False
            while time.time() < deadline:
                ready = m.js("document.body.dataset.ready === '1'" + (" && Number(document.body.dataset.gcc) >= 1" if wait_gcc else ""))
                if ready:
                    break
                time.sleep(0.2)
            time.sleep(0.3)
            if ready and action:
                m.js(action)
                deadline = time.time() + 15
                while time.time() < deadline and m.js("document.querySelector('#part-lab .btn.primary').disabled"):
                    time.sleep(0.1)
            banner = m.js("document.getElementById('error-banner').hidden ? '' : document.getElementById('error-banner').textContent")
            try:
                ok = bool(m.js(check))
            except RuntimeError as e:
                ok = False
                banner = banner or str(e)
            status = "PASS" if (ready and ok and not banner) else "FAIL"
            failures += status == "FAIL"
            print(f"{status}  {name:20s} ready={ready} check={ok} {('UI error: ' + banner) if banner else ''}")
            png = m.send("WebDriver:TakeScreenshot", {"full": True})["value"]
            with open(os.path.join(out_dir, name + ".png"), "wb") as f:
                f.write(base64.b64decode(png))
        try:
            m.send("Marionette:Quit", {"flags": ["eForceQuit"]})
        except Exception:
            pass
    finally:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(profile, ignore_errors=True)
    print(f"{failures} scenario(s) failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
