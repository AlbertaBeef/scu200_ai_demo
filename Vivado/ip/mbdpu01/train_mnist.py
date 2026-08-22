#!/usr/bin/env python3
# train_mnist.py — train + post-training-quantize the mbdpu01 MNIST CNN, and export
# DDR blobs that the mbdpu01 HLS core consumes at runtime.
#
# The network MUST match mbdpu01.cpp exactly:
#   conv1 1->8 3x3 pad1, ReLU, maxpool2  ->  conv2 8->16 3x3 pad1, ReLU, maxpool2  ->  fc 784->10
# Quantization matches the HLS requant:  y = clamp( ((acc + bias)*M0 + 2^(sh-1)) >> sh )
# with per-layer symmetric INT8 (zero-point 0), ReLU folded into the clamp ([0,127]).
#
# Run where PyTorch is available (not on the FPGA host):
#   python3 train_mnist.py --epochs 3 --out .
# Produces:
#   mnist_wblob.bin  (int8,  9064 elems: conv1_w[8][1][3][3], conv2_w[16][8][3][3], fc_w[10][784])
#   mnist_pblob.bin  (int32, 38 elems:   c1_b[8], c2_b[16], fc_b[10], c1_M0,c1_sh, c2_M0,c2_sh)
#   mnist_tests.bin  (int8,  N * (784 image + 1 label), for on-hardware spot checks)
#   report: simulated INT8 accuracy (this is what the FPGA will get, bit-exact)
import argparse, struct, numpy as np

def q_multiplier(M):
    """Represent real multiplier M in (0,1) as (M0, shift) with M ~= M0 * 2^-shift.
    M0 is kept to a 16-bit mantissa so it fits the core's ap_int<18> requant multiplier
    (the 27x18 single-DSP48 multiply in mbdpu01.cpp). A Q31 M0 would be truncated by the
    hardware to 18 bits -> garbage requant -> constant output."""
    if M <= 0: return 0, 0
    shift = 0
    while M < 0.5:
        M *= 2.0; shift += 1
    M0 = int(round(M * (1 << 16)))     # mantissa in [2^15, 2^16] -> <=17 bits (+sign fits ap_int<18>)
    shift += 16
    if M0 == (1 << 16):                # clamp to keep <=16-bit magnitude
        M0 >>= 1; shift -= 1
    return M0, shift

def requant(acc, bias, M0, sh, relu):
    x = (acc.astype(np.int64) + bias) * np.int64(M0)
    r = (x + (1 << (sh - 1))) >> sh
    if relu: r = np.maximum(r, 0)
    return np.clip(r, -127, 127).astype(np.int32)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--epochs', type=int, default=3)
    ap.add_argument('--out', default='.')
    ap.add_argument('--ntest', type=int, default=20)
    args = ap.parse_args()

    import torch, torch.nn as nn, torch.nn.functional as F
    from torchvision import datasets, transforms

    class Net(nn.Module):
        def __init__(s):
            super().__init__()
            s.c1 = nn.Conv2d(1, 8, 3, padding=1)
            s.c2 = nn.Conv2d(8, 16, 3, padding=1)
            s.fc = nn.Linear(7*7*16, 10)
        def forward(s, x):
            x = F.max_pool2d(F.relu(s.c1(x)), 2)      # 28->14
            x = F.max_pool2d(F.relu(s.c2(x)), 2)      # 14->7
            # flatten in [h][w][c] order to match the HLS FC accumulation
            x = x.permute(0, 2, 3, 1).reshape(x.size(0), -1)
            return s.fc(x)

    dev = 'cuda' if torch.cuda.is_available() else 'cpu'
    # INVERTED dataset: black digit on WHITE background, so the raw camera view
    # (dark digit on white paper) feeds the model directly with NO runtime invert.
    tf = transforms.Compose([transforms.ToTensor(),
                             transforms.Lambda(lambda x: 1.0 - x)])   # pixels -> [0,1], inverted
    tr = datasets.MNIST('data', train=True,  download=True, transform=tf)
    te = datasets.MNIST('data', train=False, download=True, transform=tf)
    trl = torch.utils.data.DataLoader(tr, batch_size=128, shuffle=True)
    tel = torch.utils.data.DataLoader(te, batch_size=256)

    net = Net().to(dev)
    opt = torch.optim.Adam(net.parameters(), 1e-3)
    for ep in range(args.epochs):
        net.train()
        for x, y in trl:
            x, y = x.to(dev), y.to(dev)
            opt.zero_grad(); F.cross_entropy(net(x), y).backward(); opt.step()
        # float eval
        net.eval(); c = t = 0
        with torch.no_grad():
            for x, y in tel:
                c += (net(x.to(dev)).argmax(1).cpu() == y).sum().item(); t += y.size(0)
        print(f'epoch {ep}: float acc {100*c/t:.2f}%')

    net.cpu().eval()
    W1 = net.c1.weight.detach().numpy(); B1 = net.c1.bias.detach().numpy()
    W2 = net.c2.weight.detach().numpy(); B2 = net.c2.bias.detach().numpy()
    Wf = net.fc.weight.detach().numpy(); Bf = net.fc.bias.detach().numpy()

    # ---- calibrate activation scales on a subset (symmetric, max-abs) ----
    xs = torch.stack([te[i][0] for i in range(1000)])
    s_in = 1.0 / 127.0                                    # input: [0,1] -> int8 [0,127]
    def sw(w): return np.abs(w).max() / 127.0
    s_w1, s_w2, s_wf = sw(W1), sw(W2), sw(Wf)
    with torch.no_grad():
        a1 = F.relu(net.c1(xs)); s_o1 = a1.abs().max().item() / 127.0
        p1 = F.max_pool2d(a1, 2)
        a2 = F.relu(net.c2(p1)); s_o2 = a2.abs().max().item() / 127.0

    # ---- quantize weights/biases ----
    q = lambda w, s: np.clip(np.round(w / s), -127, 127).astype(np.int8)
    W1q, W2q, Wfq = q(W1, s_w1), q(W2, s_w2), q(Wf, s_wf)
    B1q = np.round(B1 / (s_in * s_w1)).astype(np.int32)   # bias in accumulator scale
    B2q = np.round(B2 / (s_o1 * s_w2)).astype(np.int32)
    Bfq = np.round(Bf / (s_o2 * s_wf)).astype(np.int32)
    c1_M0, c1_sh = q_multiplier(s_in * s_w1 / s_o1)
    c2_M0, c2_sh = q_multiplier(s_o1 * s_w2 / s_o2)

    # ---- write blobs (layout MUST match mbdpu01.cpp offsets) ----
    import os
    wblob = np.concatenate([W1q.reshape(-1), W2q.reshape(-1), Wfq.reshape(-1)]).astype(np.int8)
    pblob = np.concatenate([B1q, B2q, Bfq,
                            np.array([c1_M0, c1_sh, c2_M0, c2_sh], np.int32)]).astype(np.int32)
    assert wblob.size == 9064 and pblob.size == 38, (wblob.size, pblob.size)
    wblob.tofile(os.path.join(args.out, 'mnist_wblob.bin'))
    pblob.tofile(os.path.join(args.out, 'mnist_pblob.bin'))

    # ---- simulate the exact INT8 forward pass (== what the FPGA computes) ----
    def infer_int8(img01):
        x = np.clip(np.round(img01 / s_in), 0, 127).astype(np.int32)     # [28,28]
        # conv1
        a1 = np.zeros((28, 28, 8), np.int32)
        Xp = np.pad(x, 1)
        for oc in range(8):
            acc = sum(Xp[kh:kh+28, kw:kw+28] * int(W1q[oc,0,kh,kw]) for kh in range(3) for kw in range(3))
            a1[:,:,oc] = requant(acc, B1q[oc], c1_M0, c1_sh, True)
        p1 = a1.reshape(14,2,14,2,8).max(axis=(1,3))                     # maxpool
        # conv2
        a2 = np.zeros((14,14,16), np.int32); Pp = np.pad(p1, ((1,1),(1,1),(0,0)))
        for oc in range(16):
            acc = np.zeros((14,14), np.int64)
            for ic in range(8):
                for kh in range(3):
                    for kw in range(3):
                        acc += Pp[kh:kh+14, kw:kw+14, ic] * int(W2q[oc,ic,kh,kw])
            a2[:,:,oc] = requant(acc, B2q[oc], c2_M0, c2_sh, True)
        p2 = a2.reshape(7,2,7,2,16).max(axis=(1,3))                     # [7,7,16]
        flat = p2.reshape(-1)                                            # [h][w][c]
        logits = Wfq.astype(np.int64) @ flat.astype(np.int64) + Bfq
        return int(np.argmax(logits))

    # accuracy of the quantized model over the test set
    c = t = 0
    for i in range(2000):
        img, y = te[i]; c += (infer_int8(img[0].numpy()) == y); t += 1
    print(f'INT8 (bit-exact to FPGA) acc over {t}: {100*c/t:.2f}%')

    # ---- export a handful of test images for on-hardware spot checks ----
    with open(os.path.join(args.out, 'mnist_tests.bin'), 'wb') as f:
        for i in range(args.ntest):
            img, y = te[i]
            xq = np.clip(np.round(img[0].numpy() / s_in), 0, 127).astype(np.int8)
            f.write(xq.reshape(-1).tobytes()); f.write(struct.pack('b', y))
    print(f'wrote mnist_wblob.bin (9064 B), mnist_pblob.bin (152 B), mnist_tests.bin ({args.ntest} imgs)')

if __name__ == '__main__':
    main()
