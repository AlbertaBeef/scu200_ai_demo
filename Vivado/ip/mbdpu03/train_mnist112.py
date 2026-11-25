#!/usr/bin/env python3
# train_mnist112.py — train + PTQ the mbdpu03 CNN: a 112x112 mono MNIST classifier with the dx/dy
# position head (as mbdpu02), four conv stages deep. Exports the INT8 DDR blobs that the mbdpu03
# HLS core consumes (must stay bit-exact to mbdpu03.cpp).
# SPDX-License-Identifier: Apache-2.0
#
# Network (symmetric per-layer INT8, INT32 accumulate, TFLite-style requant as mbdpu01/02):
#   in 112x112x1 (0..127)
#   conv1 1->8   3x3 pad1, ReLU, requant, maxpool2 -> 56x56x8
#   conv2 8->16  3x3 pad1, ReLU, requant, maxpool2 -> 28x28x16
#   conv3 16->32 3x3 pad1, ReLU, requant, maxpool2 -> 14x14x32
#   conv4 32->32 3x3 pad1, ReLU, requant, maxpool2 ->  7x7x32
#   fc 1568->12 (raw INT32): rows 0..9 class logits, rows 10,11 dx, dy
#
# Position convention (112x112 input pixels, 0 = centred, centre = 55.5):
#   dx = cx - 55.5  (left -ve, right +ve)      dy = -(cy - 55.5)  (top +ve, bottom -ve)
# (cx, cy) = ink centre of mass of the (shifted) model input; labels clipped to +-POS_CLIP.
#   - MNIST: 28x28 inverted, upscaled 4x (bilinear) to 112, integer shifts +-SHIFT px, white pad.
#   - Real captures (captures_112x112.npz, raw gray 0..255): model input = gray >> 1 exactly as the
#     core's input stage does at blk=1; shifts +-SHIFT px padded with the crop's border median.
# Augmentation runs per batch in torch, so nothing 112x112-sized is materialised per epoch.
#
# Blobs:
#   mnist_wblob.bin  int8  33864: c1_w[8][1][3][3] c2_w[16][8][3][3] c3_w[32][16][3][3]
#                                 c4_w[32][32][3][3] fc_w[12][1568] (fc input flattened [h][w][c])
#   mnist_pblob.bin  int32 110:   c1_b[8] c2_b[16] c3_b[32] c4_b[32] fc_b[10]
#                                 {M0,sh} x4 (conv1..4), pos_b[2], pos_k, pos_sh
#     firmware: px*100 = (acc * pos_k) >> pos_sh  for acc = out[11] (dx), out[12] (dy), 112-px units
#   mnist_tests.bin         20 x (12544 int8 img + 1 int8 label)
#   mnist_tests_expect.bin  20 x int32[13] expected core output (bit-exact) for HLS csim / HW
#
# Run from the repo root with the venv:
#   venv/bin/python3 Vivado/ip/mbdpu03/train_mnist112.py --data data --out train/mbdpu03 \
#       --real train/captures/mnist_take0{1,2,3}/captures_112x112.npz
import argparse, struct, os, time, numpy as np
from numpy.lib.stride_tricks import sliding_window_view as swv

DIM      = 112
CENTRE   = (DIM - 1) / 2.0     # 55.5
SHIFT    = 24                  # +- px augmentation (= +-6 px in demo02's 28-px units)
POS_CLIP = 24.0                # label clip, px
CH       = [1, 8, 16, 32, 32]  # conv channel chain
FC_IN    = 7 * 7 * CH[-1]      # 1568
WBLOB_N  = sum(CH[i] * CH[i + 1] * 9 for i in range(4)) + 12 * FC_IN   # 33864
PBLOB_N  = sum(CH[1:]) + 10 + 8 + 2 + 2                                 # 110
OUT_N    = 13

def q_multiplier(M):
    """Real multiplier M in (0,1) -> (M0, shift), M ~= M0 * 2^-shift, M0 <= 16 bits (fits the
    core's ap_int<18> requant multiplier). Identical to mbdpu01/02."""
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

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--epochs', type=int, default=6)
    ap.add_argument('--out', default='.')
    ap.add_argument('--data', default='data', help='torchvision MNIST root')
    ap.add_argument('--real', nargs='+', default=None, help='one or more captures_112x112.npz')
    ap.add_argument('--oversample', type=int, default=40,
                    help='shifted variants per real train sample per epoch')
    ap.add_argument('--holdout', type=float, default=0.15)
    ap.add_argument('--lam', type=float, default=0.5, help='position loss weight')
    ap.add_argument('--pos-norm', type=float, default=4.0,
                    help='position loss/regression in px/pos_norm (4 = demo02 28-px units)')
    ap.add_argument('--shift-frac', type=float, default=1.0,
                    help='fraction of MNIST images shifted (rest stay centred)')
    ap.add_argument('--batch', type=int, default=128)
    ap.add_argument('--seed', type=int, default=0)
    ap.add_argument('--ntest', type=int, default=20)
    ap.add_argument('--limit', type=int, default=0, help='debug: use only N MNIST train images')
    args = ap.parse_args()

    import torch, torch.nn as nn, torch.nn.functional as F
    from torchvision import datasets
    torch.manual_seed(args.seed); rng = np.random.default_rng(args.seed)
    torch.set_num_threads(max(1, os.cpu_count() or 1))
    T = lambda a: torch.from_numpy(np.ascontiguousarray(a))
    PN = args.pos_norm

    # ---------------------------------------------------------------- augmentation (torch, batched)
    yy, xx = torch.meshgrid(torch.arange(DIM, dtype=torch.float32),
                            torch.arange(DIM, dtype=torch.float32), indexing='ij')

    def shift_batch(x, pad, sx, sy):
        """x (B,112,112) float, pad (B,) fill value, integer shifts sx,sy (B,) (content moves
        right/down for +ve). Returns (B,112,112)."""
        B = x.shape[0]; P = SHIFT
        xp = pad.view(B, 1, 1).expand(B, DIM + 2 * P, DIM + 2 * P).clone()
        xp[:, P:P + DIM, P:P + DIM] = x
        r = (torch.arange(DIM)[None, :] + (P - sy)[:, None])        # (B,112)
        c = (torch.arange(DIM)[None, :] + (P - sx)[:, None])
        return xp[torch.arange(B)[:, None, None], r[:, :, None], c[:, None, :]]

    def ink_com(x):
        """x (B,112,112) float [0,1] (light bg, dark ink) -> (B,2) [dx,dy] px, clipped."""
        ring = torch.cat([x[:, 0, :], x[:, -1, :], x[:, 1:-1, 0], x[:, 1:-1, -1]], 1)
        bg = ring.median(1).values.view(-1, 1, 1)
        w = torch.clamp(0.75 * bg - x, min=0.0)
        s = w.sum((1, 2)); ok = s > 1e-6; sd = torch.clamp(s, min=1e-6)
        cx = torch.where(ok, (w * xx).sum((1, 2)) / sd, torch.full_like(s, CENTRE))
        cy = torch.where(ok, (w * yy).sum((1, 2)) / sd, torch.full_like(s, CENTRE))
        return torch.clamp(torch.stack([cx - CENTRE, -(cy - CENTRE)], 1), -POS_CLIP, POS_CLIP)

    def up4(x28):
        """(B,28,28) inverted [0,1] -> (B,112,112) bilinear."""
        return F.interpolate(x28.unsqueeze(1), scale_factor=4, mode='bilinear',
                             align_corners=False).squeeze(1).clamp(0, 1)

    def quant_in(x01):
        """[0,1] -> the core's 0..127 integer grid, back to float (train on what the HW sees)."""
        return torch.round(x01 * 127.0).clamp(0, 127) / 127.0

    def rand_shift(B, g, frac=1.0):
        sx = torch.randint(-SHIFT, SHIFT + 1, (B,), generator=g)
        sy = torch.randint(-SHIFT, SHIFT + 1, (B,), generator=g)
        if frac < 1.0:
            keep = torch.rand(B, generator=g) >= frac
            sx[keep] = 0; sy[keep] = 0
        return sx, sy

    def mnist_batch(x28, g, frac=1.0):
        x = up4(x28); B = x.shape[0]
        sx, sy = rand_shift(B, g, frac)
        x = quant_in(shift_batch(x, torch.ones(B), sx, sy))
        return x, ink_com(x)

    def real_batch(c112, bgm, g, shifted=True):
        """c112 uint8 (B,112,112) raw gray; model input (g>>1)/127; pad = border median."""
        x = (c112.to(torch.int32) >> 1).to(torch.float32) / 127.0; B = x.shape[0]
        if shifted:
            sx, sy = rand_shift(B, g)
            x = shift_batch(x, bgm, sx, sy)
        return x, ink_com(x)

    # ---------------------------------------------------------------- data
    tr = datasets.MNIST(args.data, train=True,  download=True)
    te = datasets.MNIST(args.data, train=False, download=True)
    mnist_x = 1.0 - tr.data.float() / 255.0                   # inverted [0,1], (60000,28,28)
    mnist_y = tr.targets.long()
    if args.limit: mnist_x, mnist_y = mnist_x[:args.limit], mnist_y[:args.limit]
    test_x  = 1.0 - te.data.float() / 255.0
    test_y  = te.targets.long()
    gte = torch.Generator().manual_seed(args.seed + 1)
    tc_x = quant_in(up4(test_x[:2000]))                        # centred MNIST test (112)
    ts_x, ts_p = mnist_batch(test_x[:2000], gte)               # shifted MNIST test (112)

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
        half = (cr.astype(np.int32) >> 1).astype(np.float32) / 127.0
        ring = np.concatenate([half[:, 0], half[:, -1], half[:, 1:-1, 0], half[:, 1:-1, -1]], 1)
        bgm = T(np.median(ring, 1).astype(np.float32))
        crT, lbT = T(cr), T(lb)
        vx0, _ = real_batch(crT[vli], bgm[vli], gte, shifted=False)
        rep = np.repeat(vli, 4)                                  # 4 shifted variants per holdout
        vxs, vps = real_batch(crT[rep], bgm[rep], gte)
        real = dict(tri=T(tri), crT=crT, lbT=lbT, bgm=bgm, vx0=vx0, vy0=lbT[vli],
                    vxs=vxs, vys=lbT[rep], vps=vps)
        print(f'real: {len(tri)} train (x{args.oversample} shifted per epoch), {len(vli)} holdout')

    # ---------------------------------------------------------------- model
    class Net(nn.Module):
        def __init__(s):
            super().__init__()
            s.convs = nn.ModuleList([nn.Conv2d(CH[i], CH[i + 1], 3, padding=1) for i in range(4)])
            s.fc = nn.Linear(FC_IN, 12)
        def acts(s, x):
            """pre-pool ReLU activations of each conv (for calibration)."""
            out = []
            for c in s.convs:
                a = F.relu(c(x)); out.append(a); x = F.max_pool2d(a, 2)
            return out, x
        def forward(s, x):
            _, x = s.acts(x)
            x = x.permute(0, 2, 3, 1).reshape(x.size(0), -1)      # [h][w][c] to match the HLS FC
            o = s.fc(x)
            return o[:, :10], o[:, 10:] * PN                     # position head in px

    net = Net()
    opt = torch.optim.Adam(net.parameters(), 1e-3)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, args.epochs)

    def batched(fn, x, bs=500):
        return [fn(x[i:i + bs]) for i in range(0, len(x), bs)]

    def predict(x):
        lg, pp = zip(*batched(lambda b: net(b.unsqueeze(1)), x))
        return torch.cat(lg).argmax(1), torch.cat(pp)

    def float_eval():
        net.eval()
        with torch.no_grad():
            pr, _ = predict(tc_x); acc = (pr == test_y[:2000]).float().mean()
            _, pp = predict(ts_x); mae = (pp - ts_p).abs().mean(0)
            msg = f'MNIST acc {100*acc:.2f}% | shifted pos MAE dx {mae[0]:.2f} dy {mae[1]:.2f} px'
            if real is not None:
                pr, _ = predict(real['vx0']); ra = (pr == real['vy0']).float().mean()
                pr, pp = predict(real['vxs']); rs = (pr == real['vys']).float().mean()
                rm = (pp - real['vps']).abs().mean(0)
                msg += (f' | real acc {100*ra:.2f}% shifted {100*rs:.2f}% '
                        f'pos MAE dx {rm[0]:.2f} dy {rm[1]:.2f} px')
        return msg

    g = torch.Generator().manual_seed(args.seed + 2)
    n_m = len(mnist_x); n_r = len(real['tri']) * args.oversample if real is not None else 0
    for ep in range(args.epochs):
        t0 = time.time(); net.train()
        order = torch.randperm(n_m + n_r, generator=g)
        for i in range(0, len(order), args.batch):
            idx = order[i:i + args.batch]
            mi = idx[idx < n_m]; ri = idx[idx >= n_m] - n_m
            xs, ys, ps = [], [], []
            if len(mi):
                x, p = mnist_batch(mnist_x[mi], g, args.shift_frac)
                xs.append(x); ys.append(mnist_y[mi]); ps.append(p)
            if len(ri):
                k = real['tri'][ri % len(real['tri'])]
                x, p = real_batch(real['crT'][k], real['bgm'][k], g)
                xs.append(x); ys.append(real['lbT'][k]); ps.append(p)
            x = torch.cat(xs).unsqueeze(1); y = torch.cat(ys); p = torch.cat(ps)
            opt.zero_grad()
            lg, pp = net(x)
            loss = F.cross_entropy(lg, y) + args.lam * F.smooth_l1_loss(pp / PN, p / PN)
            loss.backward(); opt.step()
        sched.step()
        print(f'epoch {ep}: {float_eval()}  ({time.time()-t0:.0f}s)', flush=True)

    # ---------------------------------------------------------------- PTQ
    net.eval()
    Ws = [c.weight.detach().numpy() for c in net.convs]
    Bs = [c.bias.detach().numpy() for c in net.convs]
    Wf, Bf = net.fc.weight.detach().numpy().copy(), net.fc.bias.detach().numpy().copy()
    Wf[10:] *= PN; Bf[10:] *= PN                                 # fold pos_norm into the head
    Wc, Bc, Wp, Bp = Wf[:10], Bf[:10], Wf[10:], Bf[10:]

    s_in = 1.0 / 127.0
    sw = lambda w: np.abs(w).max() / 127.0
    s_w = [sw(w) for w in Ws]; s_wc, s_wp = sw(Wc), sw(Wp)
    cal = [tc_x[:300], ts_x[:300]]
    if real is not None: cal += [real['vx0'], real['vxs'][:300]]
    with torch.no_grad():
        acts, _ = net.acts(torch.cat(cal).unsqueeze(1))
    s_o = [a.abs().max().item() / 127.0 for a in acts]

    q = lambda w, s: np.clip(np.round(w / s), -127, 127).astype(np.int8)
    Wq = [q(w, s) for w, s in zip(Ws, s_w)]
    s_prev = [s_in] + s_o[:3]
    Bq = [np.round(b / (sp * s)).astype(np.int32) for b, sp, s in zip(Bs, s_prev, s_w)]
    Mq = [q_multiplier(sp * s / so) for sp, s, so in zip(s_prev, s_w, s_o)]
    Wcq, Wpq = q(Wc, s_wc), q(Wp, s_wp)
    Bcq = np.round(Bc / (s_o[3] * s_wc)).astype(np.int32)
    Bpq = np.round(Bp / (s_o[3] * s_wp)).astype(np.int32)
    pos_scale = s_o[3] * s_wp * 100.0                            # px*100 per accumulator LSB
    pos_sh = 24
    while pos_scale * (1 << pos_sh) >= (1 << 22) and pos_sh > 8: pos_sh -= 1
    while pos_scale * (1 << pos_sh) <  (1 << 15) and pos_sh < 40: pos_sh += 1
    pos_k = int(round(pos_scale * (1 << pos_sh)))
    assert (1 << 14) <= pos_k < (1 << 23), pos_k

    wblob = np.concatenate([w.reshape(-1) for w in Wq] + [Wcq.reshape(-1), Wpq.reshape(-1)]).astype(np.int8)
    pblob = np.concatenate(Bq + [Bcq, np.array([v for m in Mq for v in m], np.int32),
                                 Bpq, np.array([pos_k, pos_sh], np.int32)]).astype(np.int32)
    assert wblob.size == WBLOB_N and pblob.size == PBLOB_N, (wblob.size, pblob.size)
    os.makedirs(args.out, exist_ok=True)
    wblob.tofile(os.path.join(args.out, 'mnist_wblob.bin'))
    pblob.tofile(os.path.join(args.out, 'mnist_pblob.bin'))

    Wfq = np.concatenate([Wcq, Wpq]).astype(np.int64)        # [12][1568] as the core sees it
    Bfq = np.concatenate([Bcq, Bpq]).astype(np.int64)
    Wq64 = [w.astype(np.int64) for w in Wq]

    def core_int8(xq):
        """Bit-exact model of mbdpu03: int8 image (112,112) 0..127 -> int32 out[13]."""
        a = xq.astype(np.int64)[:, :, None]                      # (H,W,C) layout
        for l in range(4):
            H = a.shape[0]
            win = swv(np.pad(a, ((1, 1), (1, 1), (0, 0))), (3, 3), axis=(0, 1))   # (H,W,C,3,3)
            acc = np.einsum('hwcij,ocij->hwo', win, Wq64[l])
            assert np.abs(acc).max() < (1 << 24)
            r = requant(acc, Bq[l][None, None, :].astype(np.int64), Mq[l][0], Mq[l][1], True)
            a = r.reshape(H // 2, 2, H // 2, 2, -1).max(axis=(1, 3)).astype(np.int64)
        o = Wfq @ a.reshape(-1) + Bfq
        out = np.zeros(OUT_N, np.int64)
        out[:10] = o[:10]; out[10] = int(np.argmax(o[:10])); out[11:13] = o[10:12]
        assert np.all(np.abs(out) < 2**31)
        return out.astype(np.int32)

    to_q = lambda x01: np.clip(np.round(np.asarray(x01) * 127.0), 0, 127).astype(np.int8)
    to_px = lambda acc: ((int(acc) * pos_k) >> pos_sh) / 100.0     # firmware conversion

    def run(xs):
        o = np.stack([core_int8(to_q(x)) for x in xs])
        pos = np.array([[to_px(a), to_px(b)] for a, b in o[:, 11:13]], np.float32)
        return o[:, 10], pos

    t0 = time.time()
    ty = test_y.numpy()
    pr, _ = run(tc_x[:1000]); print(f'INT8 MNIST acc (centred, 1000): {100*np.mean(pr == ty[:1000]):.2f}%')
    pr, pp = run(ts_x[:1000])
    mae = np.abs(pp - ts_p[:1000].numpy()).mean(0)
    print(f'INT8 shifted-MNIST (1000): acc {100*np.mean(pr == ty[:1000]):.2f}%  '
          f'pos MAE dx {mae[0]:.3f} dy {mae[1]:.3f} px')
    if real is not None:
        vy0 = real['vy0'].numpy()
        pr, _ = run(real['vx0'])
        print(f'INT8 real-holdout acc (centred, {len(pr)}): {100*np.mean(pr == vy0):.2f}%')
        for d in sorted(set(vy0.tolist())):
            m = vy0 == d
            print(f'   real digit {d}: {100*np.mean(pr[m] == d):.1f}%  (n={m.sum()})')
        pr, pp = run(real['vxs'])
        mae = np.abs(pp - real['vps'].numpy()).mean(0)
        print(f'INT8 shifted real-holdout ({len(pr)}): acc {100*np.mean(pr == real["vys"].numpy()):.2f}%  '
              f'pos MAE dx {mae[0]:.3f} dy {mae[1]:.3f} px')
        # sign sanity: shift one crop right / up by 12 px and check the outputs move the right way
        c = real['crT'][:1]; b = real['bgm'][:1]
        sh = lambda sx, sy: shift_batch((c.to(torch.int32) >> 1).float() / 127.0, b,
                                        torch.tensor([sx]), torch.tensor([sy]))[0]
        _, pc_ = run([sh(0, 0)]); _, pr_ = run([sh(12, 0)]); _, pu_ = run([sh(0, -12)])
        print(f'sign check (real crop shifted 12 px): centre {pc_[0]}, right {pr_[0]}, up {pu_[0]}')
        assert pr_[0][0] > pc_[0][0] and pu_[0][1] > pc_[0][1], 'position sign convention broken'
    print(f'INT8 sim {time.time()-t0:.0f}s')

    with open(os.path.join(args.out, 'mnist_tests.bin'), 'wb') as f, \
         open(os.path.join(args.out, 'mnist_tests_expect.bin'), 'wb') as fe:
        for i in range(args.ntest):
            xq = to_q(ts_x[i]) if i % 2 else to_q(tc_x[i])          # alternate centred / shifted
            f.write(xq.reshape(-1).tobytes()); f.write(struct.pack('b', int(ty[i])))
            fe.write(core_int8(xq).astype('<i4').tobytes())
    print('scales: s_o=' + ' '.join(f'{s:.5f}' for s in s_o) +
          f' s_wp={s_wp:.6f}  M0/sh={Mq}  pos_k={pos_k} pos_sh={pos_sh}')
    print(f'wrote {args.out}/mnist_wblob.bin ({WBLOB_N} B), mnist_pblob.bin ({PBLOB_N*4} B), '
          f'mnist_tests.bin, mnist_tests_expect.bin')

if __name__ == '__main__':
    main()
