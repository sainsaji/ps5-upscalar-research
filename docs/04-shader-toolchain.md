# 04 — Shader toolchain

PS5 homebrew has no shader compiler. EVO compiles GLSL offline with AMD's
open-source **LLPC** (`amdllpc`, the compiler inside AMDVLK) targeting
**gfx1013**, the PS5 GPU. It derives the hardware registers `sceAgc` needs from
the compiler's metadata.

```
gen_upscale_pipes.py ──► *.pipe ──amdllpc -gfxip=10.1.3──► PAL ELF
                                                            │
             llvm-objcopy (.text) ◄─────────────────────────┤
             llvm-readelf --notes (AMDGPU PAL metadata YAML) ┘
                                   │
             build_agc_pipes.py ──►  <name>_pipe.h : ISA blobs + register tables
                                   │
             evo_agc_runtime.c ──►  sceAgcCreateShader / sceAgcLinkShaders, bind + draw
```

A compiled example is
[`examples/pipes/upscale_easu_pipe.h`](../examples/pipes/upscale_easu_pipe.h).

## The `.pipe` format

A `.pipe` file is LLPC's text format: both shader stages, the resource layout
and the fixed-function state in one file.

```ini
[VsGlsl]
#version 450
... vertex shader ...
[VsInfo]
entryPoint = main

[FsGlsl]
... fragment shader ...
[FsInfo]
entryPoint = main

[ResourceMapping]
userDataNode[0].visibility = 2                        ; vertex stage
userDataNode[0].type = DescriptorTableVaPtr
userDataNode[0].offsetInDwords = 0
userDataNode[0].next[0].type = DescriptorConstBuffer  ; PassConstants (uUv)
userDataNode[0].next[0].set = 0
userDataNode[0].next[0].binding = 0
userDataNode[1].visibility = 64                       ; fragment stage
userDataNode[1].type = DescriptorTableVaPtr
userDataNode[1].offsetInDwords = 0
userDataNode[1].next[0].type = DescriptorCombinedTexture   ; T# + S#, 12 dwords
userDataNode[1].next[0].offsetInDwords = 0
userDataNode[1].next[0].set = 1
userDataNode[1].next[0].binding = 0

[GraphicsPipelineState]
topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
nggState.enableNgg = 1
colorBuffer[0].format = VK_FORMAT_R16G16B16A16_SFLOAT     ; or R8G8B8A8_UNORM
colorBuffer[0].channelWriteMask = 15
```

**Every upscale pass uses this exact mapping.** It is the one EVO's video
pipelines already proved on hardware:
- one vertex-stage table holding a constant buffer,
- one fragment-stage table holding 1–4 combined textures.

No pass needs fragment-stage constants, because shaders read sizes with
`textureSize`. A new shader therefore can't fail on a new, unverified binding
layout.

## User-data slots come from the compiler, not from guesses

- **What the runtime must supply.** When drawing, the runtime writes the
  *addresses* of those two descriptor tables into user SGPRs. LLPC decides
  which SGPR holds which; it reports that in the PAL metadata as
  `.user_data_reg_map`.
- **How the entries read.** Small values in the map are a node's
  `offsetInDwords`; `0x1000xxxx` values are driver-supplied (global table,
  base vertex, and so on).
- **What the build does with it.** `build_agc_pipes.py` turns the map into
  `VS_CONST_TABLE_DWORD` / `PS_TEXTURE_TABLE_DWORD` in the generated header,
  and the runtime indexes by those.

Why this matters: an SGPR written to the wrong slot is **silent**. The shader
reads its descriptors through an unset pointer and draws nothing, with no
fault. Every upscale pipe reports the same slots:

```
upscale_a4k_ul_conv3: gs=256B ps=8872B ... user_dwords(const_vs=1,...,tex=1) write(vs=2,ps=2)
```

## Registers derived from PAL metadata

The PAL metadata also carries everything needed for the context and shader
registers. The build script derives:

- `SPI_SHADER_PGM_RSRC1/2` (VGPR/SGPR counts),
- `VGT_SHADER_STAGES_EN`, `GE_CNTL`, `SPI_PS_INPUT_*`, `SPI_SHADER_COL_FORMAT`,
  `DB_SHADER_CONTROL`,
- the NGG sub-group sizes.

They go into fixed-size arrays that `sceAgcCreateShader` accepts. Nothing is
hand-tuned per shader, which is what let 58 new pipelines work on hardware
without per-shader debugging.

Worth knowing: for both an RGBA8 and an RGBA16F target, LLPC picks the
**FP16_ABGR** export format (`SPI_SHADER_COL_FORMAT = 4`). The colour block
converts on write, so a float target needs no shader change, only a different
colour-target setup.

## RGBA16F colour targets and textures

EVO's colour-target builder produces an RGBA8 target. For the feature maps it
patches `CB_COLOR0_INFO`:

| Field | RGBA8 | RGBA16F |
|---|---|---|
| FORMAT | `COLOR_8_8_8_8` (10) | `COLOR_16_16_16_16` (12) |
| NUMBER_TYPE | UNORM (0) | FLOAT (7) |
| ROUND_MODE | 0 | 1 (as Mesa sets for float) |
| BLEND_CLAMP | 1 | 0 |

Sampling them back uses GFX10 image format **71** (`16_16_16_16_FLOAT`) in
the texture descriptor (T#).

## Tiled render targets: sampling what the GPU drew

Every colour target in EVO is rendered **tiled**:
`CB_COLOR0_ATTRIB3.COLOR_SW_MODE = 27`, i.e. `SW_64KB_R_X`
(`attrib=0x4dc6c000` in the boot log). A texture descriptor that describes the
same memory as *linear* reads it as scrambled 64 KB blocks. That was the
upscaler's first hardware result; see
[06 — Lessons](06-lessons-from-hardware.md#1-the-first-frame-was-garbage).

The fix is a descriptor that matches the target:

```c
int evo_agc_build_tsharp_render_target(uint32_t out[8], uint64_t gpu_address,
                                       uint32_t width, uint32_t height, int fp16)
{
    const uint32_t bpp = fp16 ? 8u : 4u;
    if ((gpu_address & 0xffffu) != 0u)          /* 64 KB-aligned base */
        return -1;
    int rc = build_tsharp_2d_internal(out, gpu_address, width, height, width * bpp,
                                      fp16 ? GFX10_FORMAT_16_16_16_16_FLOAT
                                           : GFX10_FORMAT_8_8_8_8_UNORM, bpp,
                                      SQ_SEL_X, SQ_SEL_Y, SQ_SEL_Z, SQ_SEL_W);
    if (rc == 0)
        out[3] |= (uint32_t)SQ_SW_64KB_R_X << 20;   /* SQ_IMG_RSRC_WORD3.SW_MODE */
    return rc;
}
```

- **Size.** The width and height must be the colour target's own. The tiled
  layout is derived from them, so no pitch field is used.
- **Footprint.** In 64 KB blocks it is `ceil(w/128) × ceil(h/128)` at 4 bytes
  per pixel, or `ceil(w/128) × ceil(h/64)` at 8 bytes.
