#!/usr/bin/env python3
# train_mnist_real.py — retrain + PTQ the mbdpu01 MNIST CNN on inverted-MNIST MIXED with the
# real ROI captures from the rotating-disc rig (captures_28x28.npz), then export new INT8 DDR
# blobs. Same network + quantization as train_mnist.py (must stay bit-exact to mbdpu01.cpp).
#
# Why mix: MNIST keeps all 10 classes (the disc only has 1-8, so training on real data ALONE
# would forget 0/9). The real captures teach the model the camera's actual appearance of the
# digits it currently misreads (6->5, 4->9). Real samples are oversampled so 1k real images
# carry weight against 60k MNIST; a real holdout split measures the real-world improvement.
#
# Run in the venv (torch CPU is fine), from this dir so 'data/' + blobs resolve:
#   ../../../venv/bin/python3 train_mnist_real.py --real ../../../captures_28x28.npz --out .
import argparse, struct, os, numpy as np

def q_multiplier(M):
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
    ap.add_argument('--epochs', type=int, default=4)
    ap.add_argument('--out', default='.')
    ap.add_argument('--data', default='data')
    ap.add_argument('--real', nargs='+', default=None,
                    help='one or more captures_28x28.npz (multiple sessions combined)')
    ap.add_argument('--oversample', type=int, default=40, help='repeat real train samples x this')
    ap.add_argument('--holdout', type=float, default=0.15, help='fraction of real held out for eval')
    ap.add_argument('--seed', type=int, default=0)
    ap.add_argument('--ntest', type=int, default=20)
    args = ap.parse_args()

    import torch, torch.nn as nn, torch.nn.functional as F
    from torchvision import datasets, transforms
    torch.manual_seed(args.seed); np.random.seed(args.seed)

    class Net(nn.Module):
        def __init__(s):
            super().__init__()
            s.c1 = nn.Conv2d(1, 8, 3, padding=1)
            s.c2 = nn.Conv2d(8, 16, 3, padding=1)
            s.fc = nn.Linear(7*7*16, 10)
        def forward(s, x):
            x = F.max_pool2d(F.relu(s.c1(x)), 2)
            x = F.max_pool2d(F.relu(s.c2(x)), 2)
            x = x.permute(0, 2, 3, 1).reshape(x.size(0), -1)   # [h][w][c] to match HLS FC
            return s.fc(x)

    dev = 'cuda' if torch.cuda.is_available() else 'cpu'
    tf = transforms.Compose([transforms.ToTensor(),
                             transforms.Lambda(lambda x: 1.0 - x)])   # inverted: white bg, dark digit
    tr = datasets.MNIST(args.data, train=True,  download=True, transform=tf)
    te = datasets.MNIST(args.data, train=False, download=True, transform=tf)

    # Build ONE tensor training set (uniform int64 labels -> no collate type clash). MNIST inverted
    # = 1 - data/255 (vectorized, == the ToTensor+invert transform).
    mnist_x = (1.0 - tr.data.float() / 255.0).unsqueeze(1)   # (60000,1,28,28) [0,1] inverted
    mnist_y = tr.targets.clone().long()

    # ---- real captures: uint8 [0,127] are ALREADY in the inverted representation -> /127 = [0,1] ----
    real_val = None
    if args.real:
        _im, _lb = [], []
        for rp in args.real:
            dd = np.load(rp)
            _im.append(dd['images'].astype(np.float32) / 127.0)  # (N,28,28) -> [0,1] inverted-style
            _lb.append(dd['labels'].astype(np.int64))
            print(f'  loaded {len(dd["labels"])} from {rp}')
        imgs = np.concatenate(_im); labs = np.concatenate(_lb)
        rtr_x, rtr_y, rvl_x, rvl_y = [], [], [], []
        for c in np.unique(labs):
            idx = np.where(labs == c)[0]; np.random.shuffle(idx)
            k = max(1, int(len(idx) * args.holdout))
            rvl_x.append(imgs[idx[:k]]); rvl_y.append(labs[idx[:k]])
            rtr_x.append(imgs[idx[k:]]); rtr_y.append(labs[idx[k:]])
        rtr_x = np.concatenate(rtr_x); rtr_y = np.concatenate(rtr_y)
        rvl_x = np.concatenate(rvl_x); rvl_y = np.concatenate(rvl_y)
        rx = torch.from_numpy(rtr_x).unsqueeze(1).repeat(args.oversample, 1, 1, 1)
        ry = torch.from_numpy(rtr_y).repeat(args.oversample).long()
        X = torch.cat([mnist_x, rx]); Y = torch.cat([mnist_y, ry])
        real_val = (rvl_x, rvl_y)
        print(f'real: {len(rtr_y)} train (x{args.oversample}={len(ry)}), {len(rvl_y)} holdout; '
              f'combined train {len(Y)}')
    else:
        X, Y = mnist_x, mnist_y

    trl = torch.utils.data.DataLoader(torch.utils.data.TensorDataset(X, Y),
                                      batch_size=128, shuffle=True)
    tel = torch.utils.data.DataLoader(te, batch_size=256)

    net = Net().to(dev)
    opt = torch.optim.Adam(net.parameters(), 1e-3)
    for ep in range(args.epochs):
        net.train()
        for x, y in trl:
            x, y = x.to(dev), y.to(dev)
            opt.zero_grad(); F.cross_entropy(net(x), y).backward(); opt.step()
        net.eval(); c = t = 0
        with torch.no_grad():
            for x, y in tel:
                c += (net(x.to(dev)).argmax(1).cpu() == y).sum().item(); t += y.size(0)
        msg = f'epoch {ep}: MNIST float acc {100*c/t:.2f}%'
        if real_val is not None:
            with torch.no_grad():
                rx = torch.from_numpy(real_val[0]).unsqueeze(1).to(dev)
                ra = (net(rx).argmax(1).cpu().numpy() == real_val[1]).mean()
            msg += f' | real-holdout float acc {100*ra:.2f}%'
        print(msg)

    net.cpu().eval()
    W1, B1 = net.c1.weight.detach().numpy(), net.c1.bias.detach().numpy()
    W2, B2 = net.c2.weight.detach().numpy(), net.c2.bias.detach().numpy()
    Wf, Bf = net.fc.weight.detach().numpy(), net.fc.bias.detach().numpy()

    s_in = 1.0 / 127.0
    sw = lambda w: np.abs(w).max() / 127.0
    s_w1, s_w2, s_wf = sw(W1), sw(W2), sw(Wf)
    xs = torch.stack([te[i][0] for i in range(1000)])
    with torch.no_grad():
        a1 = F.relu(net.c1(xs)); s_o1 = a1.abs().max().item() / 127.0
        a2 = F.relu(net.c2(F.max_pool2d(a1, 2))); s_o2 = a2.abs().max().item() / 127.0

    q = lambda w, s: np.clip(np.round(w / s), -127, 127).astype(np.int8)
    W1q, W2q, Wfq = q(W1, s_w1), q(W2, s_w2), q(Wf, s_wf)
    B1q = np.round(B1 / (s_in * s_w1)).astype(np.int32)
    B2q = np.round(B2 / (s_o1 * s_w2)).astype(np.int32)
    Bfq = np.round(Bf / (s_o2 * s_wf)).astype(np.int32)
    c1_M0, c1_sh = q_multiplier(s_in * s_w1 / s_o1)
    c2_M0, c2_sh = q_multiplier(s_o1 * s_w2 / s_o2)

    wblob = np.concatenate([W1q.reshape(-1), W2q.reshape(-1), Wfq.reshape(-1)]).astype(np.int8)
    pblob = np.concatenate([B1q, B2q, Bfq,
                            np.array([c1_M0, c1_sh, c2_M0, c2_sh], np.int32)]).astype(np.int32)
    assert wblob.size == 9064 and pblob.size == 38, (wblob.size, pblob.size)
    wblob.tofile(os.path.join(args.out, 'mnist_wblob.bin'))
    pblob.tofile(os.path.join(args.out, 'mnist_pblob.bin'))

    def infer_int8(img01):
        x = np.clip(np.round(img01 / s_in), 0, 127).astype(np.int32)
        a1 = np.zeros((28, 28, 8), np.int32); Xp = np.pad(x, 1)
        for oc in range(8):
            acc = sum(Xp[kh:kh+28, kw:kw+28]*int(W1q[oc,0,kh,kw]) for kh in range(3) for kw in range(3))
            a1[:,:,oc] = requant(acc, B1q[oc], c1_M0, c1_sh, True)
        p1 = a1.reshape(14,2,14,2,8).max(axis=(1,3))
        a2 = np.zeros((14,14,16), np.int32); Pp = np.pad(p1, ((1,1),(1,1),(0,0)))
        for oc in range(16):
            acc = np.zeros((14,14), np.int64)
            for ic in range(8):
                for kh in range(3):
                    for kw in range(3):
                        acc += Pp[kh:kh+14, kw:kw+14, ic]*int(W2q[oc,ic,kh,kw])
            a2[:,:,oc] = requant(acc, B2q[oc], c2_M0, c2_sh, True)
        p2 = a2.reshape(7,2,7,2,16).max(axis=(1,3))
        logits = Wfq.astype(np.int64) @ p2.reshape(-1).astype(np.int64) + Bfq
        return int(np.argmax(logits))

    c = t = 0
    for i in range(2000):
        img, y = te[i]; c += (infer_int8(img[0].numpy()) == y); t += 1
    print(f'INT8 (bit-exact to FPGA) MNIST acc over {t}: {100*c/t:.2f}%')
    if real_val is not None:
        rc = sum(infer_int8(real_val[0][i]) == real_val[1][i] for i in range(len(real_val[1])))
        print(f'INT8 real-holdout acc over {len(real_val[1])}: {100*rc/len(real_val[1]):.2f}%')
        # per-digit real accuracy (INT8), to see 6/4 improvement
        for d in sorted(set(real_val[1].tolist())):
            idx = np.where(real_val[1] == d)[0]
            acc = np.mean([infer_int8(real_val[0][i]) == d for i in idx])
            print(f'   real digit {d}: {100*acc:.1f}%  (n={len(idx)})')

    with open(os.path.join(args.out, 'mnist_tests.bin'), 'wb') as f:
        for i in range(args.ntest):
            img, y = te[i]
            xq = np.clip(np.round(img[0].numpy() / s_in), 0, 127).astype(np.int8)
            f.write(xq.reshape(-1).tobytes()); f.write(struct.pack('b', y))
    print('wrote mnist_wblob.bin (9064 B), mnist_pblob.bin (152 B), mnist_tests.bin')

if __name__ == '__main__':
    main()
