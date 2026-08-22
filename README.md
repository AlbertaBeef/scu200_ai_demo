# SCU200 AI Demo — `scu200_ai_demo01`

A rotating-disc handwritten-digit (MNIST) recognition demo on the **AMD/Xilinx SCU200**
(Spartan UltraScale+ `xcsu200p-sbva1024-2-e-es1`, ES1), MicroBlaze bare-metal, Vivado + Vitis
2025.2.

A DC gearmotor spins a disc printed with digits; a fabric **encoder-decode + angle strobe**
(`disc_strobe`) fires the camera (OmniVision OV9281 global-shutter) at fixed disc angles so the
image freezes; a hardware preprocessor (`mbpre`) crops/downscales the ROI to 28×28; and a custom
INT8 CNN (`mbdpu01`) classifies the digit. Results + telemetry are drawn over HDMI, and captured
ROIs can be recorded to retrain the classifier.

> **Build model:** this repo is populated progressively. Each stage below is meant to be built and
> validated independently. **Stage 1 (FPGA hardware / Vivado) is present now**; later stages
> (Vitis app, programming, capture/training) will be added.

---

## Prerequisites

- **Vivado 2025.2** and **Vitis 2025.2** installed; environment sourced so `vivado` and
  `vitis-run` are on `PATH` and `$XILINX_VIVADO` is set:
  ```bash
  source /path/to/Xilinx/2025.2/Vivado/settings64.sh
  source /path/to/Xilinx/2025.2/Vitis/settings64.sh
  ```
- **Beta device enabled.** `xcsu200p…-es1` is a 2025.2 beta device. The HLS build calls
  `enable_beta_device` and `get_part.tcl` hard-codes the part (the SCU200 has no board file), but
  your Vivado install must have the beta device family enabled (check the startup line
  `NN Beta devices … enabled`). If needed, add to `~/.Xilinx/Vivado/Vivado_init.tcl`:
  `enable_beta_device xcsu200*`.
- `make`, and network access on the first IP build if the board store needs refreshing (not
  required for SCU200 — the part is hard-coded).

---

## Repository layout (current)

```
scu200_ai_demo/
├── README.md                      ← this file (build instructions)
├── .gitignore
├── train/                         all training material
│   ├── captures/                  labeled ROI datasets (core-agnostic, shared across cores)
│   │   ├── mnist_take01/          session 1 (centered framing)
│   │   └── mnist_take02/          session 2 (tighter framing)
│   │       each session holds (row-for-row aligned):
│   │         captures_28x28.npz    28×28 INT8 0–127   (the model input)
│   │         captures_112x112.npz  112×112 uint8 0–255 (raw hi-res crop)
│   │         manifest.csv          per-sample metadata (+ a preview GIF)
│   └── mbdpu01/                    model OUTPUT for the mbdpu01 CNN (runtime blobs → DDR at program)
│       ├── README.md              what the blobs are + how to regenerate
│       ├── mnist_wblob.bin        weights (INT8) → DDR 0xB0000000
│       ├── mnist_pblob.bin        biases + requant params (INT32) → DDR 0xB0010000
│       └── mnist_tests.bin        20 verification samples (C-sim / HW spot-check)
└── Vivado/
    ├── Makefile                   top-level Vivado build (target: scu200_ai_demo01)
    ├── scripts/
    │   ├── build.tcl              create project + block design
    │   └── xsa.tcl               synth / implement / export XSA+PDI
    ├── ip/
    │   ├── Makefile               HLS IP build (disc_strobe, mbpre, mbdpu01)
    │   ├── get_part.tcl           resolves the SCU200 part -> build/<board>/settings.tcl
    │   ├── disc_strobe/           encoder quadrature decode + angle-triggered FSIN strobe
    │   ├── mbpre/                 HW ROI crop/downscale to 28×28
    │   └── mbdpu01/              INT8 MNIST CNN — HLS source + training scripts
    └── src/
        ├── bd/
        │   ├── bd_scu200_ai_demo01.tcl   the block design
        │   └── mipi_locs.tcl             MIPI pin LOCs
        └── constraints/
            └── scu200_ai_demo01.xdc      pin constraints (camera, HDMI, motor/encoder, XVS)
```

The block design instantiates three custom HLS cores — **`disc_strobe`, `mbpre`, `mbdpu01`** —
plus the stock camera/HDMI/DDR pipeline. The trained model blobs live under **`train/<core>/`**
(e.g. `train/mbdpu01/`) — runtime data, not read by the Vivado build, loaded to DDR by the
programming step, and regenerable from the training scripts in `Vivado/ip/mbdpu01/` (they are
specific to each core's topology + quantization, hence the per-core sub-dir). See
`train/mbdpu01/README.md`.

---

## Stage 1 — Build the FPGA hardware (Vivado)

All commands run from the `Vivado/` directory. The default `TARGET` is `scu200_ai_demo01`.

### 1a. Build the HLS IP cores
```bash
cd Vivado
make ip TARGET=scu200_ai_demo01
```
This runs `get_part.tcl` (writes `ip/build/scu200/settings.tcl` with the SCU200 part), then
HLS C-synth + IP export for `disc_strobe`, `mbpre`, `mbdpu01`.
**Expected:** `ip/build/scu200/{disc_strobe,mbpre,mbdpu01}.prj/sol1/impl/ip/*.zip` and
`ip/build/scu200/ip_done.txt`.

### 1b. Build the bitstream (project → synth → impl → XSA/PDI)
```bash
make xsa TARGET=scu200_ai_demo01 JOBS=8
```
(`make xsa` will trigger `make ip` first if the IP isn't built yet, so 1a is optional.)

This creates the Vivado project `scu200_ai_demo01/`, builds the block design from
`bd_scu200_ai_demo01.tcl`, applies `scu200_ai_demo01.xdc`, then synthesizes, implements, and
exports.

**Expected outputs:**
- `Vivado/scu200_ai_demo01/scu200_ai_demo01.runs/impl_1/scu200_ai_demo01_wrapper.pdi` — the programming image
- `Vivado/scu200_ai_demo01/scu200_ai_demo01.runs/impl_1/scu200_ai_demo01_wrapper.bit`
- `Vivado/scu200_ai_demo01/scu200_ai_demo01_wrapper.xsa` — hardware handoff (consumed by the Vitis stage later)

**Expected result:** timing **met** (positive WNS/WHS). The build gate (`check_warnings`)
tolerates exactly two benign critical warnings on this design and fails on any other:
- `Place 30-894` — SCU200 ES1 DDR-clock delay matching in the tight X0Y3 region (timing still met).
- `Vivado 12-1790` — HDMI evaluation-IP license notice.

### Notes
- `phys_opt_design` is deliberately disabled for the SCU200 (`board_name == scu200`) in
  `build.tcl` — it perturbs the ES1 clock placement and trips `Place 30-837`. Skipping it keeps
  `place_design`'s valid clock placement.
- Clean a build: `make clean TARGET=scu200_ai_demo01` (removes the project + thOKis target's IP).

---

## Known issues

- **Triggered-capture drift (motor encoder).** The angle strobe is driven by the motor's
  quadrature encoder, which sits *before* the 20.4:1 gearbox — so a fixed encoder count is not an
  exact disc angle (the gearbox ratio is non-integer, plus backlash). The `disc_strobe` core uses
  a Q16 fractional phase accumulator and `cpr` is calibrated to **979.6** counts/disc-revolution
  (live-trimmable over UART with the `C`/`V` keys). This reduces the creep to near-zero over short
  runs, but it is **not perfect** — a small residual remains and can accumulate over long runs.
  A guaranteed zero-drift fix needs a disc-axis (post-gearbox) once-per-revolution index sensor to
  re-anchor the angle each turn.

- **URAM not used / not available.** The target `xcsu200p-sbva1024-2-e-es1` is an **ES1** device on
  which Vitis HLS rejects URAM binding (`bind_storage impl=uram`) — no `enable_beta_device` or init
  flag unlocks it. All on-chip storage in the custom cores (`mbdpu01`, `mbpre`, `disc_strobe`) is
  therefore mapped to **BRAM/LUTRAM**, not URAM. On production silicon URAM would be available; the
  HLS sources would need re-binding to take advantage of it.

---

## Coming next (later stages)
- **Vitis application** (`Vitis/common/src`, MicroBlaze bare-metal) + build scripts.
- **Programming / run scripts** (`program.tcl`, weight/gallery JTAG scripts).
- **Capture & retrain** tooling (ROI recording → `roi_export.py` → `train_mnist_real.py`) and the
  UART control cheat-sheet.
