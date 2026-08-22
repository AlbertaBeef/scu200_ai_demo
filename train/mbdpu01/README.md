# Trained model — `mbdpu01` INT8 MNIST CNN

These are **binary runtime artifacts** (the deployed trained model + verification vectors), not
source. They are loaded into DDR at program time and streamed by the weight-agnostic `mbdpu01`
core on every inference; the Vivado/HLS build does **not** read them.

CNN: `conv1(1→8,3×3,pad1)→ReLU→pool2 → conv2(8→16,3×3,pad1)→ReLU→pool2 → FC(784→10)`.

| File | Size | Format | Contents | Loaded to DDR |
|---|---|---|---|---|
| `mnist_wblob.bin` | 9064 B | INT8 | weights: conv1 `8×1×3×3` + conv2 `16×8×3×3` + fc `10×784` | `0xB0000000` |
| `mnist_pblob.bin` | 152 B | INT32 ×38 | biases conv1[8]/conv2[16]/fc[10] + requant `{c1_M0,c1_sh,c2_M0,c2_sh}` | `0xB0010000` |
| `mnist_tests.bin` | 15700 B | 20 × (784 INT8 img + 1 label) | verification samples for C-sim / on-hardware spot-checks | — |

## Provenance / regeneration
Produced by the training + post-training-quantization scripts in
`../../Vivado/ip/mbdpu01/`:

- `train_mnist.py` — baseline: trains on inverted MNIST, quantizes to INT8 (exact scheme in
  `mbdpu01.cpp`), writes all three blobs.
- `train_mnist_real.py` — mixes captured rotating-disc ROIs (`captures_*/captures_28x28.npz`) with MNIST
  for domain match; same quantization + blob export. Supports multiple `--real` sessions.

The committed blobs are the **combined-retrain** model (MNIST + two capture sessions;
INT8 MNIST ≈ 96.2 %, real-holdout ≈ 99.7 %). To regenerate into this directory, run from
`Vivado/ip/mbdpu01/` (needs a Python venv with torch + torchvision), e.g.:

```bash
python3 train_mnist_real.py \
    --real ../../../train/captures/mnist_take01/captures_28x28.npz ../../../train/captures/mnist_take02/captures_28x28.npz \
    --out  ../../../train/mbdpu01
```

Training data (the `--real` npz files) lives in `../captures/mnist_take*/` — see the layout in the
top-level `README.md`. Those capture datasets are core-agnostic (28×28 digit images), so they are
shared across cores; only the produced blobs here are specific to `mbdpu01`.
