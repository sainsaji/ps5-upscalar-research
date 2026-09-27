# 06 — Lessons from hardware

What the console taught us that the compiler and the host tools couldn't.

## 1. The first frame was garbage

The first build compiled cleanly, submitted cleanly, logged no errors and ran
at 1.1 ms a frame. And the picture looked like this: horizontal smears with
vertical seams at regular intervals, with only the broad light and dark bands
in roughly the right place.

The boot log held the answer:

```
agc color_target ... attrib=0x4dc6c000
```

- **The register.** `CB_COLOR0_ATTRIB3` bits 14–18 are `COLOR_SW_MODE`:
  `(0x4dc6c000 >> 14) & 0x1f = 27`, i.e. `SW_64KB_R_X`, a 64 KB tiled layout.
- **The mismatch.** Every colour target, the scratch surfaces included, is
  written **tiled**, while the upscaler's texture descriptors described that
  memory as **linear**. Each 64 KB tile was read as 128×128 pixels laid out in
  rows, hence the seams.
- **Why nothing complained.** The GPU does exactly what the descriptor says. A
  layout mismatch is never a fault, only a wrong picture.

**Fix:** a descriptor with `SW_MODE = 27` and the target's exact size
([04 — Shader toolchain](04-shader-toolchain.md#tiled-render-targets-sampling-what-the-gpu-drew)),
and 36 MB slots because the tiled footprint of a 4K surface exceeds 32 MB.

**Lesson:** when you sample something the GPU rendered, read the target's
swizzle mode from the actual register value, not from a comment. EVO's own
source had comments claiming its targets were linear.

## 2. Barriers are part of the algorithm

- **The failure mode.** Ping-ponging two feature maps means a surface is read,
  rewritten and read again within one command buffer. Without a
  texture-cache invalidate between passes, the third access can hit
  stale lines from the first.
- **The fix.** EVO's `RELEASE_MEM` with GCR control `0xC` carries
  `GLV_INV | GL1_INV` (in the RELEASE_MEM encoding: bit 2 GLV, bit 3 GL1) on
  top of the colour-block flush.
- **How it was verified.** The UL network runs 36 dependent passes per frame,
  and its output is stable and matches the expected structure.

## 3. Timing

`sceAgcCbReleaseMem` can write a GPU timestamp, but its data-select encoding
isn't documented, and a wrong guess can hang the GPU. Instead:

- **A timer that already exists.** EVO's frame loop blocks on each frame's
  fence right after submitting it, so the submit → retire wait is the frame's
  GPU time.
- **The grain problem.** The wait polled every 1 ms, so anything faster read
  as "≤ ~1.1 ms".
- **The fix.** Upscaled frames now poll every 0.1 ms, which is how AI Large
  measured at **1.75 ms**.
- **Limits.** It is whole-frame time (UI included) and averaged over 120
  frames, which is enough for a budget check but not a per-pass profile.

## 4. The capture has to be deterministic

Two screenshots taken by hand are never the same frame, and on a paused frame
a mode change doesn't cause a redraw: the render loop only redraws a buffer
that doesn't already hold the current frame's timestamp. The `upcompare`
command:

- forces a redraw every frame while it runs,
- hides the UI,
- waits for both scanout buffers to carry the new mode before copying.

One subtle bug along the way: the host script waited for the log line of the
*last* capture, and matched the previous run's line, so it pulled images
before the new run finished. It now ignores log lines written before it sent
the command.

## 5. Off must stay identical

The upscaler adds code to the one function that draws every video frame.
Four rules kept Off byte-for-byte unchanged:

- The plan is computed first, and emits nothing.
- Off takes exactly the old code path.
- The switch to the scratch surface is the last thing before the draw.
- Every exit path restores the scanout target, so a failure mid-chain can't
  leave the UI drawing into scratch memory.

## 6. The network matters less than the source

On clean 1080p, all three Anime4K networks land within about 1/255 of each
other on average. On a 540p web encode, the gap between *any* network and
bilinear is dramatic, while the gap between networks stays small. Bigger
networks mostly clean up compression artefacts, so their value shows on
low-bitrate anime, not on clean CGI or live action.

## 7. Add a control before trusting a negative

The Trinity queries all failed through `sceKernelDlsym` with ESRCH, and that
looked like "libkernel hides them from apps". It wasn't. A lookup of
`sceKernelUsleep`, a function the app calls constantly, failed identically:
`sceKernelDlsym` resolves nothing from a fake-signed app module on FW 12.70.
One control call turned a wrong conclusion into the right one.

The practical answer was a manual AI NETWORK setting, protected by the budget
fallback. The query that matters, `sceKernelIsTrinityMode`, can't be
imported directly either: the loader rejects the app at launch. See
[05 — PS5 Pro detection](05-ps5-pro-detection.md).
