#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Cut a time window out of an fcapz capture, to zoom a timeline rendered by fcapz_timeline.py.

    python3 scripts/fcapz_window.py in.json out.json A_MS B_MS            # from the first sample
    python3 scripts/fcapz_window.py in.json out.json A_MS B_MS --trigger  # relative to the trigger

Default: the window [A, B] ms is measured from the capture's first sample, and the output axis
starts exactly at A (a synthetic sample at A carries the signal state entering the window).
--trigger: A and B are relative to the trigger sample, the t=0 of a full-capture timeline; the
window must contain the trigger, and the axis origin stays at the trigger.
"""
import copy, gzip, json, sys


def unwrap(d):
    out, base, prev = [], 0, None
    for t in (t["value"] for t in d["timestamps"]):
        if prev is not None and t < prev:
            base += 1 << 32                      # 32-bit timestamp counter wrapped
        out.append(base + t)
        prev = t
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    trig_mode = "--trigger" in sys.argv
    src, dst, a, b = args[0], args[1], float(args[2]), float(args[3])
    d = json.load((gzip.open if src.endswith(".gz") else open)(src, "rt"))  # .json or .json.gz
    clk = d.get("sample_clock_hz") or 100_000_000
    ts = unwrap(d)
    if trig_mode:
        trig = d["pretrigger"]
        keep = [i for i, t in enumerate(ts) if a <= (t - ts[trig]) * 1e3 / clk <= b]
        i0, i1 = max(keep[0] - 1, 0), keep[-1] + 1   # one sample before: state entering the window
        assert i0 <= trig < i1, "window must contain the trigger (t=0) to keep the axis origin"
        d["samples"], d["timestamps"] = d["samples"][i0:i1], d["timestamps"][i0:i1]
        d["pretrigger"] = trig - i0
    else:
        ta, tb = ts[0] + int(a * clk / 1000), ts[0] + int(b * clk / 1000)
        keep = [i for i, t in enumerate(ts) if ta < t <= tb]
        i0, i1 = keep[0], keep[-1] + 1
        s0, t0 = copy.deepcopy(d["samples"][i0 - 1]), copy.deepcopy(d["timestamps"][i0 - 1])
        t0["value"] = ta & 0xFFFFFFFF
        d["samples"] = [s0] + d["samples"][i0:i1]
        d["timestamps"] = [t0] + d["timestamps"][i0:i1]
        d["pretrigger"] = 0
    json.dump(d, open(dst, "w"))
    print(f"{len(d['samples'])} samples -> {dst}")


if __name__ == "__main__":
    main()
