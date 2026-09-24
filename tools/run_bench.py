#!/usr/bin/env python3
"""easm benchmark: interpreter vs aarch64-JIT vs wasmtime vs node."""
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WASM = ROOT / "bench" / "bench.wasm"

# (export name, kind-arg for dispatchers, arg) — direct exports avoid the
# br_table dispatcher (not JIT-lowered in v1)
KERNELS = [
    ("fib", "10000000"),        # 10M iterations
    ("primes", "500000"),       # sieve + scan
    ("sum", "100000000"),       # 100M u64 iterations
    ("matmul", "30"),           # 30 x 128^3 FMAs
    ("memsum", "2000000"),      # 2M memory loads
]


def run(cmd, env=None, timeout=600):
    t0 = time.perf_counter()
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    dt = time.perf_counter() - t0
    return dt, r.returncode, (r.stdout or r.stderr).strip()[:60]


def best_of(fn, n=2):
    best = float("inf")
    out = ""
    for _ in range(n):
        dt, rc, out = fn()
        if rc != 0:
            return float("nan"), out
        best = min(best, dt)
    return best, out


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    easm = str(ROOT / "build" / "easm")
    env_int = {"PATH": "/usr/bin:/bin:/usr/sbin", "HOME": __import__("os").environ.get("HOME", "")}
    env_jit = dict(env_int, EA_JIT="1")
    # EA_CACHE/EA_WARM experiments: run_bench.py jit-cache / jit-warm
    extra = {k: "1" for k in sys.argv[2:] if k in ("EA_CACHE", "EA_WARM")}
    env_jit.update(extra)

    print(f"{'kernel':10} {'easm-jit':>10} {'easm-int':>10} {'wasmtime':>10} {'node':>10}   (seconds, lower is better)")
    for name, arg in KERNELS:
        t = {}
        if which in ("all", "jit"):
            t["easm-jit"], _ = best_of(lambda: run([easm, "run", str(WASM), name, arg], env=env_jit))
        if which in ("all", "int"):
            t["easm-int"], _ = best_of(lambda: run([easm, "run", str(WASM), name, arg], env=env_int))
        if which in ("all", "wt"):
            try:
                t["wasmtime"], _ = best_of(lambda: run(["wasmtime", "run", "--invoke", name, str(WASM), arg]))
            except FileNotFoundError:
                t["wasmtime"] = float("nan")
        if which in ("all", "node"):
            try:
                js = ('const w=require("fs").readFileSync("%s");'
                      'const i=new WebAssembly.Instance(new WebAssembly.Module(w));'
                      'try{i.exports.%s(%s)}catch(e){i.exports.%s(BigInt(%s))};'
                      % (WASM, name, arg, name, arg))
                t["node"], _ = best_of(lambda: run(["node", "-e", js]))
            except FileNotFoundError:
                t["node"] = float("nan")
        print(f"{name:10} {t.get('easm-jit', float('nan')):>10.3f} "
              f"{t.get('easm-int', float('nan')):>10.3f} "
              f"{t.get('wasmtime', float('nan')):>10.3f} "
              f"{t.get('node', float('nan')):>10.3f}")


if __name__ == "__main__":
    main()
