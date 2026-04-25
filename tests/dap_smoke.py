#!/usr/bin/env python3
"""DAP smoke test (Phases 2-5a).

Spawns krkrz with `-dap=<port>`, connects, and exchanges the basic
debugging protocol cycle. All requests are sent synchronously
(send → wait for matching response) so the test stays simple.

Verified:
  Phase 2: initialize/initialized/attach/disconnect basic loop
  Phase 3: evaluate "1+2" → "3", scopes/variables empty when not stopped
  Phase 4: next/stepIn/stepOut respond success, exceptionBreakpointFilters
           advertised in initialize capabilities
  Phase 5a: evaluate "[1,2,3]" returns variablesReference > 0, and a
            follow-up variables(ref) returns 3 child elements
"""

import json
import os
import socket
import subprocess
import sys
import time

PORT = 6635


def parse_messages(buf):
    msgs = []
    while True:
        sep = b"\r\n\r\n"
        i = buf.find(sep)
        if i < 0:
            break
        header = buf[:i].decode("ascii", errors="replace")
        cl = None
        for line in header.split("\r\n"):
            if line.lower().startswith("content-length:"):
                cl = int(line.split(":", 1)[1].strip())
                break
        if cl is None:
            buf = buf[i + len(sep):]
            continue
        body_start = i + len(sep)
        if len(buf) < body_start + cl:
            break
        body = buf[body_start:body_start + cl]
        msgs.append(json.loads(body.decode("utf-8")))
        buf = buf[body_start + cl:]
    return msgs, buf


class DAPClient:
    def __init__(self, sock, proc):
        self.sock = sock
        self.proc = proc
        self.recv_buf = b""
        self.events = []
        self.next_seq = 1
        sock.settimeout(2.0)

    def _raw_send(self, obj):
        body = json.dumps(obj).encode("utf-8")
        framed = b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n\r\n" + body
        self.sock.sendall(framed)

    def send_request(self, command, args=None):
        seq = self.next_seq
        self.next_seq += 1
        msg = {"seq": seq, "type": "request", "command": command}
        if args is not None:
            msg["arguments"] = args
        self._raw_send(msg)
        return seq

    def wait_response(self, seq, timeout=30.0):
        deadline = time.time() + timeout
        last_proc_check = time.time()
        while time.time() < deadline:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                if time.time() - last_proc_check > 1.0:
                    rc = self.proc.poll()
                    if rc is not None:
                        print(f"[smoke] proc exited rc={rc}")
                        return None
                    last_proc_check = time.time()
                continue
            if not chunk:
                print("[smoke] socket closed by peer")
                return None
            self.recv_buf += chunk
            msgs, self.recv_buf = parse_messages(self.recv_buf)
            for m in msgs:
                if m.get("type") == "event":
                    self.events.append(m)
                    print(f"[smoke] event: {m.get('event')}")
                elif m.get("type") == "response" and m.get("request_seq") == seq:
                    print(f"[smoke] response seq={seq} cmd={m.get('command')} success={m.get('success')}")
                    return m
        return None

    def request(self, command, args=None, timeout=30.0):
        """Send and wait for the response. Returns the full response dict, or None on timeout."""
        seq = self.send_request(command, args)
        return self.wait_response(seq, timeout)


def wait_for_port(port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    return None


def main():
    if len(sys.argv) < 3:
        print("usage: dap_smoke.py <krkrz.exe> <data_dir>")
        return 2

    exe, data_dir = sys.argv[1], sys.argv[2]
    print(f"[smoke] starting {exe} -dap={PORT} {data_dir}")

    proc = subprocess.Popen(
        [exe, f"-dap={PORT}", data_dir],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    failures = []

    try:
        sock = wait_for_port(PORT, timeout=10.0)
        if sock is None:
            print("[smoke] FAIL: server did not listen within 10s")
            return 1
        print(f"[smoke] connected to {PORT}")

        client = DAPClient(sock, proc)

        # ----- Phase 2 + 4 + 5b: initialize / capabilities -----
        r = client.request("initialize", {"clientID": "smoke", "adapterID": "krkrz"})
        if not r or not r.get("success"):
            failures.append("initialize")
        else:
            caps = r.get("body") or {}
            if "exceptionBreakpointFilters" not in caps:
                failures.append("initialize.exceptionBreakpointFilters")
            if not caps.get("supportsConditionalBreakpoints"):
                failures.append("initialize.supportsConditionalBreakpoints=true")
            if not caps.get("supportsLogPoints"):
                failures.append("initialize.supportsLogPoints=true")
            if not any(e.get("event") == "initialized" for e in client.events):
                # 'initialized' は initialize response の直後に来るはず
                # response 受信時点ではまだ event を貪欲に集めきれてない可能性があるので少し待つ
                time.sleep(0.5)
                # poll any pending recv
                client.sock.setblocking(False)
                try:
                    chunk = client.sock.recv(4096)
                    if chunk:
                        client.recv_buf += chunk
                        msgs, client.recv_buf = parse_messages(client.recv_buf)
                        for m in msgs:
                            if m.get("type") == "event":
                                client.events.append(m)
                except (BlockingIOError, OSError):
                    pass
                client.sock.setblocking(True)
                client.sock.settimeout(2.0)
            if not any(e.get("event") == "initialized" for e in client.events):
                failures.append("initialized event")

        # ----- Phase 2: attach -----
        r = client.request("attach", {})
        if not r or not r.get("success"):
            failures.append("attach")

        # ----- Phase 3: evaluate "1+2" -----
        r = client.request("evaluate", {"expression": "1+2", "context": "repl"})
        if not r or not r.get("success"):
            failures.append("evaluate(1+2)")
        else:
            result = (r.get("body") or {}).get("result")
            if result is None or "3" not in str(result):
                failures.append(f"evaluate(1+2) result={result!r}")

        # ----- Phase 3: scopes / variables (not stopped → empty) -----
        r = client.request("scopes", {"frameId": 1})
        if not r or not r.get("success"):
            failures.append("scopes")
        r = client.request("variables", {"variablesReference": 1})
        if not r or not r.get("success"):
            failures.append("variables(1)")

        # ----- Phase 4: step 系 -----
        for cmd in ("next", "stepIn", "stepOut"):
            r = client.request(cmd, {"threadId": 1})
            if not r or not r.get("success"):
                failures.append(cmd)

        # ----- Phase 5a: evaluate array → child expansion -----
        r = client.request("evaluate", {"expression": "[10,20,30]", "context": "repl"})
        if not r or not r.get("success"):
            failures.append("evaluate([10,20,30])")
        else:
            body = r.get("body") or {}
            ref = body.get("variablesReference", 0)
            if not isinstance(ref, int) or ref <= 0:
                failures.append(f"evaluate(array).variablesReference={ref!r}")
            else:
                r2 = client.request("variables", {"variablesReference": ref})
                if not r2 or not r2.get("success"):
                    failures.append("variables(ref)")
                else:
                    children = (r2.get("body") or {}).get("variables") or []
                    if len(children) != 3:
                        failures.append(f"variables(ref): got {len(children)} children, expected 3")
                    else:
                        # 各要素の name は "[0]", "[1]", "[2]"、value は "10", "20", "30"
                        expected_names  = ["[0]", "[1]", "[2]"]
                        expected_values = ["10", "20", "30"]
                        for i, child in enumerate(children):
                            if child.get("name") != expected_names[i]:
                                failures.append(f"child[{i}].name={child.get('name')!r}")
                            if str(expected_values[i]) not in str(child.get("value", "")):
                                failures.append(f"child[{i}].value={child.get('value')!r} expected {expected_values[i]}")

        # ----- Phase 2: disconnect -----
        r = client.request("disconnect", {})
        if not r or not r.get("success"):
            failures.append("disconnect")

        sock.close()

        if failures:
            print(f"[smoke] FAIL: {failures}")
            return 1
        print("[smoke] PASS: all phases verified")
        return 0

    finally:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
