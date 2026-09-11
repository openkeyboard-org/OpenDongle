#!/usr/bin/env python3
"""ppk2d: own the Nordic PPK2, keep the DUT path closed, stream 100 kS/s, answer stats.

The PPK2 opens its series switch whenever the host serial session ends, so the process
that holds the port is what keeps the DUT powered. This daemon holds it for the whole
bench session; scripts talk to it over a unix socket.

  serve  [--port P] [--sock F] [--log DIR] [--ring-s N] [--allow-source]
  status | marks | mark LABEL | power on|off | quit
  stats  (--last S | --since LABEL [--until LABEL]) [--json]
  raw    --seconds N --out FILE          full-rate CSV of the last N seconds
"""
import argparse, array, bisect, json, os, re, signal, socket, statistics, sys, threading, time
from ppk2_api.ppk2_api import PPK2_API, PPK2_Command
try:
    import numpy as np
except ImportError:
    np = None

FS = 100_000                      # PPK2 sample rate
DEF_SOCK = os.path.expanduser("~/.ppk2d.sock")   # AF_UNIX paths are limited to ~104 chars on macOS

def read_metadata(ppk, deadline=3.0):
    """Accumulate until END (the library's own loop drops chunks and loses the calibration)."""
    ppk._write_serial((PPK2_Command.GET_META_DATA,))
    buf = b""; t0 = time.time()
    while time.time() - t0 < deadline and b"END" not in buf:
        buf += ppk.ser.read(ppk.ser.in_waiting or 1)
    if b"END" not in buf:
        raise RuntimeError("no metadata from PPK2")
    return buf.decode("utf-8", "replace")

class Store:
    """1 ms buckets forever (mean/min/max/count/digital) plus a full-rate ring of the last ring_s seconds."""
    def __init__(self, ring_s):
        self.lock = threading.Lock()
        self.b_t = array.array("d"); self.b_mean = array.array("f"); self.b_min = array.array("f")
        self.b_max = array.array("f"); self.b_n = array.array("H"); self.b_dig = array.array("B")
        self.ring_len = ring_s * FS; self.ring = array.array("f", bytes(4 * self.ring_len)); self.ring_w = 0
        self.ring_total = 0; self.ring_t_last = 0.0
        self.total = 0; self.marks = {}; self.t_start = time.time()
        self._acc = [0, 0.0, float("inf"), float("-inf"), 0]   # n, sum, min, max, dig
    def push(self, samples, bits, t_end):
        n = len(samples)
        if n == 0: return
        with self.lock:
            t0 = t_end - n / FS
            for i, v in enumerate(samples):
                a = self._acc; a[0] += 1; a[1] += v
                if v < a[2]: a[2] = v
                if v > a[3]: a[3] = v
                if i < len(bits): a[4] = bits[i]
                if a[0] >= 100:
                    self.b_t.append(t0 + i / FS); self.b_mean.append(a[1] / a[0]); self.b_min.append(a[2])
                    self.b_max.append(a[3]); self.b_n.append(a[0]); self.b_dig.append(a[4] & 0xFF)
                    a[0] = 0; a[1] = 0.0; a[2] = float("inf"); a[3] = float("-inf")
            # ring
            w = self.ring_w
            for v in samples:
                self.ring[w] = v; w += 1
                if w == self.ring_len: w = 0
            self.ring_w = w; self.ring_total += n; self.ring_t_last = t_end; self.total += n
    def window(self, t0, t1):
        i0 = bisect.bisect_left(self.b_t, t0); i1 = bisect.bisect_right(self.b_t, t1)
        return i0, i1
    def stats(self, t0, t1):
        with self.lock:
            i0, i1 = self.window(t0, t1)
            if i1 <= i0: return {"error": "no samples in window", "t0": t0, "t1": t1}
            means = self.b_mean[i0:i1]; ns = self.b_n[i0:i1]
            tot = sum(ns); mean = sum(m * c for m, c in zip(means, ns)) / tot
            out = {"t0": t0, "t1": t1, "seconds": round(self.b_t[i1-1] - self.b_t[i0] + 0.001, 3),
                   "samples": tot, "buckets_ms": i1 - i0,
                   "mean_mA": mean / 1000, "min_mA": min(self.b_min[i0:i1]) / 1000, "max_mA": max(self.b_max[i0:i1]) / 1000}
            sm = sorted(means); k = len(sm)
            out["p05_mA_1ms"] = sm[k // 20] / 1000; out["p50_mA_1ms"] = sm[k // 2] / 1000; out["p95_mA_1ms"] = sm[19 * k // 20] / 1000
            # exact percentiles from the full-rate ring when the window is still in it
            ring_t_first = self.ring_t_last - min(self.ring_total, self.ring_len) / FS
            if np is not None and t0 >= ring_t_first and t1 <= self.ring_t_last:
                n_back = int((self.ring_t_last - t0) * FS); n_win = int((t1 - t0) * FS)
                n_back = min(n_back, self.ring_len); n_win = max(0, min(n_win, n_back))
                start = (self.ring_w - n_back) % self.ring_len
                r = np.frombuffer(self.ring, dtype=np.float32)
                idx = (np.arange(n_win) + start) % self.ring_len
                v = r[idx]
                if v.size:
                    p = np.percentile(v, [5, 50, 95])
                    out.update({"p05_mA": float(p[0]) / 1000, "p50_mA": float(p[1]) / 1000, "p95_mA": float(p[2]) / 1000, "exact_samples": int(v.size)})
            return out
    def raw(self, seconds):
        with self.lock:
            n = min(int(seconds * FS), self.ring_total, self.ring_len)
            start = (self.ring_w - n) % self.ring_len
            out = array.array("f")
            if start + n <= self.ring_len: out.extend(self.ring[start:start + n])
            else: out.extend(self.ring[start:]); out.extend(self.ring[:(start + n) % self.ring_len])
            return out, self.ring_t_last - n / FS

class Daemon:
    def __init__(self, a):
        self.a = a; self.store = Store(a.ring_s); self.quit = threading.Event(); self.rate = 0.0; self.err = None
        if os.path.exists(a.sock): os.unlink(a.sock)
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); self.srv.bind(a.sock); self.srv.listen(8); self.srv.settimeout(0.5)
        self.ppk = PPK2_API(a.port, timeout=0.02)
        meta = read_metadata(self.ppk); self.ppk._parse_metadata(meta)
        self.mode = int(re.search(r"mode: (\d+)", meta).group(1))
        if self.mode != 1 and not a.allow_source:
            raise SystemExit("PPK2 reports mode %d (not ampere meter); refusing without --allow-source" % self.mode)
        self.ppk.mode = "AMPERE_MODE" if self.mode == 1 else "SOURCE_MODE"; self.ppk.current_vdd = 3300  # guard only; never REGULATOR_SET in ampere mode
        self.ppk.toggle_DUT_power("ON")                     # close the series switch: the app does this on connect
        self.ppk.start_measuring(); time.sleep(0.3); self.ppk.get_data()
        self.logf = None
        if a.log:
            os.makedirs(a.log, exist_ok=True)
            self.logf = open(os.path.join(a.log, time.strftime("ppk2-%Y%m%d-%H%M%S.csv")), "w"); self.logf.write("t_unix,mean_mA,min_mA,max_mA,samples\n")
    def fetch(self):
        last_log = time.time(); acc_n = 0; acc_s = 0.0; acc_min = 1e12; acc_max = -1e12; rate_n = 0; rate_t = time.time()
        while not self.quit.is_set():
            try:
                d = self.ppk.get_data()
            except Exception as e:
                self.err = repr(e); break
            if d:
                s, bits = self.ppk.get_samples(d); now = time.time()
                self.store.push(s, bits, now)
                rate_n += len(s)
                if s:
                    acc_n += len(s); acc_s += sum(s); mn = min(s); mx = max(s)
                    if mn < acc_min: acc_min = mn
                    if mx > acc_max: acc_max = mx
            now = time.time()
            if now - rate_t >= 1.0:
                self.rate = rate_n / (now - rate_t); rate_n = 0; rate_t = now
            if self.logf and now - last_log >= 1.0 and acc_n:
                self.logf.write("%.3f,%.4f,%.4f,%.4f,%d\n" % (now, acc_s / acc_n / 1000, acc_min / 1000, acc_max / 1000, acc_n)); self.logf.flush()
                last_log = now; acc_n = 0; acc_s = 0.0; acc_min = 1e12; acc_max = -1e12
            time.sleep(0.001)
    def handle(self, req):
        c = req.get("cmd"); st = self.store; now = time.time()
        if c == "status":
            i1 = len(st.b_t); i0 = max(0, i1 - 1000)
            last = (sum(st.b_mean[i0:i1]) / max(1, i1 - i0) / 1000) if i1 > i0 else None
            return {"ok": True, "port": self.a.port, "mode": "ampere" if self.mode == 1 else "source", "dut_power": "on",
                    "uptime_s": round(now - st.t_start, 1), "samples": st.total, "rate_kSps": round(self.rate / 1000, 1),
                    "last_1s_mean_mA": last, "marks": st.marks, "fetch_error": self.err, "pid": os.getpid()}
        if c == "mark":
            st.marks[req["label"]] = now; return {"ok": True, "label": req["label"], "t": now}
        if c == "marks": return {"ok": True, "marks": st.marks}
        if c == "stats":
            if "last" in req:
                t1 = min(now, st.ring_t_last) if st.ring_t_last else now   # clamp to the newest sample so the exact path applies
                t0 = t1 - float(req["last"])
            else:
                if req["since"] not in st.marks: return {"ok": False, "error": "unknown mark %r" % req["since"]}
                t0 = st.marks[req["since"]]
                t1 = st.marks[req["until"]] if req.get("until") in st.marks else (min(now, st.ring_t_last) if st.ring_t_last else now)
            r = st.stats(t0, t1); r["ok"] = "error" not in r; return r
        if c == "power":
            self.ppk.toggle_DUT_power("ON" if req["state"] == "on" else "OFF"); st.marks["power_" + req["state"]] = now
            return {"ok": True, "dut_power": req["state"], "t": now}
        if c == "raw":
            v, t0 = st.raw(float(req["seconds"]))
            with open(req["out"], "w") as f:
                f.write("t_ms,uA\n"); f.writelines("%.2f,%.3f\n" % (i / FS * 1000, x) for i, x in enumerate(v))
            return {"ok": True, "samples": len(v), "t0_unix": t0, "out": req["out"]}
        if c == "quit":
            self.quit.set(); return {"ok": True, "note": "port will close: the PPK2 opens its switch and the DUT loses power"}
        return {"ok": False, "error": "unknown cmd %r" % c}
    def serve(self):
        srv = self.srv
        th = threading.Thread(target=self.fetch, daemon=True); th.start()
        while not self.quit.is_set():
            try: conn, _ = srv.accept()
            except socket.timeout: continue
            with conn:
                try:
                    req = json.loads(conn.makefile("r").readline() or "{}"); resp = self.handle(req)
                except Exception as e: resp = {"ok": False, "error": repr(e)}
                conn.sendall((json.dumps(resp) + "\n").encode())
        try: self.ppk.stop_measuring()
        except Exception: pass
        self.ppk.ser.close(); srv.close(); os.unlink(self.a.sock)

def client(sock, req):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try: s.connect(sock)
    except OSError as e: raise SystemExit("ppk2d not running (%s): %s" % (sock, e))
    s.sendall((json.dumps(req) + "\n").encode()); r = json.loads(s.makefile("r").readline()); s.close(); return r

def main():
    p = argparse.ArgumentParser(); sub = p.add_subparsers(dest="cmd", required=True)
    sv = sub.add_parser("serve"); sv.add_argument("--port", default=None); sv.add_argument("--sock", default=DEF_SOCK)
    sv.add_argument("--log", default=None); sv.add_argument("--ring-s", type=int, default=120); sv.add_argument("--allow-source", action="store_true")
    for n in ("status", "marks", "quit"): sub.add_parser(n).add_argument("--sock", default=DEF_SOCK)
    m = sub.add_parser("mark"); m.add_argument("label"); m.add_argument("--sock", default=DEF_SOCK)
    pw = sub.add_parser("power"); pw.add_argument("state", choices=["on", "off"]); pw.add_argument("--sock", default=DEF_SOCK)
    stt = sub.add_parser("stats"); stt.add_argument("--last", type=float); stt.add_argument("--since"); stt.add_argument("--until"); stt.add_argument("--json", action="store_true"); stt.add_argument("--sock", default=DEF_SOCK)
    rw = sub.add_parser("raw"); rw.add_argument("--seconds", type=float, required=True); rw.add_argument("--out", required=True); rw.add_argument("--sock", default=DEF_SOCK)
    a = p.parse_args()
    if a.cmd == "serve":
        if a.port is None:
            devs = PPK2_API.list_devices()
            if not devs: raise SystemExit("no PPK2 found")
            a.port = sorted(devs)[0]                      # macOS lists both CDC ports; the lower interface answers
        d = Daemon(a); signal.signal(signal.SIGTERM, lambda *_: d.quit.set()); d.serve(); return
    if a.cmd == "stats":
        req = {"cmd": "stats"}
        if a.last is not None: req["last"] = a.last
        elif a.since: req["since"] = a.since; req["until"] = a.until
        else: raise SystemExit("stats needs --last S or --since LABEL")
        r = client(a.sock, req)
        if a.json or not r.get("ok"): print(json.dumps(r)); return
        line = "%.1fs %d samples: mean %.3f mA | min %.3f | max %.3f" % (r["seconds"], r["samples"], r["mean_mA"], r["min_mA"], r["max_mA"])
        if "p50_mA" in r: line += " | p05 %.3f p50 %.3f p95 %.3f (exact)" % (r["p05_mA"], r["p50_mA"], r["p95_mA"])
        else: line += " | p05 %.3f p50 %.3f p95 %.3f (1 ms means)" % (r["p05_mA_1ms"], r["p50_mA_1ms"], r["p95_mA_1ms"])
        print(line); return
    req = {"cmd": a.cmd}
    if a.cmd == "mark": req["label"] = a.label
    if a.cmd == "power": req["state"] = a.state
    if a.cmd == "raw": req["seconds"] = a.seconds; req["out"] = a.out
    print(json.dumps(client(a.sock, req)))

if __name__ == "__main__":
    main()
