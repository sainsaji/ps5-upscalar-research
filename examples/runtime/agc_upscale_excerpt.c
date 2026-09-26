/*
 * EVO Player #103 upscaler - runtime excerpt.
 *
 * Verbatim from EVO Player (GPL-3.0-or-later):
 *   projects/evoplayer/media/src/evo_agc_runtime.c   the upscale stage
 *   projects/evoplayer/media/src/evo_agc_writer.c    tiled texture descriptor
 *
 * NOT a standalone translation unit: it depends on EVO's bare-metal sceAgc
 * runtime (g_agc_dev, the transient ring, setup_color_target(), the writer's
 * PM4 helpers). It is here to be read - see docs/02-pipeline.md for the
 * walk-through. Section markers show where each piece lives in the original.
 */

/* ---- evo_agc_writer.c: sampling a GPU-rendered (64KB_R_X tiled) target ---- */

int evo_agc_build_tsharp_render_target(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                                       uint32_t width, uint32_t height, int fp16)
{
    /* Row pitch is implied by the tiling, so pass the unpadded row size and
     * leave the linear pitch field empty. */
    const uint32_t bpp = fp16 ? 8u : 4u;
    if ((gpu_address & 0xffffu) != 0u)
        return -1;
    int rc = build_tsharp_2d_internal(out, gpu_address, width, height, width * bpp,
                                      fp16 ? GFX10_FORMAT_16_16_16_16_FLOAT
                                           : GFX10_FORMAT_8_8_8_8_UNORM, bpp,
                                      SQ_SEL_X, SQ_SEL_Y, SQ_SEL_Z, SQ_SEL_W);
    if (rc == 0)
        out[3] |= (uint32_t)SQ_SW_64KB_R_X << 20;   /* SQ_IMG_RSRC_WORD3.SW_MODE */
    return rc;
}

/* ---- evo_agc_runtime.c: the upscale stage ---- */

/* =========================================================================
 * #103 upscaler
 *
 * Off is the single pass below: YUV -> RGB, bilinear, straight into the
 * scanout. With an upscaler on, and a source smaller than the image on the
 * panel, the YUV pass instead renders at SOURCE size into scratch surface L0,
 * and the chain draws the upscaled picture into the scanout:
 *
 *   Sharp   L0 -EASU-> E (visible image size) -RCAS-> scanout
 *   AI (S)  L0 -conv0..3-> F (RGBA16F, ping-pong) ; L0 + F -final-> scanout
 *   AI (M)  L0 -conv0..6-> F, and after each conv acc_k = acc_{k-1} +
 *           W_k * crelu(f_k) into A (ping-pong) ; L0 + A -final-> scanout
 *
 * The chain writes only the visible image rectangle - the same pixels the
 * Off quad covers - with the Fit/Fill/Stretch scale folded into that rect and
 * the source UV sub-rect it shows, so letterbox bars behave exactly as before.
 *
 * Scratch surfaces are a dedicated lazily-allocated block, not the RmlUi layer
 * pool: RmlUi CPU-clears a layer on acquire, and doing that to a surface the
 * GPU is still upscaling the previous frame from would corrupt one or the
 * other. Like every colour target setup_color_target() builds, they are
 * rendered 64KB_R_X TILED, so they are sampled back with a tiled T#
 * (evo_agc_build_tsharp_render_target) sized to the exact target - a linear
 * T# reads them as scrambled blocks, which was the first hardware run.
 *
 * Passes are separated by evo_agc_flush_color_target(): RELEASE_MEM event 45
 * with GCR 0xC = CB flush + GLV/GL1 invalidate, the same barrier the RmlUi
 * blur relies on between its H and V passes.
 * ========================================================================= */

#define EVO_AGC_UP_SURFACES   5
#define EVO_AGC_UP_EXT_SURFACES 8   /* Anime4K UL only */
/* 36 MB: colour targets are 64KB_R_X tiled, so a 4K RGBA8 image takes
 * 30 x 17 blocks of 64 KB = 33.4 MB, not its 31.6 MB linear size. */
#define EVO_AGC_UP_SLOT_BYTES UINT64_C(0x02400000)
enum {
    UP_SLOT_L0 = 0, UP_SLOT_E = 1, UP_SLOT_F0 = 1, UP_SLOT_A0 = 3,
    /* UL: two sets of 3 feature maps, two sets of 3 accumulators. Slots 5+
     * live in the second block. */
    UP_SLOT_UL_F = 1, UP_SLOT_UL_A = 7,
};

/* Whole-frame GPU time (submit -> retire) of an upscaled frame. A 60 fps
 * frame has 16.7 ms; leave room for the UI, the flip and poll granularity. */
#define EVO_AGC_UP_BUDGET_US  12000u
#define EVO_AGC_UP_WINDOW     120u

typedef struct {
    uint64_t addr;
    uint32_t width, height;   /* rendered extent */
    int      fp16;
    int      scanout;
} agc_up_surface_t;

typedef struct {
    uint64_t addr;
    uint32_t width, height;
    int      fp16;
    int      bilinear;
} agc_up_tex_t;

typedef struct {
    int      mode;                        /* SHARP or AI, after every fallback */
    int      net;                         /* AI: 0 = S, 1 = M, 2 = UL */
    uint32_t src_w, src_h;
    int      x0, y0, x1, y1;              /* visible image rect on the scanout */
    float    uv[4];                       /* source UV origin + extent shown there */
} agc_up_plan_t;

/* current_layer_target while a scratch surface is bound, so the next
 * evo_agc_set_layer_target() always re-emits its target. */
static const evo_agc_layer_surface_t s_up_target_sentinel;


static agc_up_surface_t up_surface(int slot, uint32_t w, uint32_t h, int fp16)
{
    agc_up_surface_t t;
    memset(&t, 0, sizeof(t));
    const int blk = slot >= EVO_AGC_UP_SURFACES;
    t.addr = (uint64_t)(uintptr_t)g_agc_dev.up.mem_base[blk] +
             (uint64_t)(slot - (blk ? EVO_AGC_UP_SURFACES : 0)) * EVO_AGC_UP_SLOT_BYTES;
    t.width = w;
    t.height = h;
    t.fp16 = fp16;
    return t;
}

/* Tiled footprint: 64 KB blocks of 128x128 pixels at 4 bpp, 128x64 at 8. */
static int up_fits(uint32_t w, uint32_t h, uint32_t bpp)
{
    const uint32_t bh = bpp == 8u ? 64u : 128u;
    const uint64_t blocks = (uint64_t)((w + 127u) / 128u) * ((h + bh - 1u) / bh);
    return blocks * 0x10000u <= EVO_AGC_UP_SLOT_BYTES;
}

static agc_up_tex_t up_tex(const agc_up_surface_t *s, int bilinear)
{
    agc_up_tex_t t = { s->addr, s->width, s->height, s->fp16, bilinear };
    return t;
}

/* Direct memory on first use rather than at boot: the upscaler defaults to
 * Off and most sessions never need it. Block 0 (180 MB) serves every mode,
 * block 1 (288 MB) only Anime4K UL. One attempt per block. */
static int agc_upscale_alloc(int blk)
{
    if (g_agc_dev.up.alloc_state[blk])
        return g_agc_dev.up.alloc_state[blk] > 0 ? 0 : -1;

    const size_t bytes = (size_t)(blk ? EVO_AGC_UP_EXT_SURFACES : EVO_AGC_UP_SURFACES) *
                         EVO_AGC_UP_SLOT_BYTES;
    int64_t off = -1;
    void *va = NULL;
    int rc = sceKernelAllocateDirectMemory(0, (off_t)16 * 1024 * 1024 * 1024ULL, bytes,
                                           EVO_AGC_DIRECT_MEM_ALIGN,
                                           EVO_AGC_DIRECT_MEM_TYPE, &off);
    if (rc == 0 && off >= 0) {
        rc = sceKernelMapDirectMemory(&va, bytes, EVO_AGC_MAP_PROTECTION, 0, off,
                                      EVO_AGC_DIRECT_MEM_ALIGN);
        if (rc != 0 || !va) {
            sceKernelReleaseDirectMemory(off, bytes);
            va = NULL;
        }
    }
    if (!va) {
        g_agc_dev.up.alloc_state[blk] = -1;
        evo_boot_log("agc upscale: scratch block %d alloc of %zu MB FAILED rc=%#x; %s",
                     blk, bytes >> 20, (unsigned)rc,
                     blk ? "AI Maximum unavailable" : "upscaler off");
        return -1;
    }
    g_agc_dev.up.mem_offset[blk] = off;
    g_agc_dev.up.mem_base[blk] = (uint8_t *)va;
    g_agc_dev.up.alloc_state[blk] = 1;
    evo_boot_log("agc upscale: %zu MB scratch block %d at %p (%zu x %llu MB)",
                 bytes >> 20, blk, va, bytes / EVO_AGC_UP_SLOT_BYTES,
                 (unsigned long long)(EVO_AGC_UP_SLOT_BYTES >> 20));
    return 0;
}

/* Called from shutdown after the GPU drain. */
static void agc_upscale_release(void)
{
    for (int blk = 0; blk < 2; ++blk) {
        const size_t bytes = (size_t)(blk ? EVO_AGC_UP_EXT_SURFACES : EVO_AGC_UP_SURFACES) *
                             EVO_AGC_UP_SLOT_BYTES;
        if (g_agc_dev.up.mem_base[blk]) {
            sceKernelMunmap(g_agc_dev.up.mem_base[blk], bytes);
            g_agc_dev.up.mem_base[blk] = NULL;
        }
        if (g_agc_dev.up.mem_offset[blk] >= 0) {
            sceKernelReleaseDirectMemory(g_agc_dev.up.mem_offset[blk], bytes);
            g_agc_dev.up.mem_offset[blk] = -1;
        }
        g_agc_dev.up.alloc_state[blk] = 0;
    }
}

static void up_viewport_scissor(int x, int y, int w, int h, int lim_w, int lim_h)
{
    evo_agc_writer_set_viewport(&g_agc_dev.current_cb, alloc_transient_cx(12),
                                (float)x, (float)y, (float)w, (float)h);
    int l = x < 0 ? 0 : x, t = y < 0 ? 0 : y;
    int r = x + w > lim_w ? lim_w : x + w, b = y + h > lim_h ? lim_h : y + h;
    evo_agc_writer_set_scissor(&g_agc_dev.current_cb, alloc_transient_cx(2),
                               (uint32_t)l, (uint32_t)t, (uint32_t)r, (uint32_t)b);
    g_agc_dev.scissor_x = l;
    g_agc_dev.scissor_y = t;
    g_agc_dev.scissor_w = r - l;
    g_agc_dev.scissor_h = b - t;
}

/* Point MRT0 at `t` and the viewport at (x, y, w, h) inside it. A scratch
 * target's registers are built per call into the transient ring, because its
 * size follows the source; the scanout reuses its prebuilt block. */
static int agc_up_bind_target(const agc_up_surface_t *t, int x, int y, int w, int h)
{
    if (t->scanout) {
        evo_agc_writer_set_target(&g_agc_dev.current_cb,
                                  g_agc_dev.gpu_regs->color_targets[g_agc_dev.active_backbuffer],
                                  16);
        g_agc_dev.current_layer_target = NULL;
        up_viewport_scissor(x, y, w, h, g_agc_dev.width, g_agc_dev.height);
        return 0;
    }
    SceAgcRegister *mrt = alloc_transient_cx(16);
    if (!mrt || !g_agc_dev.agc_defaults ||
        setup_color_target(mrt, g_agc_dev.agc_defaults, (void *)(uintptr_t)t->addr,
                           t->width, t->height, 0) != 0) {
        g_agc_dev.ring_alloc_fail++;
        return -1;
    }
    if (t->fp16) {
        /* CB_COLOR0_INFO: FORMAT = COLOR_16_16_16_16 (12), NUMBER_TYPE = FLOAT
         * (7), ROUND_MODE = 1 and no BLEND_CLAMP, as Mesa programs a float
         * target. setup_color_target wrote the 8_8_8_8 UNORM encoding. */
        mrt[2].value = (mrt[2].value & ~(0x7cu | 0x700u | 0x8000u)) |
                       (12u << 2) | (7u << 8) | 0x40000u;
    }
    evo_agc_writer_set_target(&g_agc_dev.current_cb, mrt, 16);
    g_agc_dev.current_layer_target = &s_up_target_sentinel;
    up_viewport_scissor(x, y, w, h, (int)t->width, (int)t->height);
    return 0;
}

/* One fullscreen pass of an upscale pipe: every one shares the vertex stage
 * of tools/gen_upscale_pipes.py (a vec4 source-UV rect) and reads `n`
 * combined textures from one fragment table. */
static int agc_up_pass(int pipe_id, const agc_up_surface_t *dst,
                       int x, int y, int w, int h, const float uv[4],
                       const agc_up_tex_t *tex, int n)
{
    SceAgcCommandBuffer *cb = &g_agc_dev.current_cb;
    evo_agc_transient_ring_t *ring = &g_agc_dev.transient_ring;
    const uint32_t slot = g_agc_dev.current_slot;

    if (!g_agc_dev.pipelines[pipe_id].valid)
        return -1;
    const evo_agc_user_data_layout_t ud = evo_agc_runtime_get_user_data_layout(pipe_id);
    if (!ud.vs_count || ud.vs_const_table_dword < 0 || ud.ps_texture_table_dword < 0 ||
        ud.vs_count > 16 || ud.ps_count > 16)
        return -1;

    evo_agc_transient_slice_t cons, vsh, desc;
    if (evo_agc_transient_ring_alloc(ring, slot, 16, 16, &cons) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16, 16, &vsh) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, (size_t)n * 48u, 16, &desc) != EVO_AGC_TRANSIENT_OK) {
        g_agc_dev.ring_alloc_fail++;
        return -1;
    }
    memcpy(cons.cpu, uv, 16);
    evo_agc_build_constant_vsharp((uint32_t *)vsh.cpu, cons.gpu_addr, 16);

    uint32_t *d = (uint32_t *)desc.cpu;
    memset(d, 0, (size_t)n * 48u);
    for (int i = 0; i < n; ++i) {
        uint32_t *td = d + 12 * i;
        int rc = evo_agc_build_tsharp_render_target(td, tex[i].addr, tex[i].width,
                                                    tex[i].height, tex[i].fp16);
        if (rc != 0) {
            g_agc_dev.tex_alloc_fail++;
            return -1;
        }
        evo_agc_build_ssharp(td + 8, 1, tex[i].bilinear);
    }

    if (agc_up_bind_target(dst, x, y, w, h) != 0)
        return -1;
    evo_agc_runtime_bind_pipeline(pipe_id);
    evo_agc_runtime_set_blend(EVO_AGC_BLEND_NONE);

    uint32_t vs_user[16] = {0};
    vs_user[ud.vs_const_table_dword] = (uint32_t)vsh.gpu_addr;
    evo_agc_writer_set_user_data_gs(cb, vs_user, ud.vs_count);
    uint32_t ps_user[16] = {0};
    ps_user[ud.ps_texture_table_dword] = (uint32_t)desc.gpu_addr;
    evo_agc_writer_set_user_data_ps(cb, ps_user, ud.ps_count);

    evo_agc_writer_draw_index_modifier(cb, 6, g_agc_dev.quad_indices,
                                       g_agc_dev.pipelines[pipe_id].draw_modifier);
    /* The next pass samples what this one wrote. */
    if (!dst->scanout)
        evo_agc_flush_color_target();
    return 0;
}

/* Hand the UI back a full-canvas scanout target. */
static void agc_up_restore_scanout(void)
{
    evo_agc_writer_set_target(&g_agc_dev.current_cb,
                              g_agc_dev.gpu_regs->color_targets[g_agc_dev.active_backbuffer], 16);
    g_agc_dev.current_layer_target = NULL;
    up_viewport_scissor(0, 0, g_agc_dev.width, g_agc_dev.height,
                        g_agc_dev.width, g_agc_dev.height);
}

static int up_pipes_valid(int first, int count)
{
    for (int i = 0; i < count; ++i)
        if (!g_agc_dev.pipelines[first + i].valid)
            return 0;
    return 1;
}

static const char *const k_up_mode_name[] = { "Off", "Sharp", "AI" };

/*
 * Decide what this frame gets. `sx`/`sy` are the Off quad's NDC half-extents,
 * so the image covers [W*(1-sx)/2, W*(1+sx)/2] - the plan keeps exactly that
 * footprint. Returns 1 with `pl` filled when the chain should run.
 */
static int agc_upscale_plan(uint32_t src_w, uint32_t src_h, int ten_bit,
                            float sx, float sy, agc_up_plan_t *pl)
{
    int mode = g_agc_dev.up.requested;
    if (mode > g_agc_dev.up.cap)
        mode = g_agc_dev.up.cap;
    const char *reason = NULL;
    int net = 0;

    const float W = (float)g_agc_dev.width, H = (float)g_agc_dev.height;
    const float img_w = sx * W, img_h = sy * H;
    const float ratio = (src_w && src_h)
        ? (img_w / (float)src_w < img_h / (float)src_h ? img_w / (float)src_w
                                                       : img_h / (float)src_h)
        : 0.0f;

    /* Visible part of the image, in whole pixels, and the source UV it shows. */
    const float fx0 = (W - img_w) * 0.5f, fy0 = (H - img_h) * 0.5f;
    int x0 = (int)(fx0 + 0.5f), y0 = (int)(fy0 + 0.5f);
    int x1 = (int)(fx0 + img_w + 0.5f), y1 = (int)(fy0 + img_h + 0.5f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_agc_dev.width) x1 = g_agc_dev.width;
    if (y1 > g_agc_dev.height) y1 = g_agc_dev.height;

    if (mode != EVO_AGC_UPSCALE_OFF) {
        if (ten_bit)
            reason = "HDR source";
        else if (ratio <= 1.05f)
            reason = "source >= output";
        else if (x1 <= x0 || y1 <= y0 || !up_fits(src_w, src_h, 4u) ||
                 !up_fits((uint32_t)(x1 - x0), (uint32_t)(y1 - y0), 4u))
            reason = "size";
    }
    /* AI: Anime4K only engages above 1.2x, and needs its feature maps to fit. */
    if (!reason && mode == EVO_AGC_UPSCALE_AI) {
        /* Auto follows detection; Standard/Large/Maximum are the Settings
         * override, which is how a PS5 Pro the probe cannot identify still
         * gets the big networks. Each steps down one network when it cannot
         * run: UL -> M -> S. */
        net = g_agc_dev.up.net_pref == EVO_AGC_UPNET_MAXIMUM ? 2
            : g_agc_dev.up.net_pref == EVO_AGC_UPNET_LARGE ? 1
            : g_agc_dev.up.net_pref == EVO_AGC_UPNET_STANDARD ? 0
            : evo_hw_is_ps5_pro();
        if (net > g_agc_dev.up.net_cap)
            net = g_agc_dev.up.net_cap;
        if (net == 2 && !(up_pipes_valid(EVO_AGC_PIPE_UP_UL_CONV0, EVO_AGC_UP_UL_CONVS) &&
                          up_pipes_valid(EVO_AGC_PIPE_UP_UL_ACC0, EVO_AGC_UP_UL_ACCS) &&
                          up_pipes_valid(EVO_AGC_PIPE_UP_RGB_FINAL, 1) &&
                          agc_upscale_alloc(1) == 0))
            net = 1;
        if (net == 1 && !(up_pipes_valid(EVO_AGC_PIPE_UP_M_CONV0, EVO_AGC_UP_M_CONVS) &&
                          up_pipes_valid(EVO_AGC_PIPE_UP_M_ACC0, EVO_AGC_UP_M_CONVS)))
            net = 0;
        if (ratio < 1.2f || !up_fits(src_w, src_h, 8u) ||
            (net < 2 && !up_pipes_valid(EVO_AGC_PIPE_UP_A4K_FINAL, 1)) ||
            (net == 0 && !up_pipes_valid(EVO_AGC_PIPE_UP_S_CONV0, EVO_AGC_UP_S_CONVS)))
            mode = EVO_AGC_UPSCALE_SHARP;
    }
    if (!reason && mode == EVO_AGC_UPSCALE_SHARP &&
        !(up_pipes_valid(EVO_AGC_PIPE_UP_EASU, 1) && up_pipes_valid(EVO_AGC_PIPE_UP_RCAS, 1)))
        reason = "pipeline missing";
    if (!reason && mode != EVO_AGC_UPSCALE_OFF && agc_upscale_alloc(0) != 0)
        reason = "no memory";

    if (reason || mode == EVO_AGC_UPSCALE_OFF) {
        g_agc_dev.up.label = mode == EVO_AGC_UPSCALE_OFF ? "Off"
            : !strcmp(reason, "HDR source") ? "Off (HDR source)"
            : !strcmp(reason, "source >= output") ? "Off (source >= output)"
            : "Off (unavailable)";
        mode = EVO_AGC_UPSCALE_OFF;
    } else {
        static const char *const k_net_label[] = { "AI (Standard)", "AI (Large)", "AI (Maximum)" };
        g_agc_dev.up.label = mode != EVO_AGC_UPSCALE_AI ? "Sharp" : k_net_label[net];
    }

    /* Log a change of plan once, not per frame. */
    const int key[6] = { g_agc_dev.up.requested, mode, net, (int)src_w, (int)src_h,
                         x1 - x0 };
    if (memcmp(key, g_agc_dev.up.last_key, sizeof(key)) != 0) {
        memcpy(g_agc_dev.up.last_key, key, sizeof(key));
        if (g_agc_dev.up.requested != EVO_AGC_UPSCALE_OFF) {
            if (reason)
                evo_boot_log("agc upscale: bypass requested=%s reason=%s src=%ux%u "
                             "img=%dx%d ten_bit=%d",
                             k_up_mode_name[g_agc_dev.up.requested], reason,
                             src_w, src_h, (int)img_w, (int)img_h, ten_bit);
            else
                evo_boot_log("agc upscale: mode=%s net=%s src=%ux%u -> rect=%d,%d %dx%d "
                             "(image %dx%d, %.2fx) cap=%s",
                             k_up_mode_name[mode],
                             mode != EVO_AGC_UPSCALE_AI ? "fsr1"
                             : net == 2 ? "anime4k-UL" : net ? "anime4k-M" : "anime4k-S",
                             src_w, src_h, x0, y0, x1 - x0, y1 - y0,
                             (int)img_w, (int)img_h, (double)ratio,
                             k_up_mode_name[g_agc_dev.up.cap]);
        }
    }
    if (mode == EVO_AGC_UPSCALE_OFF)
        return 0;

    pl->mode = mode;
    pl->net = net;
    pl->src_w = src_w;
    pl->src_h = src_h;
    pl->x0 = x0; pl->y0 = y0; pl->x1 = x1; pl->y1 = y1;
    pl->uv[0] = ((float)x0 - fx0) / img_w;
    pl->uv[1] = ((float)y0 - fy0) / img_h;
    pl->uv[2] = (float)(x1 - x0) / img_w;
    pl->uv[3] = (float)(y1 - y0) / img_h;
    return 1;
}

/* Anime4K UL: every layer is 3 RGBA16F textures (12 channels), each output
 * texture its own pass reading the whole previous layer. The 1x1 conv that
 * ends the network produces 3 textures - the residual for R, G and B - and is
 * run as accumulate passes for each layer it reads (2..6), per output. */
static int agc_upscale_run_ul(const agc_up_plan_t *pl, const agc_up_tex_t *l0_tex,
                              const agc_up_surface_t *scan)
{
    static const float full[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    enum { W = EVO_AGC_UP_UL_WIDTH };
    const uint32_t sw = pl->src_w, sh = pl->src_h;
    agc_up_surface_t f[2][W], a[2][W];
    for (int s = 0; s < 2; ++s)
        for (int j = 0; j < W; ++j) {
            f[s][j] = up_surface(UP_SLOT_UL_F + s * W + j, sw, sh, 1);
            a[s][j] = up_surface(UP_SLOT_UL_A + s * W + j, sw, sh, 1);
        }

    agc_up_tex_t cur[W + 1], acc[W];
    int have_acc = 0, pass = 0, apass = 0, rc = 0;
    for (int layer = 0; layer < EVO_AGC_UP_UL_LAYERS && rc == 0; ++layer) {
        const int set = layer & 1;
        agc_up_tex_t in[W];
        for (int j = 0; j < W; ++j)
            in[j] = layer ? cur[j] : *l0_tex;
        for (int j = 0; j < W && rc == 0; ++j, ++pass)
            rc = agc_up_pass(EVO_AGC_PIPE_UP_UL_CONV0 + pass, &f[set][j], 0, 0,
                             (int)sw, (int)sh, full, in, layer ? W : 1);
        for (int j = 0; j < W; ++j)
            cur[j] = up_tex(&f[set][j], 0);
        if (layer < EVO_AGC_UP_UL_FED_FIRST)
            continue;
        const int aset = (layer - EVO_AGC_UP_UL_FED_FIRST) & 1;
        for (int j = 0; j < W && rc == 0; ++j, ++apass) {
            cur[W] = have_acc ? acc[j] : cur[0];
            rc = agc_up_pass(EVO_AGC_PIPE_UP_UL_ACC0 + apass, &a[aset][j], 0, 0,
                             (int)sw, (int)sh, full, cur, have_acc ? W + 1 : W);
        }
        for (int j = 0; j < W; ++j)
            acc[j] = up_tex(&a[aset][j], 0);
        have_acc = 1;
    }
    if (rc == 0) {
        const agc_up_tex_t fin[4] = { *l0_tex, acc[0], acc[1], acc[2] };
        rc = agc_up_pass(EVO_AGC_PIPE_UP_RGB_FINAL, scan, pl->x0, pl->y0,
                         pl->x1 - pl->x0, pl->y1 - pl->y0, pl->uv, fin, 4);
    }
    return rc;
}

/* Everything after the YUV pass has filled L0. */
static int agc_upscale_run(const agc_up_plan_t *pl)
{
    static const float full[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    const uint32_t sw = pl->src_w, sh = pl->src_h;
    const int vw = pl->x1 - pl->x0, vh = pl->y1 - pl->y0;

    agc_up_surface_t l0 = up_surface(UP_SLOT_L0, sw, sh, 0);
    agc_up_surface_t scan;
    memset(&scan, 0, sizeof(scan));
    scan.scanout = 1;
    const agc_up_tex_t l0_tex = up_tex(&l0, 1);
    int rc = 0;

    if (pl->mode == EVO_AGC_UPSCALE_SHARP) {
        agc_up_surface_t e = up_surface(UP_SLOT_E, (uint32_t)vw, (uint32_t)vh, 0);
        const agc_up_tex_t e_tex = up_tex(&e, 0);
        rc = agc_up_pass(EVO_AGC_PIPE_UP_EASU, &e, 0, 0, vw, vh, pl->uv, &l0_tex, 1);
        if (rc == 0)
            rc = agc_up_pass(EVO_AGC_PIPE_UP_RCAS, &scan, pl->x0, pl->y0, vw, vh, full, &e_tex, 1);
    } else {
        agc_up_surface_t f[2] = { up_surface(UP_SLOT_F0, sw, sh, 1),
                                  up_surface(UP_SLOT_F0 + 1, sw, sh, 1) };
        agc_up_surface_t a[2] = { up_surface(UP_SLOT_A0, sw, sh, 1),
                                  up_surface(UP_SLOT_A0 + 1, sw, sh, 1) };
        agc_up_tex_t in = l0_tex, residual;
        if (pl->net == 2) {
            rc = agc_upscale_run_ul(pl, &l0_tex, &scan);
            goto done;
        }
        if (!pl->net) {
            for (int i = 0; i < EVO_AGC_UP_S_CONVS && rc == 0; ++i) {
                rc |= agc_up_pass(EVO_AGC_PIPE_UP_S_CONV0 + i, &f[i & 1], 0, 0,
                                  (int)sw, (int)sh, full, &in, 1);
                in = up_tex(&f[i & 1], 0);
            }
            residual = in;
        } else {
            agc_up_tex_t acc = {0};
            for (int i = 0; i < EVO_AGC_UP_M_CONVS && rc == 0; ++i) {
                rc |= agc_up_pass(EVO_AGC_PIPE_UP_M_CONV0 + i, &f[i & 1], 0, 0,
                                  (int)sw, (int)sh, full, &in, 1);
                in = up_tex(&f[i & 1], 0);
                const agc_up_tex_t acc_in[2] = { in, acc };
                rc |= agc_up_pass(EVO_AGC_PIPE_UP_M_ACC0 + i, &a[i & 1], 0, 0,
                                  (int)sw, (int)sh, full, acc_in, i ? 2 : 1);
                acc = up_tex(&a[i & 1], 0);
            }
            residual = acc;
        }
        const agc_up_tex_t fin[2] = { l0_tex, residual };
        if (rc == 0)
            rc |= agc_up_pass(EVO_AGC_PIPE_UP_A4K_FINAL, &scan, pl->x0, pl->y0, vw, vh,
                              pl->uv, fin, 2);
    }

done:
    agc_up_restore_scanout();
    if (rc == 0) {
        g_agc_dev.up.this_frame = 1;
    } else {
        static int s_fail_log = 4;
        if (s_fail_log > 0) {
            s_fail_log--;
            evo_boot_log("agc upscale: chain FAILED mode=%s src=%ux%u (ring_fail=%u tex_fail=%u)",
                         k_up_mode_name[pl->mode], sw, sh,
                         g_agc_dev.ring_alloc_fail, g_agc_dev.tex_alloc_fail);
        }
    }
    return rc ? -1 : 0;
}

/* frame_end hands over the GPU time of every upscaled frame. Two windows in a
 * row over budget (~4 s at 60 fps, so one slow frame cannot trip it) cap the
 * mode one step: AI -> Sharp -> Off, for the rest of the session. */
static void agc_upscale_note_gpu_time(uint64_t us)
{
    g_agc_dev.up.window_us += us;
    if (++g_agc_dev.up.window_frames < EVO_AGC_UP_WINDOW)
        return;
    const uint64_t avg = g_agc_dev.up.window_us / g_agc_dev.up.window_frames;
    evo_boot_log("agc upscale us=%llu n=%u mode=%s budget_us=%u (frame GPU submit->retire, 100 us grain)",
                 (unsigned long long)avg, g_agc_dev.up.window_frames,
                 g_agc_dev.up.label, EVO_AGC_UP_BUDGET_US);
    g_agc_dev.up.window_us = 0;
    g_agc_dev.up.window_frames = 0;

    if (avg <= EVO_AGC_UP_BUDGET_US) {
        g_agc_dev.up.over_budget_windows = 0;
        return;
    }
    if (++g_agc_dev.up.over_budget_windows < 2)
        return;
    g_agc_dev.up.over_budget_windows = 0;
    /* A big network steps down one size before AI gives way to Sharp. */
    if (g_agc_dev.up.last_key[1] == EVO_AGC_UPSCALE_AI && g_agc_dev.up.last_key[2] > 0) {
        static const char *const k_net[] = { "Standard", "Large", "Maximum" };
        const int from_net = g_agc_dev.up.last_key[2];
        g_agc_dev.up.net_cap = from_net - 1;
        g_agc_dev.up.downgrade_notice = EVO_AGC_UPSCALE_AI;
        evo_boot_log("agc upscale: over budget (us=%llu > %u) - AI %s -> AI %s "
                     "for this session", (unsigned long long)avg, EVO_AGC_UP_BUDGET_US,
                     k_net[from_net], k_net[from_net - 1]);
        return;
    }
    const int from = g_agc_dev.up.last_key[1];
    const int to = from > EVO_AGC_UPSCALE_OFF ? from - 1 : EVO_AGC_UPSCALE_OFF;
    g_agc_dev.up.cap = to;
    g_agc_dev.up.downgrade_notice = to;
    evo_boot_log("agc upscale: over budget (us=%llu > %u) - %s -> %s for this session",
                 (unsigned long long)avg, EVO_AGC_UP_BUDGET_US,
                 k_up_mode_name[from], k_up_mode_name[to]);
}

void evo_agc_upscale_set_mode(int mode)
{
    if (mode < EVO_AGC_UPSCALE_OFF || mode > EVO_AGC_UPSCALE_AI)
        mode = EVO_AGC_UPSCALE_OFF;
    if (mode == g_agc_dev.up.requested)
        return;
    /* A fresh choice in Settings is a fresh chance: drop the GPU-time cap. */
    g_agc_dev.up.requested = mode;
    g_agc_dev.up.cap = EVO_AGC_UPSCALE_AI;
    g_agc_dev.up.net_cap = 2;
    g_agc_dev.up.over_budget_windows = 0;
    g_agc_dev.up.window_frames = 0;
    g_agc_dev.up.window_us = 0;
}

void evo_agc_upscale_set_network(int pref)
{
    if (pref < EVO_AGC_UPNET_AUTO || pref > EVO_AGC_UPNET_MAXIMUM)
        pref = EVO_AGC_UPNET_AUTO;
    if (pref == g_agc_dev.up.net_pref)
        return;
    g_agc_dev.up.net_pref = pref;
    g_agc_dev.up.net_cap = 2;
    g_agc_dev.up.over_budget_windows = 0;
    g_agc_dev.up.window_frames = 0;
    g_agc_dev.up.window_us = 0;
}

const char *evo_agc_upscale_label(void)
{
    return g_agc_dev.up.label ? g_agc_dev.up.label : "Off";
}

int evo_agc_upscale_take_downgrade(void)
{
    const int v = g_agc_dev.up.downgrade_notice;
    g_agc_dev.up.downgrade_notice = -1;
    return v;
}

/* The Off quad's NDC half-extents for Fit (0) / Fill (1) / Stretch (2). */
static void agc_video_scale(int disp_w, int disp_h, int view_mode, float *sx, float *sy)
{
    *sx = 1.0f;
    *sy = 1.0f;
    if (view_mode != 2 && disp_w > 0 && disp_h > 0 && g_agc_dev.width > 0 && g_agc_dev.height > 0) {
        float va = (float)disp_w / (float)disp_h;
        float sa = (float)g_agc_dev.width / (float)g_agc_dev.height;
        if (view_mode == 0) { /* FIT (letterbox) */
            if (va > sa) *sy = sa / va; else *sx = va / sa;
        } else {               /* FILL (crop overflow) */
            if (va > sa) *sx = va / sa; else *sy = sa / va;
        }
    }
}

/* ---- evo_agc_runtime.c: evo_agc_blit_yuv() - where the stage hooks in ---- */

int evo_agc_blit_yuv(/* ...planes, sizes, view mode, pts... */)
{
    /* ...bind the YUV->RGB pipeline for this frame's format... */

    float sx, sy;
    agc_video_scale(disp_w, disp_h, view_mode, &sx, &sy);

    /* #103: with an upscaler engaged this pass renders the picture at source
     * size into scratch surface L0 instead, and agc_upscale_run() below takes
     * it to the scanout. */
    const uint32_t src_w = (disp_w > 0 && disp_w <= coded_w) ? (uint32_t)disp_w : (uint32_t)coded_w;
    const uint32_t src_h = (disp_h > 0 && disp_h <= coded_h) ? (uint32_t)disp_h : (uint32_t)coded_h;
    agc_up_plan_t up_plan;
    const int upscale = agc_upscale_plan(src_w, src_h, ten_bit, sx, sy, &up_plan);

    /* 2. Fullscreen viewport and scissor */
    if (!upscale) {
        evo_agc_writer_set_viewport(&g_agc_dev.current_cb, alloc_transient_cx(12), 0.0f, 0.0f,
                                    (float)g_agc_dev.width, (float)g_agc_dev.height);
        evo_agc_writer_set_scissor(&g_agc_dev.current_cb, alloc_transient_cx(2), 0, 0,
                                   (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height);
        g_agc_dev.scissor_x = 0;
        g_agc_dev.scissor_y = 0;
        g_agc_dev.scissor_w = g_agc_dev.width;
        g_agc_dev.scissor_h = g_agc_dev.height;
    }

    /* ...VideoConstants (crop, scale), stage the Y/UV planes into the
     *    transient ring, build their texture descriptors... */

    /* #103: switch to L0 only now, after every early return above - one of
     * those leaving MRT0 on a scratch surface would send the UI there too. */
    if (upscale) {
        agc_up_surface_t l0 = up_surface(UP_SLOT_L0, src_w, src_h, 0);
        if (agc_up_bind_target(&l0, 0, 0, (int)src_w, (int)src_h) != 0) {
            agc_up_restore_scanout();
            return -1;
        }
    }

    /* ...draw the quad, note_draw()... */

    if (upscale) {
        evo_agc_flush_color_target();   /* the chain samples L0 */
        if (agc_upscale_run(&up_plan) != 0)
            return -1;
    }

    /* ...stamp the buffer with this frame's pts... */
    return 0;
}
