#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Per-stage pipeline profile of an fcapz event-timeline capture (scu200_ai_demo03_fcapz).

    python3 scripts/fcapz_profile.py capture.json[.gz] [label]

For each FSIN strobe it finds the frame write, ISR, worker take, mbdpu03_infer(), the
mbdpu03 camera-read burst and the overlay draw that follow it, and prints mean/min/max
per stage. It also prints how many strobes the camera answered (frames written per FSIN),
the collision counts and, on captures that carry the result probes (digit/dx/dy, from
d6fa43e on), the inferred digit sequence.
Run from the repo root (it imports scripts/fcapz_timeline.py).
"""
import os, statistics as st, sys
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fcapz_timeline import load, intervals  # noqa: E402


def main():
    path = sys.argv[1]
    label = sys.argv[2] if len(sys.argv) > 2 else os.path.basename(path)
    _, rows = load(path)
    fields = rows[0].keys()

    def lane(f):
        return [(a, b) for a, b, _ in intervals(rows, f)] if f in fields else []

    def first_after(L, t, t2):
        for a, b in L:
            if t <= a < t2:
                return (a, b)

    fs = [r["t_us"] for i, r in enumerate(rows) if r["fsin"] and (i == 0 or not rows[i - 1]["fsin"])]
    L = {k: lane(k) for k in ["wr_busy", "isr", "fw_take", "fw_dim", "fw_held", "fw_infer",
                              "dpu_busy", "fw_overlay", "fw_roibox", "col_dpu", "col_disp"]}
    stats = {}

    def add(k, v):
        stats.setdefault(k, []).append(v)

    nper = []
    for i in range(len(fs) - 1):
        f, f2 = fs[i], fs[i + 1]
        add("FSIN period", (f2 - f) / 1000)
        nper.append(sum(1 for a, _ in L["wr_busy"] if f <= a < f2))
        w = first_after(L["wr_busy"], f, f2)
        if not w:
            continue
        add("FSIN -> frame write start", (w[0] - f) / 1000)
        add("frame write (frmbuf_wr)", (w[1] - w[0]) / 1000)
        hz = f2 + 50000                                   # look up to 50 ms past the next FSIN
        q = first_after(L["isr"], w[1] - 200, hz)
        if q:
            add("ISR duration (us)", q[1] - q[0])
        tk = first_after(L["fw_take"], w[1] - 200, hz)
        if tk:
            add("worker: frame taken -> released", (tk[1] - tk[0]) / 1000)
        inf = first_after(L["fw_infer"], w[1] - 200, hz)
        if not inf:
            continue
        add("write end -> infer start", (inf[0] - w[1]) / 1000)
        add("mbdpu03_infer() (fw)", (inf[1] - inf[0]) / 1000)
        dp = first_after(L["dpu_busy"], inf[0], inf[1] + 1)
        if dp:
            add("mbdpu03 camera-read burst", (dp[1] - dp[0]) / 1000)
        ov = first_after(L["fw_overlay"], inf[1] - 10, hz)
        if not ov:
            continue
        add("infer end -> overlay start (us)", ov[0] - inf[1])
        add("overlay draw", (ov[1] - ov[0]) / 1000)
        rb = first_after(L["fw_roibox"], ov[1] - 10, hz)
        if rb:
            add("ROI box draw", (rb[1] - rb[0]) / 1000)
        add("FSIN -> worker done", ((rb or ov)[1] - f) / 1000)

    span = (rows[-1]["t_us"] - rows[0]["t_us"]) / 1000
    ans = sum(1 for n in nper if n)
    print(f"{label}: {span:.0f} ms, {len(fs)} FSIN, strobes answered {ans}/{len(nper)}"
          f" ({100 * ans / max(len(nper), 1):.0f}%), frames/strobe {dict(Counter(nper))}")
    print(f"  collisions: wr=mbdpu03 read {len(L['col_dpu'])}, wr=display {len(L['col_disp'])};"
          f" fw_dim {len(L['fw_dim'])}, fw_held {len(L['fw_held'])}")
    for k, v in stats.items():
        u = "us" if "(us)" in k else "ms"
        print(f"  {k:34s} n={len(v):4d}  mean={st.mean(v):8.3f}  min={min(v):8.3f}  max={max(v):8.3f} {u}")
    if "digit" in fields:
        seq, last = [], None
        for r in rows:
            t = (r["digit"], r["dx"], r["dy"])
            if t != last:
                seq.append(t)
                last = t
        dg = [s[0] for s in seq[1:]]
        bad = sum(1 for a, b in zip(dg, dg[1:]) if not (b == a + 1 or (a == 8 and b == 1)))
        print(f"  results {len(dg)}, digits {''.join(map(str, dg[:48]))}{'...' if len(dg) > 48 else ''}"
              f" (out of sequence, 1..8 disc: {bad})")


if __name__ == "__main__":
    main()
