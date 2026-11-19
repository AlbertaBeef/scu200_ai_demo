#!/usr/bin/env python3
# train_mnist_pos.py — train + PTQ the mbdpu02 CNN: the mbdpu01 MNIST classifier PLUS a 2-output
# position head (dx, dy) that regresses where the digit sits inside the 28x28 ROI. Exports the
# INT8 DDR blobs that the mbdpu02 HLS core consumes (must stay bit-exact to mbdpu02.cpp).
#
# Network (shared features, one wider FC):
#   conv1 1->8 3x3 pad1, ReLU, pool2 -> conv2 8->16 3x3 pad1, ReLU, pool2 -> fc 784->12
#   fc rows 0..9  = class logits (as mbdpu01)
#   fc rows 10,11 = dx, dy position head (raw int32 accumulators on the FPGA, no requant)
#
# Position convention (28x28 model pixels, 0 = centred, centre = 13.5):
#   dx = cx - 13.5        (left -ve, right +ve)
#   dy = -(cy - 13.5)     (top +ve, bottom -ve)
# where (cx, cy) is the INK CENTRE OF MASS of the model input. No capture records a position, so
# labels are synthesised: every training image is randomly shifted and its label is the ink COM
# measured on the shifted image (one definition for MNIST and real captures alike).
#   - MNIST (inverted, 28x28): integer shifts of +-MNIST_SHIFT px, padded with white.
#   - Real captures (captures_112x112.npz): shifts of +-REAL_SHIFT camera px on the 112 crop,
#     padded with the crop's border median, then 4x4 box-average >> 1 to 28x28 exactly as the
#     mbpre core does -> 0.25 px label resolution.
#
# Blobs (prefix-compatible with mbdpu01: the head rows are appended to fc_w, the head params to
# the end of the pblob):
#   mnist_wblob.bin  int8  10632: conv1_w[8][1][3][3], conv2_w[16][8][3][3], fc_w[12][784]
#   mnist_pblob.bin  int32 42:    c1_b[8], c2_b[16], fc_b[10], c1_M0,c1_sh, c2_M0,c2_sh,
#                                 pos_b[2], pos_k, pos_sh
#     firmware: px*100 = (acc * pos_k) >> pos_sh   for acc = out[11] (dx), out[12] (dy)
#   mnist_tests.bin         20 x (784 int8 img + 1 int8 label)   (same format as mbdpu01)
#   mnist_tests_expect.bin  20 x int32[13] expected core output (bit-exact) for HLS csim / HW
#
# Run from the repo root with the venv, e.g.:
#   venv/bin/python3 Vivado/ip/mbdpu02/train_mnist_pos.py --data data --out train/mbdpu02 \
#       --real train/captures/mnist_take0{1,2,3}/captures_112x112.npz
import argparse, struct, os, time, numpy as np

MNIST_SHIFT = 6      # +- px (28-px space)
REAL_SHIFT  = 24     # +- camera px on the 112 crop (= +-6 model px)
POS_CLIP    = 6.0    # label clip, px
WBLOB_N, PBLOB_N, OUT_N = 10632, 42, 13

def q_multiplier(M):
    """Real multiplier M in (0,1) -> (M0, shift), M ~= M0 * 2^-shift, M0 <= 16 bits (fits the
    core's ap_int<18> requant multiplier). Identical to mbdpu01/train_mnist.py."""
    if M <= 0: return 0, 0
    shift = 0
    while M < 0.5: M *= 2.0; shift += 1
    M0 = int(round(M * (1 << 16))); shift += 16
    if M0 == (1 << 16): M0 >>= 1; shift -= 1
    return M0, shift

def requant(acc, bias, M0, sh, relu):
    x = (acc.astype(np.int64) + bias) * np.int64(M0)
    r = (x + (1 << (sh - 1))) >> sh
    if relu: r = np.maximum(r, 0)
    return np.clip(r, -127, 127).astype(np.int32)

# ---------------------------------------------------------------------------------------------
# Labels: ink centre of mass on a [0,1] inverted image (light background, dark ink)
# ---------------------------------------------------------------------------------------------
_YY, _XX = np.mgrid[0:28, 0:28].astype(np.float32)

def ink_com(imgs):
    """imgs (N,28,28) float [0,1] -> (N,2) [dx, dy] px, clipped to +-POS_CLIP.
    ink = max(0, bg - v - 0.25*bg) with bg = border-ring median, so background texture and
    vignetting below 25 % of bg contribute nothing. Empty images -> (0,0)."""
    imgs = np.asarray(imgs, np.float32)
    ring = np.concatenate([imgs[:, 0, :], imgs[:, -1, :], imgs[:, 1:-1, 0], imgs[:, 1:-1, -1]], 1)
    bg = np.median(ring, axis=1)[:, None, None]
    w = np.maximum(0.0, 0.75 * bg - imgs)
    s = w.sum(axis=(1, 2))
    ok = s > 1e-6
    cx = np.where(ok, (w * _XX).sum(axis=(1, 2)) / np.maximum(s, 1e-6), 13.5)
    cy = np.where(ok, (w * _YY).sum(axis=(1, 2)) / np.maximum(s, 1e-6), 13.5)
    pos = np.stack([cx - 13.5, -(cy - 13.5)], 1)
    return np.clip(pos, -POS_CLIP, POS_CLIP).astype(np.float32)

# ---------------------------------------------------------------------------------------------
# Augmentation
# ---------------------------------------------------------------------------------------------
def shift_mnist(x, rng, smax=MNIST_SHIFT):
    """x (N,28,28) float inverted (bg=1). Integer shift per image, pad white. Returns new array.
    Grouped by (sx,sy) so it is a handful of vectorised slices, not N python iterations."""
    n = x.shape[0]
    sx = rng.integers(-smax, smax + 1, n); sy = rng.integers(-smax, smax + 1, n)
    P = smax
    xp = np.pad(x, ((0, 0), (P, P), (P, P)), constant_values=1.0)
    out = np.empty_like(x)
    for dx in range(-smax, smax + 1):
        for dy in range(-smax, smax + 1):
            idx = np.where((sx == dx) & (sy == dy))[0]
            if idx.size == 0: continue
            # content moves right by dx -> source window starts dx to the left
            out[idx] = xp[idx, P - dy:P - dy + 28, P - dx:P - dx + 28]
    return out

def pool112(c):
    """112x112 uint8 crop(s) -> 28x28 uint8 0..127, exactly as mbpre: (sum4x4 >> 4) >> 1."""
    c = c.astype(np.int32)
    s = c.reshape(*c.shape[:-2], 28, 4, 28, 4).sum(axis=(-3, -1))
    return ((s >> 4) >> 1).astype(np.uint8)

def shift_real(c112, sx, sy):
    """One 112x112 crop shifted by (sx, sy) camera px (content moves right/down for +ve),
    padded with its border median, pooled to 28x28 [0,1]."""
    bg = np.median(np.concatenate([c112[0], c112[-1], c112[1:-1, 0], c112[1:-1, -1]]))
    P = REAL_SHIFT
    cp = np.pad(c112, P, constant_values=int(bg))
    w = cp[P - sy:P - sy + 112, P - sx:P - sx + 112]
    return pool112(w).astype(np.float32) / 127.0

def real_augment(crops, labels, k, rng, include_zero=True):
    xs, ys = [], []
    for c, l in zip(crops, labels):
        for j in range(k):
            if include_zero and j == 0: sx = sy = 0
            else: sx, sy = rng.integers(-REAL_SHIFT, REAL_SHIFT + 1, 2)
            xs.append(shift_real(c, sx, sy)); ys.append(l)
    return np.stack(xs), np.array(ys, np.int64)

# ---------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--epochs', type=int, default=6)
    ap.add_argument('--out', default='.')
    ap.add_argument('--data', default='data', help='torchvision MNIST root')
    ap.add_argument('--real', nargs='+', default=None,
                    help='one or more captures_112x112.npz (shifted in 112-px space)')
    ap.add_argument('--oversample', type=int, default=40,
                    help='shifted variants per real train sample (replaces plain repetition)')
    ap.add_argument('--holdout', type=float, default=0.15)
    ap.add_argument('--lam', type=float, default=0.5, help='position loss weight')
    ap.add_argument('--shift-frac', type=float, default=1.0,
                    help='fraction of MNIST images shifted each epoch (rest stay centred)')
    ap.add_argument('--seed', type=int, default=0)
    ap.add_argument('--ntest', type=int, default=20)
    args = ap.parse_args()

    import torch, torch.nn as nn, torch.nn.functional as F
    from torchvision import datasets
    torch.manual_seed(args.seed); rng = np.random.default_rng(args.seed)
    torch.set_num_threads(max(1, os.cpu_count() or 1))

    class Net(nn.Module):
        def __init__(s):
            super().__init__()
            s.c1 = nn.Conv2d(1, 8, 3, padding=1)
            s.c2 = nn.Conv2d(8, 16, 3, padding=1)
            s.fc = nn.Linear(7 * 7 * 16, 12)
        def feats(s, x):
            x = F.max_pool2d(F.relu(s.c1(x)), 2)
            x = F.max_pool2d(F.relu(s.c2(x)), 2)
            return x
        def forward(s, x):
            x = s.feats(x)
            x = x.permute(0, 2, 3, 1).reshape(x.size(0), -1)   # [h][w][c] to match HLS FC
            o = s.fc(x)
            return o[:, :10], o[:, 10:]

    tr = datasets.MNIST(args.data, train=True,  download=True)
    te = datasets.MNIST(args.data, train=False, download=True)
    mnist_x = 1.0 - tr.data.numpy().astype(np.float32) / 255.0      # inverted [0,1]
    mnist_y = tr.targets.numpy().astype(np.int64)
    test_x  = 1.0 - te.data.numpy().astype(np.float32) / 255.0
    test_y  = te.targets.numpy().astype(np.int64)

    # ---- real captures (112 crops), stratified holdout per class ----
    real = None
    if args.real:
        cr, lb = [], []
        for rp in args.real:
            d = np.load(rp); cr.append(d['images']); lb.append(d['labels'].astype(np.int64))
            print(f'  loaded {len(d["labels"])} from {rp}')
        cr = np.concatenate(cr); lb = np.concatenate(lb)
        tri, vli = [], []
        for c in np.unique(lb):
            idx = np.where(lb == c)[0]; rng.shuffle(idx)
            k = max(1, int(len(idx) * args.holdout)); vli += list(idx[:k]); tri += list(idx[k:])
        tri, vli = np.array(tri), np.array(vli)
        t0 = time.time()
        rx, ry = real_augment(cr[tri], lb[tri], args.oversample, rng)
        vx0 = np.stack([shift_real(c, 0, 0) for c in cr[vli]])               # centred holdout
        vxs, vys = real_augment(cr[vli], lb[vli], 4, rng, include_zero=False)  # shifted holdout
        real = dict(tx=rx, ty=ry, tp=ink_com(rx), vx0=vx0, vy0=lb[vli], vxs=vxs, vys=vys,
                    vps=ink_com(vxs))
        print(f'real: {len(tri)} train (x{args.oversample} shifted = {len(ry)}), '
              f'{len(vli)} holdout; augment {time.time()-t0:.1f}s')

    # fixed shifted MNIST test set for position metrics
    ts_x = shift_mnist(test_x[:2000], np.random.default_rng(args.seed + 1))
    ts_p = ink_com(ts_x)

    net = Net()
    opt = torch.optim.Adam(net.parameters(), 1e-3)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, args.epochs)
    T = lambda a: torch.from_numpy(np.ascontiguousarray(a))

    def float_eval():
        net.eval()
        with torch.no_grad():
            lg, _ = net(T(test_x[:5000]).unsqueeze(1))
            acc = (lg.argmax(1).numpy() == test_y[:5000]).mean()
            _, pp = net(T(ts_x).unsqueeze(1))
            mae = np.abs(pp.numpy() - ts_p).mean(0)
            msg = f'MNIST acc {100*acc:.2f}% | shifted-MNIST pos MAE dx {mae[0]:.3f} dy {mae[1]:.3f} px'
            if real is not None:
                lg, _ = net(T(real['vx0']).unsqueeze(1))
                ra = (lg.argmax(1).numpy() == real['vy0']).mean()
                _, pp = net(T(real['vxs']).unsqueeze(1))
                rm = np.abs(pp.numpy() - real['vps']).mean(0)
                msg += f' | real acc {100*ra:.2f}% pos MAE dx {rm[0]:.3f} dy {rm[1]:.3f} px'
        return msg

    for ep in range(args.epochs):
        t0 = time.time()
        mx = mnist_x.copy()                                # fresh shifts every epoch
        sel = rng.random(len(mx)) < args.shift_frac
        mx[sel] = shift_mnist(mnist_x[sel], rng)
        X = [mx]; Y = [mnist_y]; P = [ink_com(mx)]
        if real is not None: X.append(real['tx']); Y.append(real['ty']); P.append(real['tp'])
        X = T(np.concatenate(X)).unsqueeze(1); Y = T(np.concatenate(Y)); P = T(np.concatenate(P))
        dl = torch.utils.data.DataLoader(torch.utils.data.TensorDataset(X, Y, P),
                                         batch_size=128, shuffle=True)
        net.train()
        for x, y, p in dl:
            opt.zero_grad()
            lg, pp = net(x)
            loss = F.cross_entropy(lg, y) + args.lam * F.smooth_l1_loss(pp, p)
            loss.backward(); opt.step()
        sched.step()
        print(f'epoch {ep}: {float_eval()}  ({time.time()-t0:.0f}s)')

    # ------------------------------------------------------------------ PTQ
    net.eval()
    W1, B1 = net.c1.weight.detach().numpy(), net.c1.bias.detach().numpy()
    W2, B2 = net.c2.weight.detach().numpy(), net.c2.bias.detach().numpy()
    Wf, Bf = net.fc.weight.detach().numpy(), net.fc.bias.detach().numpy()
    Wc, Bc, Wp, Bp = Wf[:10], Bf[:10], Wf[10:], Bf[10:]

    s_in = 1.0 / 127.0
    sw = lambda w: np.abs(w).max() / 127.0
    s_w1, s_w2, s_wc, s_wp = sw(W1), sw(W2), sw(Wc), sw(Wp)
    cal = [test_x[:500], ts_x[:500]]
    if real is not None: cal += [real['vx0'], real['tx'][::40][:500]]
    xs = T(np.concatenate(cal)).unsqueeze(1)
    with torch.no_grad():
        a1 = F.relu(net.c1(xs)); s_o1 = a1.abs().max().item() / 127.0
        a2 = F.relu(net.c2(F.max_pool2d(a1, 2))); s_o2 = a2.abs().max().item() / 127.0

    q = lambda w, s: np.clip(np.round(w / s), -127, 127).astype(np.int8)
    W1q, W2q, Wcq, Wpq = q(W1, s_w1), q(W2, s_w2), q(Wc, s_wc), q(Wp, s_wp)
    B1q = np.round(B1 / (s_in * s_w1)).astype(np.int32)
    B2q = np.round(B2 / (s_o1 * s_w2)).astype(np.int32)
    Bcq = np.round(Bc / (s_o2 * s_wc)).astype(np.int32)
    Bpq = np.round(Bp / (s_o2 * s_wp)).astype(np.int32)
    c1_M0, c1_sh = q_multiplier(s_in * s_w1 / s_o1)
    c2_M0, c2_sh = q_multiplier(s_o1 * s_w2 / s_o2)
    pos_scale = s_o2 * s_wp * 100.0            # px*100 per accumulator LSB
    pos_sh = 24
    while pos_scale * (1 << pos_sh) >= (1 << 22) and pos_sh > 8: pos_sh -= 1
    while pos_scale * (1 << pos_sh) <  (1 << 15) and pos_sh < 40: pos_sh += 1
    pos_k = int(round(pos_scale * (1 << pos_sh)))
    assert (1 << 14) <= pos_k < (1 << 23), pos_k

    wblob = np.concatenate([W1q.reshape(-1), W2q.reshape(-1), Wcq.reshape(-1),
                            Wpq.reshape(-1)]).astype(np.int8)
    pblob = np.concatenate([B1q, B2q, Bcq,
                            np.array([c1_M0, c1_sh, c2_M0, c2_sh], np.int32),
                            Bpq, np.array([pos_k, pos_sh], np.int32)]).astype(np.int32)
    assert wblob.size == WBLOB_N and pblob.size == PBLOB_N, (wblob.size, pblob.size)
    os.makedirs(args.out, exist_ok=True)
    wblob.tofile(os.path.join(args.out, 'mnist_wblob.bin'))
    pblob.tofile(os.path.join(args.out, 'mnist_pblob.bin'))

    Wfq = np.concatenate([Wcq, Wpq]).astype(np.int64)        # [12][784] as the core sees it
    Bfq = np.concatenate([Bcq, Bpq]).astype(np.int64)

    def core_int8(xq):
        """Bit-exact model of mbdpu02: int8 image (28,28) 0..127 -> int32 out[13]."""
        x = xq.astype(np.int32)
        a1 = np.zeros((28, 28, 8), np.int32); Xp = np.pad(x, 1)
        for oc in range(8):
            acc = sum(Xp[kh:kh+28, kw:kw+28] * int(W1q[oc, 0, kh, kw]) for kh in range(3) for kw in range(3))
            a1[:, :, oc] = requant(acc, B1q[oc], c1_M0, c1_sh, True)
        p1 = a1.reshape(14, 2, 14, 2, 8).max(axis=(1, 3))
        a2 = np.zeros((14, 14, 16), np.int32); Pp = np.pad(p1, ((1, 1), (1, 1), (0, 0)))
        for oc in range(16):
            acc = np.zeros((14, 14), np.int64)
            for ic in range(8):
                for kh in range(3):
                    for kw in range(3):
                        acc += Pp[kh:kh+14, kw:kw+14, ic] * int(W2q[oc, ic, kh, kw])
            a2[:, :, oc] = requant(acc, B2q[oc], c2_M0, c2_sh, True)
        p2 = a2.reshape(7, 2, 7, 2, 16).max(axis=(1, 3))
        o = Wfq @ p2.reshape(-1).astype(np.int64) + Bfq
        out = np.zeros(OUT_N, np.int64)
        out[:10] = o[:10]; out[10] = int(np.argmax(o[:10])); out[11:13] = o[10:12]
        assert np.all(np.abs(out) < 2**31)
        return out.astype(np.int32)

    to_q = lambda img01: np.clip(np.round(img01 / s_in), 0, 127).astype(np.int8)
    to_px = lambda acc: ((int(acc) * pos_k) >> pos_sh) / 100.0     # firmware conversion

    def run(xs):
        o = np.stack([core_int8(to_q(x)) for x in xs])
        pos = np.array([[to_px(a), to_px(b)] for a, b in o[:, 11:13]], np.float32)
        return o[:, 10], pos

    t0 = time.time()
    pr, _ = run(test_x[:2000]); print(f'INT8 MNIST acc (centred, 2000): {100*np.mean(pr == test_y[:2000]):.2f}%')
    pr, pp = run(ts_x[:1000])
    mae = np.abs(pp - ts_p[:1000]).mean(0)
    print(f'INT8 shifted-MNIST (1000): acc {100*np.mean(pr == test_y[:1000]):.2f}%  '
          f'pos MAE dx {mae[0]:.3f} dy {mae[1]:.3f} px')
    if real is not None:
        pr, _ = run(real['vx0'])
        print(f'INT8 real-holdout acc (centred, {len(pr)}): {100*np.mean(pr == real["vy0"]):.2f}%')
        for d in sorted(set(real['vy0'].tolist())):
            m = real['vy0'] == d
            print(f'   real digit {d}: {100*np.mean(pr[m] == d):.1f}%  (n={m.sum()})')
        pr, pp = run(real['vxs'])
        mae = np.abs(pp - real['vps']).mean(0)
        print(f'INT8 shifted real-holdout ({len(pr)}): acc {100*np.mean(pr == real["vys"]):.2f}%  '
              f'pos MAE dx {mae[0]:.3f} dy {mae[1]:.3f} px')
    # sign sanity: shift a centred real/MNIST sample right / up and check the outputs move the right way
    base = real['vx0'][0] if real is not None else test_x[0]
    _, p0 = run([base])
    if real is not None:
        c = np.load(args.real[0])['images'][0]
        _, pr_ = run([shift_real(c, 12, 0)]); _, pu_ = run([shift_real(c, 0, -12)])
        _, pc_ = run([shift_real(c, 0, 0)])
        print(f'sign check (real crop shifted 3 px): centre {pc_[0]}, right {pr_[0]}, up {pu_[0]}')
        assert pr_[0][0] > pc_[0][0] and pu_[0][1] > pc_[0][1], 'position sign convention broken'
    print(f'INT8 sim {time.time()-t0:.0f}s')

    with open(os.path.join(args.out, 'mnist_tests.bin'), 'wb') as f, \
         open(os.path.join(args.out, 'mnist_tests_expect.bin'), 'wb') as fe:
        for i in range(args.ntest):
            xq = to_q(ts_x[i]) if i % 2 else to_q(test_x[i])      # alternate centred / shifted
            f.write(xq.reshape(-1).tobytes()); f.write(struct.pack('b', int(test_y[i])))
            fe.write(core_int8(xq).astype('<i4').tobytes())
    print(f'scales: s_o1={s_o1:.5f} s_o2={s_o2:.5f} s_wp={s_wp:.6f}  pos_k={pos_k} pos_sh={pos_sh}')
    print(f'wrote {args.out}/mnist_wblob.bin ({WBLOB_N} B), mnist_pblob.bin ({PBLOB_N*4} B), '
          f'mnist_tests.bin, mnist_tests_expect.bin')

if __name__ == '__main__':
    main()
