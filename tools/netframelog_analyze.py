#!/usr/bin/env python3
"""Summarise a NexusDoom -netframelog file.

For every rendered frame, the "view time" is (gametic - 1 + frac) * tic interval:
the moment in game time the renderer is showing. In a perfectly smooth render it
advances exactly as fast as the wall clock, so for each pair of consecutive
frames the error  (view advance - wall advance)  is zero. A freeze shows up as a
negative error (wall time passes, view doesn't); a skip as a positive one.

Usage: netframelog_analyze.py <log> [<log> ...] [--skip-seconds N] [--dump]
"""
import sys


def analyze(path, skip_us, dump):
    frames = []      # (t, view_us)
    gate_to_exec = []  # time between gate firing and tic execution
    restores = 0
    last_gate_t = None
    interval = 28571
    first_t = None

    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            kind, t = parts[0], int(parts[1])
            if first_t is None:
                first_t = t
            if t - first_t < skip_us:
                continue
            if kind == 'G':
                last_gate_t = t
            elif kind == 'T':
                interval = int(parts[3])
                if last_gate_t is not None:
                    gate_to_exec.append(t - last_gate_t)
                last_gate_t = None
            elif kind == 'K':
                restores += 1
                frames.append(None)  # break the frame sequence
            elif kind == 'F':
                gametic, frac = int(parts[2]), int(parts[3])
                frames.append((t, (gametic - 1 + frac / 65536.0) * interval))

    errors = []
    for a, b in zip(frames, frames[1:]):
        if a is None or b is None:
            continue
        dt = b[0] - a[0]
        dv = b[1] - a[1]
        errors.append((b[0], dt, dv - dt))

    if not errors:
        print(f"{path}: no frames")
        return

    abs_err = sorted(abs(e[2]) for e in errors)
    n = len(abs_err)
    frozen = sum(1 for _, dt, err in errors if dt > 500 and err <= -dt + 1)
    backwards = sum(1 for _, dt, err in errors if err < -dt - 1)
    dts = sorted(e[1] for e in errors)

    def pct(arr, p):
        return arr[min(len(arr) - 1, int(len(arr) * p))]

    print(f"== {path}")
    print(f"  frames: {n + 1}, median frame time {pct(dts, 0.5) / 1000:.1f} ms, "
          f"tic interval {interval / 1000:.1f} ms, key frame restores: {restores}")
    print(f"  per-frame view error |view advance - wall advance| (ms): "
          f"median {pct(abs_err, 0.5) / 1000:.2f}, p90 {pct(abs_err, 0.9) / 1000:.2f}, "
          f"p99 {pct(abs_err, 0.99) / 1000:.2f}, max {abs_err[-1] / 1000:.2f}")
    print(f"  mean |error| per frame: {sum(abs_err) / n / 1000:.2f} ms "
          f"({sum(abs_err) / n / interval * 100:.1f}% of a tic)")
    print(f"  frozen frames (view did not move): {frozen} ({frozen / n * 100:.1f}%), "
          f"backwards: {backwards}")
    if gate_to_exec:
        g = sorted(gate_to_exec)
        print(f"  gate -> tic executed (remote cmd wait + hold) ms: "
              f"median {pct(g, 0.5) / 1000:.2f}, p90 {pct(g, 0.9) / 1000:.2f}, max {g[-1] / 1000:.2f}")

    if dump:
        for t, dt, err in errors:
            print(f"    t={t / 1000:10.1f} dt={dt / 1000:6.2f} err={err / 1000:+7.2f}")


def main():
    args = sys.argv[1:]
    skip = 0
    dump = False
    paths = []
    i = 0
    while i < len(args):
        if args[i] == '--skip-seconds':
            skip = int(float(args[i + 1]) * 1000000)
            i += 2
            continue
        if args[i] == '--dump':
            dump = True
        else:
            paths.append(args[i])
        i += 1
    for p in paths:
        analyze(p, skip, dump)


if __name__ == '__main__':
    main()
