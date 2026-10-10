#!/usr/bin/env python3
"""汽水签名 HTTP 服务（napi_host 常驻 worker 池前端）
监听 0.0.0.0:8900，POST /sign {url, headers:{...}} -> {ok:true, headers:{X-Helios, X-Medusa,...}}
worker: wine napi_host.exe ... server 模式（stdin 行协议 b64url\\tb64headers -> b64sig）

并发模型（实测矩阵决定）：
  单worker自由(1.4核) = 12 QPS；双worker绑核0/1 = 15.3 QPS（+27%）
  每个 worker 独立 WINEPREFIX + taskset 绑核，round-robin 分配 + per-worker 锁。
"""
import base64, json, os, subprocess, threading, time, itertools

BIND = os.environ.get("NAPI_BIND", "0.0.0.0")
PORT = int(os.environ.get("NAPI_PORT", "8900"))
TOKEN = os.environ.get("NAPI_TOKEN", "CHANGE_ME_REQUIRED")
WINE = os.environ.get("WINE", "/usr/bin/wine")
HOSTEXE = os.environ.get("NAPI_HOST", "/opt/soda/app/resources/app.asar.unpacked/napi_host.exe")
BDMS = os.environ.get("NAPI_BDMS", "Z:\\opt\\soda\\app\\resources\\app.asar.unpacked\\bdms.node")

# worker 池配置：每个 = (WINEPREFIX, taskset CPU 列表或 None)
WORKERS = [
    (os.environ.get("NAPI_PREFIX1", "/home/ubuntu/wine-prefix"), "0"),
    (os.environ.get("NAPI_PREFIX2", "/home/ubuntu/wine-prefix2"), "1"),
]
_rr = itertools.cycle(range(len(WORKERS)))
_pool = [{"lock": threading.Lock(), "proc": None, "prefix": p, "aff": a} for p, a in WORKERS]

def _cmd(i):
    prefix, aff = WORKERS[i]
    cmd = []
    if aff:
        cmd += ["taskset", "-c", aff]
    cmd += ["env", "WINEPREFIX=" + prefix, WINE, HOSTEXE, BDMS, "http://x", "", "1", "server"]
    return cmd

def _start_worker(i):
    w = _pool[i]
    if w["proc"] is not None and w["proc"].poll() is None:
        return w["proc"]
    try:
        w["proc"] = subprocess.Popen(
            _cmd(i), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, bufsize=0)
    except Exception as e:
        print(f"[napi] worker{i} start failed: {e}", flush=True)
        w["proc"] = None
        raise
    return w["proc"]

def _sign(url, headers):
    hdr_lines = []
    for k, v in headers.items():
        hdr_lines.append(k)
        hdr_lines.append(str(v))
    hdrs = "\r\n".join(hdr_lines)
    line = base64.b64encode(url.encode()).decode() + "\t" + base64.b64encode(hdrs.encode()).decode()

    # round-robin 选 worker，失败换下一个（最多全池轮一遍）
    for _ in range(len(_pool)):
        i = next(_rr)
        w = _pool[i]
        with w["lock"]:
            proc = _start_worker(i)
            try:
                proc.stdin.write((line + "\n").encode())
                proc.stdin.flush()
                out = proc.stdout.readline()
                if not out:
                    raise RuntimeError(f"worker{i} 无响应")
                out = out.strip()
                if out.startswith(b"ERR:"):
                    raise RuntimeError(out.decode(errors="replace"))
                sig = base64.b64decode(out + b"==" * ((4 - len(out) % 4) % 4))
                parts = sig.decode(errors="replace").split("\r\n")
                result = {}
                for j in range(0, len(parts) - 1, 2):
                    if parts[j]:
                        result[parts[j]] = parts[j + 1]
                return result
            except (BrokenPipeError, RuntimeError) as e:
                print(f"[napi] worker{i} 异常，尝试重启: {e}", flush=True)
                w["proc"] = None
                _start_worker(i)
                if _pool.index(w) == len(_pool) - 1:
                    raise
                continue
    raise RuntimeError("所有 worker 均失败")

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass
    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        if self.path == "/healthz":
            status = []
            for w in _pool:
                p = w["proc"]
                status.append(p.poll() if p else "not-started")
            self._send(200, {"ok": True, "service": "napi-host-sign", "workers": status})
        else:
            self._send(404, {"ok": False, "error": "not found", "code": "not_found"})
    def do_POST(self):
        if self.path != "/sign":
            self._send(404, {"ok": False, "error": "not found", "code": "not_found"})
            return
        auth = self.headers.get("Authorization", "")
        if auth != "Bearer " + TOKEN:
            self._send(401, {"ok": False, "error": "unauthorized", "code": "unauthorized"})
            return
        try:
            n = int(self.headers.get("Content-Length", "0"))
            spec = json.loads(self.rfile.read(n) or b"{}")
            url = spec.get("url", "")
            headers = spec.get("headers", {})
            if not url:
                self._send(400, {"ok": False, "error": "missing url", "code": "bad_request"})
                return
            t0 = time.time()
            sh = _sign(url, headers)
            dt = (time.time() - t0) * 1000
            self._send(200, {"ok": True, "headers": sh, "latency_ms": round(dt, 1)})
        except Exception as e:
            self._send(500, {"ok": False, "error": str(e), "code": "signer_error"})

if __name__ == "__main__":
    for i in range(len(_pool)):
        try:
            _start_worker(i)
        except Exception as e:
            print(f"[napi] worker{i} 预启动失败: {e}", flush=True)
    srv = ThreadingHTTPServer((BIND, PORT), H)
    print("napi-http (multi-worker pool) listening on", BIND, PORT, flush=True)
    srv.serve_forever()
