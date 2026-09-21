#!/usr/bin/env python3
"""esam conformance coverage runner.

Converts spec .wast files with wast2json (cached), runs each converted script
through build/easm, and aggregates a per-file coverage report.

Usage:
  python3 tools/run_spec.py [--suite DIR]... [--filter NAME] [-j N] [--report PATH]
"""
import argparse
import concurrent.futures as futures
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
WORK = BUILD / "conformance"


def convert(wast: Path, outdir: Path) -> Path | None:
    """wast2json conversion with caching. Returns json path or None on failure."""
    json_path = outdir / (wast.stem + ".json")
    stamp = outdir / (wast.stem + ".conv.ok")
    if stamp.exists() and json_path.exists() and stamp.stat().st_mtime > wast.stat().st_mtime:
        return json_path
    outdir.mkdir(parents=True, exist_ok=True)
    # clean previous emitted files
    for old in outdir.glob(wast.stem + ".*"):
        old.unlink(missing_ok=True)
    r = subprocess.run(
        ["wast2json", "--enable-threads", "--enable-function-references",
         "--enable-gc", str(wast), "-o", str(json_path)],
        capture_output=True, text=True, timeout=120,
    )
    if r.returncode != 0:
        # fall back to the custom converter (GC text syntax etc.)
        conv = WORK / ".conv" / (wast.stem + ".py.stamp")
        conv.parent.mkdir(parents=True, exist_ok=True)
        r2 = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "wast_convert.py"),
             str(wast), "-o", str(json_path)],
            capture_output=True, text=True, timeout=300,
        )
        if r2.returncode != 0:
            return None
        conv.write_text("ok")
        return json_path
    stamp.write_text("ok")
    return json_path


def run_one(easm: Path, json_path: Path, timeout: float) -> tuple[str, str, str]:
    """Returns (status, summary, detail). status in ok/fail/crash."""
    try:
        r = subprocess.run(
            [str(easm), "wast", str(json_path)],
            capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return "fail", "TIMEOUT", ""
    lines = [l for l in r.stdout.splitlines() if l.startswith("PASS ")]
    if not lines:
        return "crash", r.stderr.strip()[:200] or "no output", ""
    stats = lines[-1].split()
    passed, failed, skipped = int(stats[1]), int(stats[3]), int(stats[5])
    detail = "\n".join(l for l in r.stdout.splitlines() if l.startswith("line "))
    if failed == 0:
        return "ok", f"{passed} pass, {skipped} text-skip", ""
    return "fail", f"{passed} pass, {failed} FAIL, {skipped} text-skip", detail


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--suite", action="append", default=None,
                    help="suite dir with .wast files (default: tests/spec/test/core)")
    ap.add_argument("--filter", default=None, help="substring filter on file name")
    ap.add_argument("-j", type=int, default=8)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--report", default=str(ROOT / "build" / "coverage_report.md"))
    ap.add_argument("--jit", action="store_true", help="enable the aarch64 JIT")
    args = ap.parse_args()

    easm = BUILD / "easm"
    if not easm.exists():
        print("build/easm missing; run make first", file=sys.stderr)
        return 2

    suites = args.suite or [str(ROOT / "tests" / "spec" / "test" / "core")]
    wastes = []
    for s in suites:
        p = Path(s)
        if p.is_file():
            wastes.append(p)
        else:
            wastes.extend(sorted(p.rglob("*.wast")))
    if args.filter:
        wastes = [w for w in wastes if args.filter in w.name]
    print(f"{len(wastes)} wast files to run")

    t0 = time.time()
    # convert (serial; wast2json is fast and cached)
    conv_ok, conv_fail = {}, {}
    for w in wastes:
        rel = w.relative_to(ROOT / "tests")
        outdir = WORK / rel.parent
        jp = convert(w, outdir)
        if jp:
            conv_ok[w] = jp
        else:
            conv_fail[w] = "wast2json failed"
    if conv_fail:
        print(f"wast2json failures: {len(conv_fail)}")

    results = {}
    with futures.ThreadPoolExecutor(max_workers=args.j) as ex:
        futs = {ex.submit(run_one, easm, jp, args.timeout): w for w, jp in conv_ok.items()}
        for i, fut in enumerate(futures.as_completed(futs)):
            w = futs[fut]
            results[w] = fut.result()
            if (i + 1) % 25 == 0 or i + 1 == len(futs):
                print(f"  [{i+1}/{len(futs)}] done")

    ok = [w for w, (st, _, _) in results.items() if st == "ok"]
    fails = [(w, summ, det) for w, (st, summ, det) in results.items() if st != "ok"]

    total_pass = total_assert = total_skip = 0
    for w, (st, summ, _) in results.items():
        parts = summ.replace("text-skip", "").split(",")
        for p in parts:
            p = p.strip()
            if p.endswith("pass") and p[0].isdigit():
                total_pass += int(p.split()[0])
            if p.endswith("FAIL") and p[0].isdigit():
                total_pass  # failures counted separately
            if "skip" in p and p[0].isdigit():
                total_skip += int(p.split()[0])

    lines = [
        "# esam conformance coverage",
        "",
        f"- suites: {', '.join(suites)}",
        f"- wast files: {len(wastes)}",
        f"- files fully passing: {len(ok)} ({100*len(ok)/max(1,len(wastes)):.1f}%)",
        f"- files failing: {len(fails)}",
        f"- wast2json conversion failures: {len(conv_fail)}",
        f"- assert commands passed: {total_pass} (text-format skips: {total_skip})",
        f"- elapsed: {time.time()-t0:.1f}s",
        "",
        "## Failing files",
        "",
    ]
    for w, summ, det in sorted(fails, key=lambda x: x[1], reverse=True):
        try:
            rel = w.relative_to(ROOT)
        except ValueError:
            rel = w
        lines.append(f"### {rel} — {summ}")
        if det:
            lines.append("```")
            lines.extend(det.splitlines()[:40])
            lines.append("```")
        lines.append("")
    lines.append("## Passing files")
    lines.append("")
    for w in sorted(ok):
        try:
            rel = w.relative_to(ROOT)
        except ValueError:
            rel = w
        lines.append(f"- {rel}")
    report = Path(args.report)
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text("\n".join(lines))
    print(f"\nfully passing: {len(ok)}/{len(wastes)} files; report → {report}")
    return 0 if not fails and not conv_fail else 1


if __name__ == "__main__":
    sys.exit(main())
