#!/usr/bin/env python3
"""Parse the SCU200 ROI training-data gallery (dumped by scripts/dump_gallery.tcl) into labeled
images + a manifest + training .npz files.

Each captured sample holds the exact 28x28 INT8 the CNN saw, a 112x112 grayscale hi-res crop,
and metadata (strobe `position`, predicted digit, brightness, logits). Because the strobe fires
at N fixed angular positions per revolution, samples cluster into N groups by `position` -- so we
can AUTO-LABEL by angular slot: give --map with the digit at each slot (slots ordered by
ascending position) and every sample is labeled with no manual work.

Usage:
  python3 tools/roi_export.py --bin train/captures/roi_gallery.bin --out train/captures/mnist_takeNN
      -> writes crop/ + img28/ PNGs, manifest.csv, and prints the detected slot centers.
  python3 tools/roi_export.py --bin train/captures/roi_gallery.bin --out train/captures/mnist_takeNN \
      --map 1,2,3,4,5,6,7,8
      -> also assigns labels (slot0=1, slot1=2, ...) and writes the two aligned datasets:
         captures_28x28.npz   (N x 28 x 28   uint8 0..127, the model input)
         captures_112x112.npz (N x 112 x 112 uint8 0..255, the raw hi-res crop)

Options:
  --nslots N      force N clusters (default: use num_fires from the header)
  --min-bright V  drop samples dimmer than V (QC; default 0 = keep all)
"""
import argparse, struct, os, csv, sys
import numpy as np

GAL_MAGIC = 0x47494F52  # 'ROIG'
SMP_MAGIC = 0x32494F52  # 'ROI2'

def u32(b, o): return struct.unpack_from('<I', b, o)[0]

def parse(path):
    with open(path, 'rb') as f: data = f.read()
    if u32(data, 0) != GAL_MAGIC:
        sys.exit("bad gallery magic 0x%08x (expected 0x%08x)" % (u32(data,0), GAL_MAGIC))
    count   = u32(data, 4);  stride  = u32(data, 12); galhdr = u32(data, 16)
    smphdr  = u32(data, 20); imgdim  = u32(data, 24); imgb   = u32(data, 28)
    cropdim = u32(data, 32); cropb   = u32(data, 36)
    print("gallery: count=%d stride=%d img=%dx%d crop=%dx%d" % (count, stride, imgdim, imgdim, cropdim, cropdim))
    samples = []
    for i in range(count):
        base = galhdr + i * stride
        if u32(data, base) != SMP_MAGIC:
            print("  warn: sample %d bad magic, stopping" % i); break
        seq  = u32(data, base+4);  pos = u32(data, base+8);  rev = u32(data, base+12)
        nfir = u32(data, base+16); pred= u32(data, base+20);  mean= u32(data, base+24)
        logits = struct.unpack_from('<10i', data, base+32)
        img28  = np.frombuffer(data, np.int8,  imgb,  base+smphdr).reshape(imgdim, imgdim)
        crop   = np.frombuffer(data, np.uint8, cropb, base+smphdr+imgb).reshape(cropdim, cropdim)
        samples.append(dict(seq=seq, pos=pos, rev=rev, nfir=nfir, pred=pred, mean=mean,
                            logits=logits, img28=img28, crop=crop))
    return samples

def cluster_slots(positions, nslots):
    """Assign each position to one of nslots evenly-spaced angular slots, CIRCULARLY (position
    wraps at cpr). A naive linear k-means mislabels captures that land just past the wrap (e.g.
    the last digit's frame completing after position rolls 979->0), dumping them into slot 0.
    Fix: estimate the fire pitch, then slot = round((pos - c0)/step) mod N so wrapped samples
    fold back to the correct slot. Returns (slot_per_sample, slot_centers) ordered by position."""
    p = np.asarray(positions, float)
    if len(p) == 0: return np.array([]), np.array([])
    # rough linear k-means for initial centers (good enough to derive pitch + slot0 anchor)
    centers = np.quantile(p, np.linspace(0, 1, nslots*2+1)[1::2])
    for _ in range(50):
        assign = np.abs(p[:, None] - centers[None, :]).argmin(1)
        new = np.array([p[assign == k].mean() if np.any(assign == k) else centers[k]
                        for k in range(nslots)])
        if np.allclose(new, centers): break
        centers = new
    centers = np.sort(centers)
    step = float(np.median(np.diff(centers))) if nslots > 1 else max(1.0, p.max()+1)
    cpr  = step * nslots
    c0   = centers[0]
    slot = (np.rint((p - c0) / step).astype(int)) % nslots     # circular assignment
    # circular mean per slot (unwrap around the expected center) for reporting
    cen = []
    for k in range(nslots):
        pk = p[slot == k]; exp = c0 + k*step
        if len(pk) == 0: cen.append(exp % cpr); continue
        pu = pk + np.where(pk < exp - cpr/2, cpr, 0.0) - np.where(pk > exp + cpr/2, cpr, 0.0)
        cen.append(float(pu.mean()) % cpr)
    return slot, np.array(cen)

def save_png(arr, path):
    try:
        from PIL import Image
        Image.fromarray(arr).save(path)
    except ImportError:
        np.save(path.replace('.png', '.npy'), arr)   # fallback if Pillow absent

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default='train/captures/roi_gallery.bin')
    ap.add_argument('--out', default='train/captures/mnist_take')
    ap.add_argument('--map', default=None, help='comma list: digit per slot, slots by ascending position')
    ap.add_argument('--nslots', type=int, default=0, help='override N (default: header num_fires)')
    ap.add_argument('--min-bright', type=int, default=0)
    a = ap.parse_args()

    s = parse(a.bin)
    s = [x for x in s if x['mean'] >= a.min_bright]
    if not s: sys.exit("no samples after brightness filter")

    nslots = a.nslots or max(1, s[0]['nfir'])
    slots, centers = cluster_slots([x['pos'] for x in s], nslots)
    print("detected %d angular slots at positions: %s" %
          (nslots, ", ".join("%.0f" % c for c in centers)))
    for x, sl in zip(s, slots): x['slot'] = int(sl)

    dmap = None
    if a.map:
        dmap = [int(v) for v in a.map.split(',')]
        if len(dmap) != nslots:
            sys.exit("--map has %d entries but %d slots" % (len(dmap), nslots))
        for x in s: x['label'] = dmap[x['slot']]

    os.makedirs(os.path.join(a.out, 'crop'), exist_ok=True)
    os.makedirs(os.path.join(a.out, 'img28'), exist_ok=True)
    with open(os.path.join(a.out, 'manifest.csv'), 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['seq','pos','rev','slot','label','pred','mean'])
        for x in s:
            lab = x.get('label', '')
            sub = str(lab) if dmap else ('slot%d' % x['slot'])
            for d in ('crop', 'img28'): os.makedirs(os.path.join(a.out, d, sub), exist_ok=True)
            save_png(x['crop'], os.path.join(a.out, 'crop', sub, 'c_%05d.png' % x['seq']))
            # 28x28 model input is 0..127 -> scale to 0..255 for viewing
            save_png((np.clip(x['img28'].astype(np.int16), 0, 127) * 2).astype(np.uint8),
                     os.path.join(a.out, 'img28', sub, 'i_%05d.png' % x['seq']))
            w.writerow([x['seq'], x['pos'], x['rev'], x['slot'], lab, x['pred'], x['mean']])

    print("wrote %d samples -> %s/{crop,img28}/ + manifest.csv" % (len(s), a.out))
    if dmap:
        # two row-for-row aligned datasets (same sample order as manifest.csv)
        X28  = np.stack([(np.clip(x['img28'].astype(np.int16),0,127)).astype(np.uint8) for x in s])
        X112 = np.stack([x['crop'].astype(np.uint8) for x in s])
        y    = np.array([x['label'] for x in s], np.int64)
        np.savez_compressed(os.path.join(a.out, 'captures_28x28.npz'),   images=X28,  labels=y)
        np.savez_compressed(os.path.join(a.out, 'captures_112x112.npz'), images=X112, labels=y)
        print("wrote captures_28x28.npz   images %s (0..127 uint8), labels %s" % (X28.shape,  y.shape))
        print("wrote captures_112x112.npz images %s (0..255 uint8), labels %s" % (X112.shape, y.shape))
        # class balance
        u, c = np.unique(y, return_counts=True)
        print("label counts: " + ", ".join("%d:%d" % (d, n) for d, n in zip(u, c)))
    else:
        print("no --map given: inspect crop/slotN/ folders, then re-run with "
              "--map <digit-per-slot> to write labels + the .npz datasets")

if __name__ == '__main__':
    main()
