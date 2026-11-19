# Trained model — `mbdpu02` INT8 MNIST CNN + digit-position head

Binary runtime artifacts for the `mbdpu02` core (used by the `scu200_ai_demo02` target). They are
loaded into DDR at program time; the Vivado/HLS build does **not** read them (the HLS C-sim does,
to check the core bit-exactly against the trainer's INT8 model).

CNN: `conv1(1→8,3×3,pad1)→ReLU→pool2 → conv2(8→16,3×3,pad1)→ReLU→pool2 → FC(784→12)`.
FC rows 0–9 are the class logits (as `mbdpu01`); rows 10–11 are the **position head**:
where the digit's ink centre of mass sits in the 28×28 ROI, in model pixels, 0 = centred.
- `dx`: left −, right +
- `dy`: top +, bottom −

| File | Size | Format | Contents | Loaded to DDR |
|---|---|---|---|---|
| `mnist_wblob.bin` | 10632 B | INT8 | conv1 `8×1×3×3` + conv2 `16×8×3×3` + fc `12×784` | `0xB0000000` |
| `mnist_pblob.bin` | 168 B | INT32 ×42 | conv1[8]/conv2[16]/fc[10] biases, `{c1_M0,c1_sh,c2_M0,c2_sh}`, pos biases[2], `pos_k`, `pos_sh` | `0xB0010000` |
| `mnist_tests.bin` | 15700 B | 20 × (784 INT8 img + 1 label) | alternating centred / shifted MNIST test images | — |
| `mnist_tests_expect.bin` | 1040 B | 20 × INT32[13] | expected core output for each test image (bit-exact) | — |

Both blobs are **prefix-compatible** with `mbdpu01`'s: the head rows and params are appended.

Core output `out[13]`: `[0..9]` logits, `[10]` argmax, `[11]` dx accumulator, `[12]` dy
accumulator. Firmware converts: `px×100 = (acc × pos_k) >> pos_sh` (`pblob[40]`, `pblob[41]`).

## Provenance / regeneration
Produced by `../../Vivado/ip/mbdpu02/train_mnist_pos.py`. No capture records a digit position,
so labels are synthesised: every training image is randomly shifted (MNIST ±6 px in 28-px
space; real captures ±24 camera px on the 112×112 crop, then pooled to 28×28 exactly as `mbpre`
does) and labelled with the ink centre of mass of the shifted image.

Current model: MNIST + capture sessions take01/02/03 (`captures_112x112.npz`), 6 epochs,
λ = 0.5, ×40 shifted real variants. INT8 results:

| Metric | Value |
|---|---|
| real holdout, centred — accuracy | 100.0 % (456 samples, digits 1–8) |
| real holdout, shifted — accuracy / position MAE | 96.6 % / dx 0.30 px, dy 0.36 px |
| MNIST centred — accuracy | 90.7 % (the disc only carries 1–8; tune `--shift-frac`/`--oversample` if 0/9 matter) |
| MNIST shifted — accuracy / position MAE | 87.7 % / dx 0.42 px, dy 0.58 px |

Regenerate from the repo root:
```bash
venv/bin/python3 Vivado/ip/mbdpu02/train_mnist_pos.py --data data --out train/mbdpu02 \
    --real train/captures/mnist_take01/captures_112x112.npz \
           train/captures/mnist_take02/captures_112x112.npz \
           train/captures/mnist_take03/captures_112x112.npz
```
Then re-run the HLS C-sim (`make -C Vivado/ip ip`, or `vitis-run --mode hls --tcl
mbdpu02/run_hls.tcl` from `Vivado/ip`) to re-check the core against the new vectors.
