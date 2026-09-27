# 07 — Reproducing

## On the host (no console needed)

Requirements: Python 3.9+, `numpy`, `pillow`.

```bash
pip install numpy pillow
```

### Generate the pipelines

```bash
python examples/generator/gen_upscale_pipes.py
# Generating upscaler .pipe files...
#   wrote upscale_easu.pipe (1 sampler(s), R8G8B8A8_UNORM)
#   ...
#   UL: 7 layers x 3 textures, 1x1 conv over layers [2, 3, 4, 5, 6]
# OK
```

This writes 58 `.pipe` files plus `upscale_wide_pipes.inc` (the runtime's
table rows for UL) into `examples/generated/`. They are byte-identical to the
ones EVO Player ships.

Compiling them to gfx1013 ISA needs AMD's `amdllpc` and LLVM 18's `readelf` /
`objcopy`; see [04 — Shader toolchain](04-shader-toolchain.md). In EVO Player:

```bash
docker compose -f docker-compose.yml -f docker-compose.amdllpc.yml run --rm ps5-dev \
    bash -lc 'python3 tools/gen_upscale_pipes.py && python3 tools/build_agc_pipes.py'
```

### Run the reference maths

```bash
python examples/generator/upscale_ref.py --selftest
python examples/generator/upscale_ref.py frame.png --size 3840x2160 --out out/
```

- **Input.** The frame should be the RGB picture at **source** resolution,
  which is what the GPU's L0 surface holds after the YUV pass.
- **Output.** `bilinear.png`, `sharp.png`, `ai-s.png` and `ai-m.png` at the
  requested size, for diffing against a console capture.

### Rebuild every comparison image

```bash
python tools/build_comparisons.py              # all sets
python tools/build_comparisons.py bbb-540p-web # one set
```

The script reads only `comparisons/<set>/full/*.png`, the lossless captures. It
finds the most detailed 640×360 window in the Off frame (highest edge energy)
and writes `crop_*.png`, `zoom_*.png` (3× nearest-neighbour, one per mode),
the `strip.png` / `zoom.png` / `grid.png` sheets, and `metrics.md`.

## On a console

You need:
- a jailbroken PS5,
- [EVO Player](https://github.com/sainsaji/EVO-PLAYER-PS5) built with the dev
  remote: `scripts/package-app.sh --ffpfsc --usb-remote`, then
  `scripts/deploy-app.sh --ffpfsc`,
- a USB drive at `/mnt/usb0`, and the console's FTP server reachable.

Then:

1. **Play a clip remotely.** From Git Bash on Windows, prefix console paths
   with `//` so MSYS doesn't rewrite them:
   ```bash
   ./tools/evo-remote.sh play "//mnt/usb0/media/Demos/bbb_1080p_h264.mp4"
   ./tools/evo-remote.sh seek 4
   ```
2. **Capture the same frame in every mode:**
   ```bash
   ./tools/evo-remote.sh upcompare
   #   upcompare: off -> /mnt/usb0/evo_up_off.bmp (active="Off" pts=6333333)
   #   upcompare: sharp -> ... (active="Sharp" ...)
   #   upcompare: ai -> ... (active="AI (Standard)" ...)
   #   upcompare: ai_large -> ... (active="AI (Large)" ...)
   #   upcompare: ai_max -> ... (active="AI (Maximum)" ...)
   ```
   The captures land in EVO's `output/upcompare/*.bmp`. The `active=` label is
   what the runtime actually ran, so a bypass or fallback shows up here.
3. **Import them here as lossless PNG and build the images:**
   ```bash
   python tools/build_comparisons.py --import-bmp /path/to/output/upcompare my-set
   python tools/build_comparisons.py my-set
   ```
   Add a `comparisons/my-set/set.json` with a `title` to label the metrics.
4. **Measure GPU cost.** Pick the mode in Settings, play for a few seconds and
   pull the log (`./tools/evo-remote.sh log`):
   ```
   agc upscale us=1753 n=120 mode=AI (Large) budget_us=12000 (frame GPU submit->retire, 100 us grain)
   ```

### Making test clips

These are the 720p and 540p "web encodes" used here:

```bash
for r in 720 540; do
  ffmpeg -stream_loop 2 -i bbb_1080p_h264.mp4 -vf "scale=-2:${r}:flags=bicubic" \
    -c:v libx264 -profile:v high -pix_fmt yuv420p -preset slow -crf 24 \
    -c:a aac -b:a 128k -movflags +faststart bbb_${r}p_web_h264.mp4
done
```
