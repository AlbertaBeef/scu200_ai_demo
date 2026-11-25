# Trained model — `mbdpu03` 112×112 INT8 MNIST CNN + digit-position head

Binary runtime artifacts for the `mbdpu03` core (used by the `scu200_ai_demo03` target). They are
loaded into DDR at program time; the core copies them into its on-chip stores (URAM/BRAM) on
the first inference and whenever the param blob changes. The Vivado/HLS build does **not** read
them (the HLS C-sim does, to check the core bit-exactly against the trainer's INT8 model).

CNN, monochrome 112×112 input (0..127, inverted: ink bright):
```
conv1 1→8   3×3 pad1 → ReLU → pool2 → 56×56×8
conv2 8→16  3×3 pad1 → ReLU → pool2 → 28×28×16
conv3 16→32 3×3 pad1 → ReLU → pool2 → 14×14×32
conv4 32→32 3×3 pad1 → ReLU → pool2 →  7×7×32
FC 1568 → 12
```
FC rows 0–9 are the class logits; rows 10–11 are the **position head**: where the digit's ink
centre of mass sits in the 112×112 ROI, in **112-px units** (4× finer than mbdpu02's 28-px units),
0 = centred.
- `dx`: left −, right +
- `dy`: top +, bottom −

| File | Size | Format | Contents | Loaded to DDR |
|---|---|---|---|---|
| `mnist_wblob.bin` | 33864 B | INT8 | conv1 `8×1×3×3` + conv2 `16×8×3×3` + conv3 `32×16×3×3` + conv4 `32×32×3×3` + fc `12×1568` (fc input order h,w,c) | `0xB0000000` |
| `mnist_pblob.bin` | 440 B | INT32 ×110 | conv1[8]/conv2[16]/conv3[32]/conv4[32]/fc[10] biases, `{M0,sh}×4`, pos biases[2], `pos_k`, `pos_sh` | `0xB0010000` |
| `mnist_tests.bin` | 250900 B | 20 × (12544 INT8 img + 1 label) | alternating centred / shifted test images | — |
| `mnist_tests_expect.bin` | 1040 B | 20 × INT32[13] | expected core output for each test image (bit-exact) | — |

Core output `out[13]`: `[0..9]` logits, `[10]` argmax, `[11]` dx accumulator, `[12]` dy
accumulator. Firmware converts: `px×100 = (acc × pos_k) >> pos_sh` (`pblob[108]`, `pblob[109]`).

## Provenance / regeneration
Produced by `../../Vivado/ip/mbdpu03/train_mnist112.py`. MNIST digits are upscaled ×4 (bilinear)
to 112×112; real captures use their native 112×112 crops. Position labels are synthesised as in
mbdpu02: every training image is randomly shifted (±24 px, white/border-median padding) and
labelled with the ink centre of mass of the shifted image. The head is trained in px/4 and the
scale is folded back into the INT8 weights at quantisation.

Current model: MNIST + capture sessions take01/02/03 (`captures_112x112.npz`), 6 epochs,
×40 shifted real variants. INT8 results:

| Metric | Value |
|---|---|
| real holdout, centred — accuracy | 100.0 % (456 samples, digits 1–8) |
| real holdout, shifted — accuracy / position MAE | 99.45 % / dx 0.77 px, dy 0.76 px (112-px units; mbdpu02: 96.6 %, 1.2 / 1.4 px equivalent) |
| MNIST centred — accuracy | 96.1 % |
| MNIST shifted — accuracy / position MAE | 96.2 % / dx 1.22 px, dy 1.70 px |

Regenerate from the repo root (CPU, ~40 min on 32 cores):
```bash
venv/bin/python3 Vivado/ip/mbdpu03/train_mnist112.py --data data --out train/mbdpu03 \
    --real train/captures/mnist_take01/captures_112x112.npz \
           train/captures/mnist_take02/captures_112x112.npz \
           train/captures/mnist_take03/captures_112x112.npz
```
Then re-run the HLS C-sim (`make -C Vivado/ip ip`, or `vitis-run --mode hls --tcl
mbdpu03/run_hls.tcl` from `Vivado/ip`) to re-check the core against the new vectors.
