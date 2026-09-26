# Examples

| Path | What it is | Runs on |
|---|---|---|
| [`generator/gen_upscale_pipes.py`](generator/gen_upscale_pipes.py) | Translates Anime4K's mpv hooks and a GLSL port of FSR 1 into one `.pipe` per GPU pass (58 total) | any host, Python only |
| [`generator/upscale_ref.py`](generator/upscale_ref.py) | numpy reference of EASU, RCAS, Anime4K S/M and depth-to-space; `--selftest` | any host |
| [`toolchain/build_agc_pipes.py`](toolchain/build_agc_pipes.py) | `.pipe` → amdllpc (gfx1013) → PAL metadata → C header with ISA + registers. Paths assume EVO Player's layout | amdllpc + LLVM 18 |
| [`pipes/`](pipes/) | Representative generated passes (FSR, S/M conv, M/UL accumulate, both depth-to-space variants) and one compiled header | read |
| [`runtime/agc_upscale_excerpt.c`](runtime/agc_upscale_excerpt.c) | The upscale stage from EVO's bare-metal sceAgc runtime, verbatim: plan, scratch surfaces, pass encoder, S/M/UL chains, budget fallback, tiled T# | read (needs EVO's runtime to build) |
| [`runtime/evo_hw.c`](runtime/evo_hw.c) | PS5 Pro (Trinity) probe via `sceKernelDlsym` | PS5 app module |
| [`capture/upcompare_run.py`](capture/upcompare_run.py) | Drives EVO's `upcompare` over FTP and pulls same-frame captures | host + console |

Start with [`../docs/03-networks.md`](../docs/03-networks.md) for a walk-through.
