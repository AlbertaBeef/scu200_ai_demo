#!/usr/bin/env python3
"""fcapz_timeline.py - turn an fcapz_evtcap capture (fcapz JSON export) into a swimlane timeline.

    fcapz --backend hw_server --chain 3 capture --probe-file Vivado/src/hdl/fcapz_evtcap.prob \
          --stor-qual-mode 1 --stor-qual-value 1 --stor-qual-mask 1 ... --format json --out cap.json
    python3 scripts/fcapz_timeline.py cap.json -o cap.html      # writes cap.html, prints a summary

Each stored sample is one event (storage qualification on the evt bit), with a 32-bit
timestamp in 100 MHz sample clocks. The summary lists every camera frame write, every
mbdpu03 camera read and every collision (frame-buffer writer and reader on the same slot).
Standard library only.
"""
import argparse
import json
import sys

LANES = [
    # (title, kind, fields)
    ("FSIN strobe",         "bit",  "fsin"),
    ("SOF early (wake frame)", "bit", "frame_early"),
    ("frmbuf_wr writes",    "slot", ("wr_busy", "wr_slot")),
    ("frmbuf_wr IRQ",       "bit",  "wr_irq"),
    ("ISR (FrmbufWrCallback)", "bit", "isr"),
    ("ISR next-write slot", "step", "isr_next_slot"),
    ("worker: frame taken", "slot", ("fw_take", "fw_slot")),
    ("worker: dim frame",   "bit",  "fw_dim"),
    ("worker: display held","bit",  "fw_held"),
    ("mbdpu03_infer()",     "bit",  "fw_infer"),
    ("mbdpu03 camera reads","slot", ("dpu_busy", "dpu_slot")),
    ("overlay draw",        "bit",  "fw_overlay"),
    ("ROI box draw",        "bit",  "fw_roibox"),
    ("display slot",        "step", "disp_slot"),
    ("collision: wr = dpu read", "bit", "col_dpu"),
    ("collision: wr = display",  "bit", "col_disp"),
    ("result: digit",       "val",  "digit"),
    ("result: dx (px)",     "val",  "dx"),
    ("result: dy (px)",     "val",  "dy"),
]
SIGNED_HALF_PX = ("dx", "dy")      # signed 8-bit, 0.5 px units -> px


def load(path):
    d = json.load(open(path))
    probes = {p["name"]: (p["lsb"], p["width"]) for p in d.get("probes", [])}
    if not probes:
        sys.exit("capture has no probe map - pass --probe-file to fcapz capture")
    if "timestamps" not in d:
        sys.exit("capture has no timestamps (core built with TIMESTAMP_W=0?)")
    clk = d.get("sample_clock_hz") or 100_000_000
    vals = [s["value"] for s in d["samples"]]
    ts = [t["value"] for t in d["timestamps"]]
    # unwrap the 32-bit timestamp counter
    out, base, prev = [], 0, None
    for t in ts:
        if prev is not None and t < prev:
            base += 1 << 32
        out.append(base + t)
        prev = t
    trig = d.get("pretrigger", 0)
    t0 = out[trig] if trig < len(out) else out[0]
    rows = []
    for v, t in zip(vals, out):
        r = {n: (v >> lsb) & ((1 << w) - 1) for n, (lsb, w) in probes.items()}
        for n in SIGNED_HALF_PX:
            if n in r:
                w = probes[n][1]
                x = r[n] - (1 << w) if r[n] >> (w - 1) else r[n]
                r[n] = x / 2
        r["t_us"] = (t - t0) * 1e6 / clk
        rows.append(r)
    return d, rows


def intervals(rows, on, val=None):
    """[(t_start, t_end, value)] where field `on` is 1 (value = rows[val] at the start)."""
    res, start, sv = [], None, None
    for r in rows:
        if r[on] and start is None:
            start, sv = r["t_us"], (r[val] if val else 1)
        elif r[on] and val and r[val] != sv:           # slot changed while still busy
            res.append((start, r["t_us"], sv)); start, sv = r["t_us"], r[val]
        elif not r[on] and start is not None:
            res.append((start, r["t_us"], sv)); start = None
    if start is not None:
        res.append((start, rows[-1]["t_us"], sv))
    return res


def steps(rows, f):
    res, start, sv = [], rows[0]["t_us"], rows[0][f]
    for r in rows[1:]:
        if r[f] != sv:
            res.append((start, r["t_us"], sv)); start, sv = r["t_us"], r[f]
    res.append((start, rows[-1]["t_us"], sv))
    return res


def summary(rows):
    wr = intervals(rows, "wr_busy", "wr_slot")
    dpu = intervals(rows, "dpu_busy", "dpu_slot")
    inf = intervals(rows, "fw_infer")
    cd = intervals(rows, "col_dpu")
    cdisp = intervals(rows, "col_disp")
    fs = [r["t_us"] for i, r in enumerate(rows) if r["fsin"] and (i == 0 or not rows[i - 1]["fsin"])]
    span = rows[-1]["t_us"] - rows[0]["t_us"]
    print(f"{len(rows)} events over {span/1000:.1f} ms; {len(fs)} FSIN pulses, "
          f"{len(wr)} frame writes, {len(inf)} inferences, {len(dpu)} mbdpu03 read bursts")
    if len(fs) > 1:
        per = [(b - a) / 1000 for a, b in zip(fs, fs[1:])]
        print(f"FSIN period: {min(per):.2f}..{max(per):.2f} ms")
    if wr:
        print("frame write durations (ms): " +
              ", ".join(f"{(b-a)/1000:.2f}" for a, b, _ in wr[:8]) + (" ..." if len(wr) > 8 else ""))
    print(f"collisions writer=mbdpu03 read: {len(cd)}   writer=display: {len(cdisp)}")
    for a, b, _ in cd[:20]:
        r = next(x for x in rows if x["t_us"] >= a)
        print(f"  COL_DPU  t={a/1000:9.3f} ms  dur={(b-a):8.1f} us  slot={r['wr_slot']}  "
              f"fw_slot={r['fw_slot']} isr_next={r['isr_next_slot']} held={r['isr_held_slot']}")
    # ISR bookkeeping check: the slot the ISR reports as "done" should be the slot of the
    # last frame write that started before the ISR (its busy hold outlasts the IRQ).
    isr_t = [(r["t_us"], r["isr_done_slot"]) for i, r in enumerate(rows)
             if r["isr"] and (i == 0 or not rows[i - 1]["isr"])]
    ok = n = 0
    for t, _ in isr_t:
        prev = [w for w in wr if w[0] <= t]
        if not prev:
            continue
        # isr_done_slot is written at ISR exit: use the value at the next ISR entry
        after = next((r for r in rows if r["t_us"] > t and not r["isr"]), None)
        if after is None:
            continue
        n += 1
        ok += after["isr_done_slot"] == prev[-1][2]
    print(f"ISR done-slot == slot actually written: {ok}/{n}")
    if "frame_early" in rows[0]:
        ne = len([1 for i, r in enumerate(rows) if r["frame_early"] and (i == 0 or not rows[i - 1]["frame_early"])])
        print(f"early (wake) SOFs: {ne}")
    return dict(wr=wr, dpu=dpu, inf=inf, col_dpu=cd, col_disp=cdisp, fsin=fs)


def lanes_data(rows):
    lanes = []
    for title, kind, f in LANES:
        if (f if isinstance(f, str) else f[0]) not in rows[0]:
            continue                      # probe not in this capture (older bitstream)
        if kind == "bit":
            segs = [(a, b, 1) for a, b, _ in intervals(rows, f)]
        elif kind == "slot":
            segs = intervals(rows, f[0], f[1])
        else:
            segs = steps(rows, f)
        lanes.append({"title": title, "kind": kind, "field": f if isinstance(f, str) else f[0],
                      "segs": [[round(a, 3), round(b, 3), v] for a, b, v in segs]})
    return lanes


HTML = r"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>fcapz Event Timeline</title>
<style>
:root{--bg:#fff;--fg:#1b1f24;--mut:#6a737d;--grid:#e3e6ea;--s0:#2b7bd6;--s1:#d98b1a;--s2:#2a9d6a;--s3:#9aa3ad;--bit:#5b5fc7;--col:#d62f2f;--teal:#0fb3b3}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#121417;--fg:#e6e8eb;--mut:#9aa3ad;--grid:#2a2f36;--s0:#5aa2f0;--s1:#f0a642;--s2:#4cc48e;--s3:#6b7480;--bit:#8b8ff0;--col:#ff5c5c;--teal:#22e6e6}}
:root[data-theme=dark]{--bg:#121417;--fg:#e6e8eb;--mut:#9aa3ad;--grid:#2a2f36;--s0:#5aa2f0;--s1:#f0a642;--s2:#4cc48e;--s3:#6b7480;--bit:#8b8ff0;--col:#ff5c5c;--teal:#22e6e6}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:13px/1.4 system-ui,sans-serif}
h1{font-size:18px;margin:0 0 4px} .mut{color:var(--mut)} pre{font-size:12px;white-space:pre-wrap}
#wrap{overflow:hidden;border:1px solid var(--grid);border-radius:6px;cursor:grab;touch-action:none}
svg text{fill:var(--fg);font-size:11px} .lg span{display:inline-block;width:10px;height:10px;margin:0 4px 0 12px;border-radius:2px;vertical-align:-1px}
</style></head><body>
<h1>fcapz event timeline</h1>
<div class="mut">__SRC__ · t = 0 at the trigger sample · wheel/pinch to zoom, drag to pan, double-click to reset</div>
<div class="lg mut">slot <span style="background:var(--s0)"></span>0<span style="background:var(--s1)"></span>1<span style="background:var(--s2)"></span>2<span style="background:var(--s3)"></span>none
<span style="background:var(--col)"></span>collision</div>
<div id="wrap"><svg id="svg" role="img" aria-label="Swimlane timeline of camera, ISR, worker, mbdpu03 and display events"></svg></div>
<pre id="sum">__SUMMARY__</pre>
<script>
const L=__LANES__, T0=__T0__, T1=__T1__;
const svg=document.getElementById('svg'), wrap=document.getElementById('wrap');
const LW=170, RH=22, TOP=24; let a=T0, b=T1;
const css=v=>getComputedStyle(document.documentElement).getPropertyValue(v);
function draw(){
  const W=wrap.clientWidth, H=TOP+L.length*RH+8, pw=W-LW-8;
  svg.setAttribute('viewBox',`0 0 ${W} ${H}`); svg.setAttribute('width',W); svg.setAttribute('height',H);
  const x=t=>LW+(t-a)/(b-a)*pw; let s='';
  const span=b-a, raw=span/8, p=Math.pow(10,Math.floor(Math.log10(raw))), st=[1,2,5,10].map(k=>k*p).find(k=>k>=raw);
  for(let t=Math.ceil(a/st)*st;t<=b;t+=st){const X=x(t);
    s+=`<line x1="${X}" x2="${X}" y1="${TOP-6}" y2="${H}" stroke="${css('--grid')}"/>`;
    s+=`<text x="${X+2}" y="${TOP-10}">${span>5000?(t/1000).toFixed(st>=1000?0:1)+' ms':t.toFixed(st>=1?0:2)+' µs'}</text>`;}
  L.forEach((ln,i)=>{const y=TOP+i*RH;
    s+=`<text x="4" y="${y+14}">${ln.title}</text><line x1="${LW}" x2="${W}" y1="${y+RH}" y2="${y+RH}" stroke="${css('--grid')}"/>`;
    for(const [t0,t1,v] of ln.segs){ if(t1<a||t0>b) continue;
      const X0=Math.max(x(t0),LW), X1=Math.min(x(t1),W), w=Math.max(X1-X0,1.2);
      let c=ln.kind==='bit'?(ln.field.startsWith('col')?css('--col'):css('--bit')):ln.kind==='val'?css('--bit'):css('--s'+v);
      if(ln.kind==='step'&&v===3) c=css('--s3');
      s+=`<rect x="${X0}" y="${y+4}" width="${w}" height="${RH-8}" rx="2" fill="${c}"><title>${ln.title}: ${(t0/1000).toFixed(3)}–${(t1/1000).toFixed(3)} ms (${(t1-t0).toFixed(1)} µs)${ln.kind==='val'?' = '+v:ln.kind!=='bit'?' slot '+v:''}</title></rect>`;
      if(ln.kind!=='bit'&&w>(ln.kind==='val'?30:14)) s+=`<text x="${X0+3}" y="${y+15}" style="fill:#fff">${ln.kind==='val'?v:(v===3?'–':v)}</text>`;}
  });
  svg.innerHTML=s;
}
function zoom(cx,f){const W=wrap.clientWidth,pw=W-LW-8;const t=a+(Math.max(cx-LW,0))/pw*(b-a);a=t-(t-a)*f;b=t+(b-t)*f;draw();}
wrap.addEventListener('wheel',e=>{e.preventDefault();zoom(e.offsetX,e.deltaY>0?1.25:0.8);},{passive:false});
let drag=null; wrap.addEventListener('pointerdown',e=>{drag={x:e.clientX,a,b};wrap.setPointerCapture(e.pointerId);});
wrap.addEventListener('pointermove',e=>{if(!drag)return;const pw=wrap.clientWidth-LW-8,dt=(e.clientX-drag.x)/pw*(drag.b-drag.a);a=drag.a-dt;b=drag.b-dt;draw();});
wrap.addEventListener('pointerup',()=>drag=null);
wrap.addEventListener('dblclick',()=>{a=T0;b=T1;draw();});
addEventListener('resize',draw); draw();
</script></body></html>"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("capture", help="fcapz JSON export (with probe map and timestamps)")
    ap.add_argument("-o", "--out", help="HTML output (default: <capture>.html)")
    args = ap.parse_args()
    d, rows = load(args.capture)
    import contextlib, io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        summary(rows)
    text = buf.getvalue()
    print(text, end="")
    out = args.out or args.capture.rsplit(".", 1)[0] + ".html"
    html = (HTML.replace("__LANES__", json.dumps(lanes_data(rows)))
                .replace("__T0__", str(rows[0]["t_us"])).replace("__T1__", str(rows[-1]["t_us"]))
                .replace("__SRC__", args.capture.rsplit("/", 1)[-1])
                .replace("__SUMMARY__", text.replace("&", "&amp;").replace("<", "&lt;")))
    open(out, "w").write(html)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
