#!/usr/bin/env python3
"""
Samsung 730B match evaluation against a local libfprint build.

Writes under --outdir:
  STATUS          current step (enroll / genuine / impostor)
  progress.log    step log with timestamps
  baseline.json   full results
  baseline.md     TAR/FAR summary

Requires root for USB access and a built libfprint with GI typelib.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

_SCORE_BAG: list[tuple[int, int]] = []
_OUT: Path | None = None


def setup_gi(libdir: str | None) -> None:
    if libdir:
        os.environ["LD_LIBRARY_PATH"] = libdir + (
            (os.pathsep + os.environ["LD_LIBRARY_PATH"])
            if os.environ.get("LD_LIBRARY_PATH")
            else ""
        )
        os.environ["GI_TYPELIB_PATH"] = libdir + (
            (os.pathsep + os.environ["GI_TYPELIB_PATH"])
            if os.environ.get("GI_TYPELIB_PATH")
            else ""
        )
    os.environ.setdefault("G_MESSAGES_DEBUG", "all")
    import gi

    gi.require_version("FPrint", "2.0")


def status(msg: str) -> None:
    """Update STATUS file + progress.log + stdout."""
    line = msg.strip()
    print(line, flush=True)
    if _OUT is None:
        return
    (_OUT / "STATUS").write_text(line + "\n", encoding="utf-8")
    with (_OUT / "progress.log").open("a", encoding="utf-8") as f:
        f.write(time.strftime("%H:%M:%S ") + line + "\n")


def _install_score_log_handlers() -> None:
    from gi.repository import GLib

    try:
        GLib.log_set_debug_enabled(True)
    except Exception:
        pass

    def on_log(domain, flags, message, user_data=None):
        global _SCORE_BAG
        if isinstance(message, bytes):
            message = message.decode("utf-8", "replace")
        m = re.search(r"score\s+(\d+)\s*/\s*(\d+)", message or "")
        if m:
            _SCORE_BAG.append((int(m.group(1)), int(m.group(2))))

    for dom in (None, "", "libfprint", "libfprint-print", "libfprint-device"):
        try:
            GLib.log_set_handler(
                dom,
                GLib.LogLevelFlags.LEVEL_MASK
                | GLib.LogLevelFlags.FLAG_FATAL
                | GLib.LogLevelFlags.FLAG_RECURSION,
                on_log,
                None,
            )
        except Exception:
            pass


def open_device():
    from gi.repository import FPrint

    ctx = FPrint.Context()
    ctx.enumerate()
    devs = ctx.get_devices()
    if not devs:
        raise SystemExit("장치 없음 (04e8:730b / root 권한 확인)")
    dev = next((d for d in devs if d.get_driver() == "samsung730b"), devs[0])
    status(f"장치 연결: driver={dev.get_driver()} stages={dev.get_nr_enroll_stages()}")
    dev.open_sync()
    return ctx, dev


def enroll(dev):
    from gi.repository import FPrint

    stages = dev.get_nr_enroll_stages()
    template = FPrint.Print.new(dev)
    template.set_finger(FPrint.Finger.RIGHT_INDEX)
    template.set_username(os.environ.get("USER", "eval"))
    template.set_description("match-eval")

    def progress(device, completed, print_, user_data, error):
        if error:
            status(f"WAIT 등록 오류: {error.message} — 다시 올리세요")
        else:
            status(
                f"WAIT 등록 {completed}/{stages} 완료 — 손 떼고, "
                f"같은 손가락 다시 올리세요"
                if completed < stages
                else f"OK 등록 {completed}/{stages} 완료"
            )

    status(
        f"WAIT 등록 시작 — 같은 손가락을 센서에 올리세요 "
        f"(총 {stages}번, 올렸다 떼기 반복)"
    )
    enrolled = dev.enroll_sync(template, None, progress, None)
    status("OK 등록 끝 — 이 손가락이 '본인' 입니다")
    return enrolled


def one_verify(dev, template, label: str, attempt: int, n: int) -> dict:
    from gi.repository import GLib

    global _SCORE_BAG

    if label == "genuine":
        who = "본인(등록과 같은 손가락)"
    else:
        who = "타인(등록과 다른 손가락)"

    t0 = time.monotonic()
    match = False
    err_msg = None
    scores: list[tuple[int, int]] = []
    # Retry finger-still-on / transient errors so they don't pollute TAR/FAR.
    for try_i in range(1, 6):
        _SCORE_BAG = []
        status(f"WAIT {who} {attempt}/{n} — 손 완전히 떼고 3초 후 올리세요 (시도 {try_i})")
        time.sleep(3.0)
        try:
            result = dev.verify_sync(template)
            match = bool(result[0] if isinstance(result, tuple) else result)
            err_msg = None
            scores = list(_SCORE_BAG)
            break
        except GLib.Error as e:
            err_msg = e.message
            match = False
            scores = list(_SCORE_BAG)
            low = (err_msg or "").lower()
            if "removing the finger" in low or "try again" in low or "too hot" in low:
                status(f"RETRY {who} {attempt}/{n}: {err_msg}")
                time.sleep(1.5)
                continue
            break

    dt = time.monotonic() - t0
    score = max(scores, key=lambda t: t[0]) if scores else None
    verdict = "일치" if match else "불일치"
    score_s = f"{score[0]}/{score[1]}" if score else "n/a"
    status(
        f"OK {who} {attempt}/{n} → {verdict} 점수={score_s} ({dt:.1f}s)"
        + (f" err={err_msg}" if err_msg else "")
    )
    return {
        "label": label,
        "attempt": attempt,
        "match": match,
        "score": score[0] if score else None,
        "threshold": score[1] if score else None,
        "all_scores": scores,
        "seconds": round(dt, 2),
        "error": err_msg,
    }


def summarize(rows: list[dict], label: str) -> dict:
    subset = [r for r in rows if r["label"] == label]
    n = len(subset)
    matches = sum(1 for r in subset if r["match"])
    scores = [r["score"] for r in subset if r["score"] is not None]
    return {
        "n": n,
        "matches": matches,
        "rate": (matches / n) if n else None,
        "score_min": min(scores) if scores else None,
        "score_max": max(scores) if scores else None,
        "score_median": sorted(scores)[len(scores) // 2] if scores else None,
        "scores": scores,
    }


def write_reports(out_dir: Path, meta: dict, rows: list[dict]) -> None:
    genuine = summarize(rows, "genuine")
    impostor = summarize(rows, "impostor")
    payload = {"meta": meta, "genuine": genuine, "impostor": impostor, "rows": rows}
    (out_dir / "baseline.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")

    def rate(s):
        return f"{100 * s['rate']:.1f}%" if s["rate"] is not None else "n/a"

    md = [
        f"# Match eval — {meta.get('config')}\n\n",
        f"- time: {meta['time']}\n",
        f"- driver: {meta.get('driver')}\n\n",
        "## Results\n\n",
        "| set | n | matches | rate | score min | med | max |\n",
        "|-----|---|---------|------|-----------|-----|-----|\n",
        f"| genuine TAR | {genuine['n']} | {genuine['matches']} | {rate(genuine)} | "
        f"{genuine['score_min']} | {genuine['score_median']} | {genuine['score_max']} |\n",
        f"| impostor FAR | {impostor['n']} | {impostor['matches']} | {rate(impostor)} | "
        f"{impostor['score_min']} | {impostor['score_median']} | {impostor['score_max']} |\n",
    ]
    (out_dir / "baseline.md").write_text("".join(md), encoding="utf-8")
    status(
        f"DONE TAR={genuine['matches']}/{genuine['n']} "
        f"FAR={impostor['matches']}/{impostor['n']} "
        f"→ {out_dir}"
    )


def main() -> int:
    global _OUT
    ap = argparse.ArgumentParser()
    ap.add_argument("--libdir", default=os.environ.get("LIBFPRINT_BUILD_LIBDIR"))
    ap.add_argument("--genuine", type=int, default=10)
    ap.add_argument("--impostor", type=int, default=5)
    ap.add_argument("--outdir", type=Path, required=True)
    ap.add_argument("--config", default="experiment")
    args = ap.parse_args()

    _OUT = args.outdir
    _OUT.mkdir(parents=True, exist_ok=True)
    (_OUT / "progress.log").write_text("", encoding="utf-8")

    if os.geteuid() != 0:
        status("WARN not root — USB open may fail")

    setup_gi(args.libdir)
    _install_score_log_handlers()
    ctx, dev = open_device()
    meta = {
        "time": datetime.now(timezone.utc).isoformat(),
        "libdir": args.libdir,
        "driver": dev.get_driver(),
        "enroll_stages": dev.get_nr_enroll_stages(),
        "genuine_n": args.genuine,
        "impostor_n": args.impostor,
        "config": args.config,
    }

    try:
        enrolled = enroll(dev)
        rows: list[dict] = []

        status(f"INFO 본인 확인 {args.genuine}회 — 등록과 같은 손가락")
        for i in range(1, args.genuine + 1):
            rows.append(one_verify(dev, enrolled, "genuine", i, args.genuine))

        status(f"INFO 타인 확인 {args.impostor}회 — 다른 손가락으로 바꾸세요")
        time.sleep(1.5)
        for i in range(1, args.impostor + 1):
            rows.append(one_verify(dev, enrolled, "impostor", i, args.impostor))

        write_reports(_OUT, meta, rows)
        g, im = summarize(rows, "genuine"), summarize(rows, "impostor")
        if g["rate"] is not None:
            print(f"TAR: {g['matches']}/{g['n']} = {100*g['rate']:.1f}%", flush=True)
        if im["rate"] is not None:
            print(f"FAR: {im['matches']}/{im['n']} = {100*im['rate']:.1f}%", flush=True)
    finally:
        try:
            dev.close_sync()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
