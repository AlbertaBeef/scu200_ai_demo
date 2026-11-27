# Regenerate the standalone light/dark SVGs from a diagram page.
# Usage: python3 make_svgs.py <page>.html [<page>.html ...] <out_dir>
#   writes <out_dir>/<page>_light.svg and <page>_dark.svg for each page.
# The size comes from the page's <svg class="dp"> viewBox, the title from its aria-label and the
# legend from the page's <div class="legend">, so one script serves every demo's page.
import html as H, re, sys, pathlib

T = {"light": dict(bg="#ffffff",ink="#18202b",mute="#5b6675",line="#6b7684",box="#f3f5f8",boxs="#8d98a6",teal="#e1f5ee",teals="#0f6e56",tealt="#085041",vio="#eeedfe",vios="#534ab7",viot="#3c3489",coral="#d85a30"),
     "dark":  dict(bg="#11151c",ink="#e6eaf0",mute="#9aa5b4",line="#8d98a6",box="#1b222c",boxs="#5b6675",teal="#0b3f33",teals="#5dcaa5",tealt="#c9f1e2",vio="#2b2563",vios="#afa9ec",viot="#dcd9fb",coral="#d85a30")}
css = """text{font-family:"Red Hat Text",system-ui,-apple-system,"Segoe UI",Roboto,"DejaVu Sans",sans-serif;fill:var(--ink)}
.t{font-size:14px;font-weight:500}.s{font-size:12px;fill:var(--mute)}
.b rect{fill:var(--box);stroke:var(--boxs)}
.c rect{fill:var(--teal);stroke:var(--teals)}.c .t{fill:var(--tealt)}.c .s{fill:var(--teals)}
.d rect{fill:var(--vio);stroke:var(--vios)}.d .t{fill:var(--viot)}.d .s{fill:var(--vios)}
.a{stroke:var(--line);stroke-width:1.2;fill:none}
.reg rect{fill:none}.reg .k{font-weight:500;fill:var(--tealt)}
.lg{font-size:12px}"""
SW = {"c": '<rect x="{x}" y="2" width="12" height="12" rx="3" fill="var(--teal)" stroke="var(--teals)"/>',
      "d": '<rect x="{x}" y="2" width="12" height="12" rx="3" fill="var(--vio)" stroke="var(--vios)"/>',
      "l": '<line x1="{x}" y1="8" x2="{x2}" y2="8" class="a" stroke-dasharray="4 3"/>'}


def legend(page, h):
    items = re.findall(r'<i class="sw (\w)"></i>([^<]+)</span>', re.search(r'<div class="legend">.*?</div>', page, re.S).group(0))
    out, x = [f'<g transform="translate(0,{h - 20})">'], 40
    for k, label in items:
        sw = SW[k].format(x=x, x2=x + 18)
        tx = x + (24 if k == "l" else 18)
        out.append(f'{sw}<text class="s" x="{tx}" y="8" dominant-baseline="central">{label}</text>')
        x = tx + 6.3 * len(H.unescape(label)) + 24        # ~6.3 px per 12 px glyph + gap
    return "".join(out) + "</g>"


def main():
    *pages, out = sys.argv[1:]
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    for p in map(pathlib.Path, pages):
        page = p.read_text()
        svg = re.search(r'<svg class="dp".*?</svg>', page, re.S).group(0)
        w, h = map(int, re.search(r'viewBox="0 0 (\d+) (\d+)"', svg).groups())
        title = re.search(r'aria-label="([^"]+)"', svg).group(1).replace("Data path block diagram of ", "") + " data path"
        hh = h + 10                                           # room for the legend row
        svg = re.sub(r'<svg class="dp" viewBox="0 0 \d+ \d+"',
                     f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {hh}" width="{2 * w}" height="{2 * hh}"', svg, count=1)
        for name, c in T.items():
            s = svg.replace('<defs>', f'<title>{title}</title><style>{css}</style><rect width="{w}" height="{hh}" fill="{c["bg"]}"/><defs>', 1)
            s = s.replace('</svg>', legend(page, h) + '</svg>')
            s = re.sub(r'var\(--(\w+)\)', lambda m: c[m.group(1)], s)
            (out / f"{p.stem}_{name}.svg").write_text(s)
            print(out / f"{p.stem}_{name}.svg")


if __name__ == "__main__":
    main()
