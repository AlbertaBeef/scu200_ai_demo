#!/usr/bin/env python3
# patch_uram.py — turn mbdpu03's HLS-generated uram_* memories into UltraRAM.
# SPDX-License-Identifier: Apache-2.0
#
# Vitis HLS 2025.2 rejects `bind_storage impl=uram` for the beta xcsu200p (HLS 214-370), although
# the part has 64 URAM288 that Vivado can infer. So mbdpu03 declares those arrays impl=bram and
# this script rewrites the exported RAM modules (file names containing "uram_") so Vivado maps
# them to URAM:
#   - ram_style "block" -> "ultra"
#   - drop the `initial $readmemh(...)` block (URAM has no initial contents; the arrays are
#     never read before being written)
#   - write ports: read-first -> no-change (q holds on a write). HLS never consumes read data
#     from a write access, so behaviour is unchanged; URAM inference needs it.
# Idempotent. Usage: patch_uram.py <dir> [<dir> ...]   (e.g. sol1/impl/verilog sol1/impl/ip/hdl/verilog)
import glob, os, re, sys

def patch(path):
    s = open(path).read()
    o = s
    s = s.replace('(* ram_style = "block" *)', '(* ram_style = "ultra" *)')
    s = re.sub(r'\ninitial begin\s*\n\s*\$readmemh\([^)]*\);\s*\nend\s*\n', '\n', s)
    # "if (weN) ram[addressN] <= dN;  qN <= ram[addressN];"  ->  "... else qN <= ram[addressN];"
    s = re.sub(r'(if \(we(\d+)\)\s*\n\s*ram\[address\2\] <= d\2;\s*\n)(\s*)(q\2 <= ram\[address\2\];)',
               r'\1\3else\n\3    \4', s)
    if s != o:
        open(path, 'w').write(s)
    ok = 'ram_style = "ultra"' in s and '$readmemh' not in s
    return s != o, ok

def main():
    files = []
    for d in sys.argv[1:]:
        files += sorted(glob.glob(os.path.join(d, '*uram_*RAM*.v')))
    if not files:
        print('patch_uram: no uram_* RAM modules found in', sys.argv[1:]); return 1
    bad = 0
    for f in files:
        changed, ok = patch(f)
        bad += not ok
        print(f'patch_uram: {"patched " if changed else "already  "} {"ok " if ok else "BAD"} {f}')
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
