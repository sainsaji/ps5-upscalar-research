# 03 — The networks

Everything here is produced by one script,
[`examples/generator/gen_upscale_pipes.py`](../examples/generator/gen_upscale_pipes.py),
which writes a `.pipe` file per GPU pass. Each pass draws a fullscreen quad.

## The shared vertex stage

Every upscale pass uses the same vertex shader. It builds the quad from
`gl_VertexIndex` (no vertex buffer) and maps it onto a sub-rectangle of the
source:

```glsl
layout(set = 0, binding = 0, std140) uniform PassConstants {
    vec4 uUv;       /* xy = source UV origin, zw = source UV extent */
} pass;
layout(location = 0) out vec2 vUV;

void main() {
    vec2 p = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vUV = pass.uUv.xy + vec2(p.x, 1.0 - p.y) * pass.uUv.zw;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
```

The runtime sets the viewport to the target size. A fragment then reads texel
`ivec2(vUV * textureSize(...))`, the texel under it at the pass's own
resolution. Nothing depends on `gl_FragCoord`, because no shader in this
toolchain had been verified on hardware with it.

## Sharp: an FSR 1 port

FSR 1 is two passes.

**EASU** (edge-adaptive spatial upsampling) runs at output resolution:

- It reads a 12-texel neighbourhood around each output pixel's source position
  (`b c / e f g h / i j k l / n o`).
- It estimates a local edge direction and strength from the luma of the four
  bilinear corners.
- It filters with a Lanczos-2 approximation stretched along that edge.
- It clamps the result to the 2×2 neighbourhood to avoid ringing.

**RCAS** (robust contrast-adaptive sharpening) is a 5-tap cross:

- It sharpens by the largest amount that cannot push any channel outside the
  local minimum and maximum.
- It backs off on isolated single-pixel detail (noise).
- Sharpness is FSR's default 0.2 stops.

The port follows `ffx_fsr1.h`'s float path, with two changes for this
platform:

- **Direct fetches instead of gathers.** Each of the 12 taps is a clamped
  `texelFetch`. `textureGather` component ordering is easy to get wrong, and a
  wrong order produces an image that looks almost right.
- **Guarded reciprocals.** FSR's approximate `rcp` becomes `1 / max(x, 1/65536)`
  everywhere a flat region would otherwise divide by zero and turn NaN.

## AI: translating Anime4K

[Anime4K](https://github.com/bloc97/Anime4K) ships each network as an **mpv
user-shader**: a text file of `//!HOOK` passes whose weights are literal
`mat4` constants. Here is part of the second conv layer of network S:

```glsl
//!DESC Anime4K-v3.2-Upscale-CNN-x2-(S)-Conv-4x3x3x8
//!HOOK MAIN
//!BIND conv2d_tf
//!SAVE conv2d_1_tf
#define go_0(x_off, y_off) (max((conv2d_tf_texOff(vec2(x_off, y_off))), 0.0))
#define go_1(x_off, y_off) (max(-(conv2d_tf_texOff(vec2(x_off, y_off))), 0.0))
vec4 hook() {
    vec4 result = mat4(-0.011029496, 0.05866063, ...) * go_0(-1.0, -1.0);
    result += mat4(0.16062798, -0.10190268, ...) * go_0(-1.0, 0.0);
    ...
    result += vec4(0.012077211, 0.013045883, 0.0380778, -0.02908858);
    return result;
}
```

- **Layer shape.** A layer is a 3×3 convolution from 8 inputs to 4 outputs.
- **CReLU.** The 8 inputs come from *CReLU*: the previous layer's 4 channels,
  split into `max(x, 0)` and `max(-x, 0)`.
- **Storage.** Every layer's output is one RGBA texture, 4 channels.

The generator does a **textual translation that keeps the weights and the
arithmetic byte-for-byte**:

- `NAME_texOff(vec2(x, y))` (mpv's clamped fetch at a texel offset) becomes a
  macro over `texelFetch` with the same edge clamp. Row order carries over,
  because mpv's y axis and the texture memory both run top-down.
- `hook()` is called from a `main()` that sets up the texel coordinates.

Result (from [`examples/pipes/upscale_a4k_s_conv1.pipe`](../examples/pipes/upscale_a4k_s_conv1.pipe)):

```glsl
layout(set = 1, binding = 0) uniform sampler2D uIn0;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 out_color;

ivec2 g_ip;
ivec2 g_sz;
#define T0(x, y) texelFetch(uIn0, clamp(g_ip + ivec2(int(x), int(y)), ivec2(0), g_sz - 1), 0)
#define go_0(x_off, y_off) (max((T0(x_off, y_off)), 0.0))
#define go_1(x_off, y_off) (max(-(T0(x_off, y_off)), 0.0))
vec4 hook() {
    vec4 result = mat4(-0.011029496, 0.05866063, ...) * go_0(-1.0, -1.0);
    ...
}

void main() {
    g_sz = textureSize(uIn0, 0);
    g_ip = clamp(ivec2(vUV * vec2(g_sz)), ivec2(0), g_sz - 1);
    out_color = hook();
}
```

The feature maps are stored as **RGBA16F**. They are signed and unbounded, so
any UNORM format would clip them.

### The four networks

| Network | Shape | Final layer | GPU passes (incl. YUV and final) |
|---|---|---|---|
| **S** | 4 × (3×3 conv, 4 ch) | the 4th conv *is* the output | 6 |
| **M** | 7 × (3×3 conv, 4 ch) | 1×1 conv over **all 7** layers (56 inputs) | 16 |
| **UL** | 7 × (3×3 conv, 12 ch = 3 textures) | three 1×1 convs over layers 2–6 (120 inputs) | 38 |

L and VL are vendored in `third_party/anime4k/` too. They have UL's wide shape
(8 channels = 2 textures), so the same generator path handles them. They aren't
wired into EVO's settings.

## Splitting the dense final layer

M's last layer reads **every** earlier layer:

```glsl
//!BIND conv2d_tf  //!BIND conv2d_1_tf  ...  //!BIND conv2d_6_tf
#define g_0 (max((conv2d_tf_tex(conv2d_tf_pos)), 0.0))
#define g_1 (max(-(conv2d_tf_tex(conv2d_tf_pos)), 0.0))
...
vec4 hook() {
    vec4 result = mat4(...) * g_0;
    result += mat4(...) * g_1;
    ...                                   /* 14 terms */
    result += vec4(bias);
    return result;
}
```

Run literally, it needs all seven feature maps alive at once. But the layer is
**linear in its inputs**:

```
out = b + Σ_k ( W⁺_k · max(f_k, 0) + W⁻_k · max(−f_k, 0) )
```

So it can be computed as a running sum, with one accumulate pass after each
conv layer:

```
acc_0 = b       + W⁺_0 · max(f_0, 0) + W⁻_0 · max(−f_0, 0)
acc_k = acc_k−1 + W⁺_k · max(f_k, 0) + W⁻_k · max(−f_k, 0)
out   = acc_6
```

A generated accumulate pass (`upscale_a4k_m_acc1.pipe`):

```glsl
/* Anime4K-v3.2-Upscale-CNN-x2-(M)-Conv-4x1x1x56 - layer 1 of 7 */
void main() {
    ivec2 sz = textureSize(uIn0, 0);
    ivec2 ip = clamp(ivec2(vUV * vec2(sz)), ivec2(0), sz - 1);
    vec4 f = texelFetch(uIn0, ip, 0);                 /* feature map of layer 1 */
    vec4 r = mat4(-0.07579662, ...) * max(f, 0.0);    /* W+ of layer 1 */
    r += mat4(0.15458213, ...) * max(-f, 0.0);        /* W- of layer 1 */
    r += texelFetch(uIn1, ip, 0);                     /* running sum */
    out_color = r;
}
```

- **Memory.** Only two feature maps and two accumulators are ever alive (ping-
  pong), instead of seven.
- **Exactness.** In exact arithmetic the split is identical to the original.
  `upscale_ref.py --selftest` checks it: maximum difference `0.00e+00` in
  float64. On the GPU each partial sum is rounded to fp16 once, which is the
  only difference.

## UL: wide layers

- **Width.** Each UL layer is 12 channels, stored as three RGBA16F textures.
- **One pass per output texture.** Each reads all three input textures (24
  inputs after CReLU), so a layer is three passes. The generator's
  `conv_pass_fs` handles any number of bound textures (`T0`..`Tn`).
- **Three 1×1 convolutions at the end**, over layers 2–6. Each produces one
  texture: the residual for **R**, **G** and **B**. Split per layer and per
  output, that is 5 × 3 = 15 accumulate passes.
- **Term grouping.** The generator parses the `g_N` macro definitions to learn
  which bound texture and which sign each term uses, then groups terms by
  layer. It asserts every layer gets exactly `2 × width` terms.

The 4-sampler resource mapping of one UL accumulate pass
(`upscale_a4k_ul_acc3.pipe`): three feature textures plus the running sum.

```
userDataNode[1].next[0..3].type = DescriptorCombinedTexture   ; 12 dwords each, set 1, bindings 0..3
```

## Depth-to-space

Anime4K networks output a **2× residual packed into channels**: texel (x, y)
of the last layer holds the four sub-pixels of the 2× grid, with
index = `(y & 1) * 2 + (x & 1)`. mpv's last pass unpacks that at 2× and adds it
to a bilinear sample of the source. mpv then scales the 2× image to the output
with its own scaler.

EVO needs arbitrary scale factors (2×, 3×, 4× and Fit rectangles), so its
final pass does both steps at once, directly at output resolution:

```glsl
float R(ivec2 v) {                      /* one sub-pixel of the virtual 2x grid */
    v = clamp(v, ivec2(0), g_sz * 2 - 1);
    vec4 t = texelFetch(uIn1, v >> 1, 0);
    return t[(v.y & 1) * 2 + (v.x & 1)];
}

void main() {
    g_sz = textureSize(uIn1, 0);
    vec3 base = texture(uIn0, vUV).rgb;             /* bilinear source */
    vec2 q = vUV * vec2(g_sz * 2) - vec2(0.5);      /* position on the 2x grid */
    vec2 f = fract(q);
    ivec2 i0 = ivec2(floor(q));
    float r = mix(mix(R(i0),               R(i0 + ivec2(1, 0)), f.x),
                  mix(R(i0 + ivec2(0, 1)), R(i0 + ivec2(1, 1)), f.x), f.y);
    out_color = vec4(clamp(base + vec3(r), 0.0, 1.0), 1.0);
}
```

- **The residual** is bilinearly resampled from the virtual 2× grid.
- **The base** is a bilinear sample of the source.
- **S and M** add one luma residual to all three channels.
- **UL** (`upscale_a4k_rgb_final.pipe`) reads three residual textures and adds
  a separate value per channel. That colour-aware correction is the visible
  signature of Maximum in the [results](01-results.md#metrics).

## Checking the maths on the host

[`upscale_ref.py`](../examples/generator/upscale_ref.py) reimplements EASU,
RCAS, Anime4K S and M, and the depth-to-space in numpy. It parses weights from
the same `.glsl` files with the same clamped fetches.

```bash
python examples/generator/upscale_ref.py --selftest
#   sharp    finite=True range=[0.000,1.000] sharpness=0.1185 (bilinear 0.0949)
#   ai-s     finite=True range=[0.000,1.000] sharpness=0.1825 (bilinear 0.0949)
#   ai-m     finite=True range=[0.000,1.000] sharpness=0.1785 (bilinear 0.0949)
#   ai-m split accumulate vs single 1x1 conv: max |diff| = 0.00e+00
#   OK

python examples/generator/upscale_ref.py my_720p_frame.png --size 3840x2160
#   bilinear / sharp / ai-s / ai-m PNGs in output/upscale_ref/
```

A console capture of the same frame should match the reference within fp16
and 8-bit rounding. A large difference points at wrong weights, wrong
sampling or a wrong pass order. UL is not in the reference yet; its per-layer
term grouping is checked by assertions in the generator instead.
