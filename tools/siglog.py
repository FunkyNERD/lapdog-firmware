#!/usr/bin/env python3
"""Record and summarise the timer's full-rate signal log (issue #12).

Record (turns "sdr log on" on, writes every L: line to CSV, echoes the rest):

    tools/siglog.py record /dev/ttyACM0 run.csv [--fifo /path/ctl]

While recording, lines written to the FIFO go to the timer as console commands
("sdr gain 30", "arm practice", ...). Lines starting with '#' are not sent: they
become marker rows in the CSV ("#quad off", "#passes gain 30") that split the
summary into segments.

Summarise a recording (no extra packages needed):

    tools/siglog.py summary run.csv
"""

import argparse
import csv
import os
import select
import signal
import statistics
import sys
import time

FIELDS = ["ms", "gain", "dbfs", "baseline", "above", "peak", "clipped", "since", "event"]
BURST_WINDOW = 3  # captures after a resume counted as "just after a burst"


def record(args):
    import serial  # pyserial

    port = serial.Serial(args.port, 115200, timeout=0.05)
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt))
    fifo = None
    if args.fifo:
        if not os.path.exists(args.fifo):
            os.mkfifo(args.fifo)
        fifo = os.open(args.fifo, os.O_RDONLY | os.O_NONBLOCK)
    out = open(args.csv, "w", newline="")
    w = csv.writer(out)
    w.writerow(["host_s", "mark"] + FIELDS)
    port.write(b"sdr log on\n")
    buf = b""
    pending = b""
    n = 0
    last_status = time.time()
    try:
        while True:
            if fifo is not None:
                r, _, _ = select.select([fifo], [], [], 0)
                if r:
                    chunk = os.read(fifo, 4096)
                    if not chunk:  # writer closed: reopen so the next echo works
                        os.close(fifo)
                        fifo = os.open(args.fifo, os.O_RDONLY | os.O_NONBLOCK)
                    pending += chunk
                    while b"\n" in pending:
                        cmd, pending = pending.split(b"\n", 1)
                        cmd = cmd.decode(errors="replace").strip()
                        if not cmd:
                            continue
                        if cmd.startswith("#"):
                            w.writerow([f"{time.time():.3f}", cmd[1:].strip()] + [""] * len(FIELDS))
                            print(f"== mark: {cmd[1:].strip()}", flush=True)
                        else:
                            port.write((cmd + "\n").encode())
                            print(f">> {cmd}", flush=True)
            buf += port.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.decode(errors="replace").strip()
                if line.startswith("L:") and "=" not in line:
                    w.writerow([f"{time.time():.3f}", ""] + line[2:].split(","))
                    n += 1
                elif line:
                    print(line, flush=True)
            if time.time() - last_status >= 10:
                last_status = time.time()
                out.flush()
                print(f"-- {n} samples", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            port.write(b"sdr log off\n")
        except Exception:
            pass
        out.close()


def pct(xs, p):
    if not xs:
        return float("nan")
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p / 100 * len(xs)))]


def summarise(rows, title):
    if not rows:
        return
    ms = [int(r["ms"]) for r in rows]
    secs = max(1e-3, (ms[-1] - ms[0]) / 1000)
    dbfs = [float(r["dbfs"]) for r in rows]
    peak = [int(r["peak"]) for r in rows]
    clipped = [int(r["clipped"]) for r in rows]
    events = [r["event"] for r in rows]
    gains = sorted({r["gain"] for r in rows})
    print(f"\n## {title}")
    print(f"  {secs:.1f} s, {len(rows)} samples ({len(rows) / secs:.0f}/s), gain {','.join(gains)}")
    print(f"  dBFS median {statistics.median(dbfs):.1f}, p5 {pct(dbfs, 5):.1f}, p95 {pct(dbfs, 95):.1f}, max {max(dbfs):.1f}")
    print(f"  raw peak median {statistics.median(peak)}, p99 {pct(peak, 99)}, max {max(peak)} (511 = full scale)")
    nclip = sum(1 for c in clipped if c)
    print(f"  clipped captures {nclip} ({100 * nclip / len(rows):.2f}%), worst {max(clipped)} samples")
    per_min = 60 / secs
    for ev, name in (("P", "passes"), ("S", "ignored too soon"), ("L", "ignored too long"), ("E", "peaks entered")):
        k = events.count(ev)
        print(f"  {name}: {k} ({k * per_min:.1f}/min)")

    # Each peak, entry to result: how high, whether it clipped, and how soon after a burst it started.
    peaks = []
    start = None
    for i, r in enumerate(rows):
        if r["event"] == "E":
            start = i
        elif r["event"] in "PSL" and start is not None:
            seg = rows[start : i + 1]
            peaks.append((r["event"], seg))
            start = None
    if peaks:
        print("  peaks (result, ms, dur ms, max above dB, max raw peak, clipped captures, since at entry):")
        for ev, seg in peaks:
            dur = int(seg[-1]["ms"]) - int(seg[0]["ms"])
            print(
                f"    {ev} {seg[0]['ms']:>9} {dur:>5} {max(float(s['above']) for s in seg):6.1f} "
                f"{max(int(s['peak']) for s in seg):4} {sum(1 for s in seg if int(s['clipped'])):4} {seg[0]['since']:>5}"
            )

    # Post-burst settling: the first captures after a resume vs. the steady level.
    after = [float(r["dbfs"]) for r in rows if int(r["since"]) < BURST_WINDOW]
    steady = [float(r["dbfs"]) for r in rows if int(r["since"]) >= 20]
    if after and steady:
        resumes = sum(1 for r in rows if r["since"] == "0")
        print(
            f"  after a resume ({resumes} resumes): first {BURST_WINDOW} captures median "
            f"{statistics.median(after):.1f} dBFS vs steady {statistics.median(steady):.1f} "
            f"(step {statistics.median(after) - statistics.median(steady):+.1f} dB)"
        )


def summary(args):
    segments = [("start", [])]
    with open(args.csv, newline="") as f:
        for r in csv.DictReader(f):
            if r["mark"]:
                segments.append((r["mark"], []))
            elif r["ms"]:
                segments[-1][1].append(r)
    for title, rows in segments:
        summarise(rows, title)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record")
    r.add_argument("port")
    r.add_argument("csv")
    r.add_argument("--fifo", help="named pipe for commands and #marks")
    s = sub.add_parser("summary")
    s.add_argument("csv")
    args = ap.parse_args()
    record(args) if args.cmd == "record" else summary(args)


if __name__ == "__main__":
    sys.exit(main())
