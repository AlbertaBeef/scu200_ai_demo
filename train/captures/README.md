# Capture datasets

Labeled real ROI captures of the rotating disc (digits **1–8**), used as training data for the
`mbdpu01` MNIST CNN. Each session is 1024 samples (8 angular slots × ~128 revolutions),
auto-labeled by angular position. `captures_28x28.npz` = 28×28 INT8 0–127 (the model input) and `captures_112x112.npz` = 112×112
uint8 0–255 (the raw hi-res crop) — row-for-row aligned. `manifest.csv` =
per-sample metadata (position, slot, prediction, brightness). Core-agnostic (reusable by any
MNIST-class core).

The GIFs below animate the hi-res crops (one 56×56 tile per digit, 1→8 left-to-right) across
successive captures (every 3rd revolution) at 10 fps.

## mnist_take01 — centered framing
![mnist_take01 crops](mnist_take01/captures_448x56_10fps.gif)

## mnist_take02 — tighter framing
![mnist_take02 crops](mnist_take02/captures_448x56_10fps.gif)
