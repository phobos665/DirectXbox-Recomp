# AI texture upscaling, faithful to the original art

An investigation, 19 October 2026. Nothing in `src` was changed to write it, and no title
was run: there are no game files in the container this was written in. Where a number was
measured it says so; everything else is marked as an estimate or as unverified.

It builds on `modding-models-textures.md` and keeps its item numbers: T4 is the
content-hash dump, T5 the hash-keyed replacement, T7 the cube/volume/P8 coverage. None of
T4 or T5 exists yet. What this note adds is the pipeline that makes the replacement
textures: 1x cleanup of compression artefacts, a smooth super-resolution model at 2x or
4x, and a **back-projection** step so that any detail added has to average back to the
artist's own pixels. Diffusion upscalers are out. GAN models are allowed only as a preset
the player opts into.

**Before any of this, try `RECOMP_ANISO=16` and `RECOMP_RES_SCALE=2`.** Both exist
(`src/d3d/d3d8_display.c:40-65`, `src/d3d/d3d8_states.c:387-404`). A texture that looks
mushy on a floor seen side-on is a filtering problem, not a resolution problem. Upscaling
is for textures that are *magnified*, where there are more screen pixels than texels.

---

## What changes the plan

1. **`RECOMP_TEX_DUMP` is already taken.** `src/kernel/nv2a_pb_exec.c:1086` reads it as a
   file-name prefix for the LLE executor's BMP dumper. T4 needs another name, e.g.
   `RECOMP_TEXPACK_DUMP=<dir>`, and T5 `RECOMP_TEXPACK=<dir>`.
2. **"The original lower mips, exactly" cannot hold in a BC7 texture.** A texture has
   one format. BC7 has no mode that reproduces BC1's 1/3 and 2/3 interpolation exactly.
   Its nearest weights are 21/64 and 43/64, so re-encoding the shipped DXT1 levels as
   BC7 moves them by a level or two out of 255. BC1 decoding also differs slightly
   between GPU vendors. So "exact" has two possible meanings, and the recipe has to
   choose one per texture:
   - *Same-format output* (DXT1 in, DXT1 out): the shipped levels go in byte for byte,
     but the new top levels carry DXT1 blocking again.
   - *BC7 output*: the new levels look better, and the shipped levels are only
     near-exact (to be measured, expected within 2/255).
3. **Back-projection only fixes the mean of each block.** If every s×s block of the
   upscale averages to the original texel, the check passes. Detail that sums to zero
   inside a block still passes, and that includes invented texture. So back-projection
   is necessary but not enough. The checks that tell outputs apart are the halo and
   added-detail measures in §4.
4. **The licence and training data narrow the model choice a lot** (§5). I found no
   1x DXT-cleanup model on OpenModelDB that both has a permissive licence and states it
   was trained without a GAN. The permissive ones (WTFPL) are hosted on Google Drive or
   Mega, which a tool cannot reliably download from.

   The cleanest super-resolution default found is `4x-pix-span-pretrain`: L1 loss only,
   trained over mixed downsamplers including box, CC-BY-4.0, 9 MB, on a GitHub release.
5. **The runtime work is a little bigger in one place and smaller in another.**
   - Bigger: there is no BC7 anywhere in the RHI (`src/d3d/rhi.h:94-97` has BC1/2/3/5
     only), and the D3D8 layer accepts only Xbox format codes.
   - Smaller: T7 has shrunk. Static cube textures and P8 are mirrored now
     (`src/hle/hle_d3d8_texture.c:1401-1552` and `:268-283`), and only volume textures
     are still refused (`:244`).
   - Out of date: the cache checksum is no longer the 4096-byte sample that
     `modding-models-textures.md` describes. It covers every byte of every level
     (`:328-374`), but it is folded to **32 bits**, which is too weak as an identity
     key for a shared recipe.

---

## 1. The runtime side, in the code

### 1.1 Where the hooks go

Every 2D texture the title binds passes through `shadow_set_texture()`
(`src/hle/hle_d3d8_texture.c:2076`) and then `host_texture()` (`:837`). That includes the
D3DSurfaces it binds as textures, and the textures that `D3DDevice_SwitchTexture` and
the push-buffer walk name only by offset and format (synthetic keys, `:153-200`, `:2001`).

`host_texture()` hands back four kinds of thing before it gets to an ordinary texture:

- rendered surfaces (`:859-868`);
- the device depth read as colour (`:877-897`);
- the frame buffer (`:901-906`);
- a cache hit (`:908-976`).

A cache miss goes to `read_layout()` (`:978`), `host_CreateTexture()` (`:992`) and
`upload()` (`:1013`). That miss path is the hook for both pieces:

- **Dump (T4):** after `read_layout()` succeeds and the frame-buffer check at `:984`
  has said no. There the bytes are exactly the asset (measured in
  `modding-models-textures.md`: 98 of 98 genuine assets matched the disc, whole chain).
- **Replace (T5):** in place of the `host_CreateTexture` + `upload` pair. Look the key
  up in the pack index. On a hit, create the host texture at the pack's size and format
  and fill it from the pack. Mark the entry `replaced`.

Static cubes come through `static_cube()` (`:1484`), which is a separate cache. The
cube part of T7 is the same two hooks placed there.

### 1.2 What is known at the hook

| Fact | Where it comes from | Notes |
|---|---|---|
| Format, width, height, mip count | `read_layout()` `:266-317`, from the Format word (log2 dims, levels) or the Size word (linear) | Exact |
| Swizzled/compressed or linear | `t.linear` (Size != 0), `d3d8_format_is_linear()` `src/d3d/d3d8_resources.c:1720` | Exact |
| P8 | `t.fmt == 0x0B`; the palette is per *stage*, read at each draw (`sync_palette` `:1079`) | The texels do not tell you the palette |
| Cube | Format bit 2; goes to `static_cube()` | Volume (dimension 3) refused at `:244` |
| Render target, frame buffer, drawn surface | entry `rendered` / `framebuffer` (`:85-95`), `hle_d3d8_is_framebuffer()` | Never replace these |
| Bytes, physical address | `t.bytes`, `t.phys` | Guest addresses are not stable keys |
| Stage it is bound to | the `stage` argument of `shadow_set_texture` | Known at bind time, not at upload time |
| Filtering, addressing, alpha test and blend, combiner, projection | **not known here** | Only at the draw (§1.3) |

### 1.3 Signals only a draw can give, recorded into the manifest

Classification (§2) needs usage, not just pixels. The place to collect it is
`d3d8_shaders_prepare_draw()` (`src/d3d/d3d8_shaders.c:1194`). That is where the bound
textures, the TSS array, the render states and the screen-space verdict all exist at
once:

- the screen-space verdict is `d3d8_SetTwoDSqueeze(...)` at `:1208` and `:1220`, the
  same test the widescreen 2D squeeze has been tuned on;
- `ff_op_reads_texture()` at `:1183` says whether a fixed-function stage reads its
  texture at all.

The proposal: the HLE stamps each host texture it creates with its content key (a
64-bit field on `D3D8Texture`). The draw path then adds to a small per-key table:

- draws in total, and screen-space draws;
- for each stage: which stages it appeared on, and the `TEXCOORDINDEX`, `COLOROP` and
  `ALPHAOP` it ran under, or "combiner program" when a combiner draws;
- `ADDRESSU`/`ADDRESSV` (wrap, mirror, clamp, border) and `MAG`/`MIN`/`MIPFILTER`;
- `ALPHATESTENABLE` with the most common `ALPHAREF`, and `ALPHABLENDENABLE` with
  `SRCBLEND`/`DESTBLEND`;
- whether it was the source stage of a `BUMPENVMAP` or a dot-product (`texm3x3`)
  combiner mode, or used with a texture-coordinate generation mode;
- whether its content was ever seen to change (`texels_changed()` firing at `:965` or
  `:1208`).

The dump writes this per key into `manifest.json` on exit and every N seconds, beside
`<key>.dds`. An F11 capture (`src/hle/d3d8_capture.h`) holds the same state for one
frame. That makes captures a second, offline source of the same signals, but a session
dump covers far more textures.

### 1.4 The paths that assume the host texture mirrors the guest's

`modding-models-textures.md` named three. There are six, and each one writes guest-format
bytes into the host texture with the guest's dimensions. Into a larger BC7 texture that
gives garbage, not just a stale picture:

1. **Re-upload on change.** `texels_changed()` leads to `upload()` at `:965-973`, and
   `refresh_bound()` at `:1197-1214` does the same.

   For a replaced entry, a change in content has to mean "this is not the asset any
   more". Re-key the entry. If the new key is in the pack, swap in that replacement;
   if not, recreate the host texture in the guest's format and upload as today. The
   manifest should mark the key `dynamic`.
2. **P8 baking.** `bake()` and `sync_palette()` (`:1055-1151`) re-expand the guest
   indices.
3. **The budget.** `entry_bytes()` (`:537`) counts guest bytes. It has to count the
   replacement, and the CPU shadow copy as well (point 6).
4. **The render-target conversion.** `hle_d3d8_render_texture()` (`:1559`) recreates an
   entry as a render target. That is correct as it is: a texture the title draws into
   stops being an asset.
5. **The static cube re-check.** `static_cube()` (`:1515-1524`).
6. **The CPU shadow copy and the capture.** `d3d8_CreateTextureImpl()` keeps the whole
   chain in `sys_mem` as well as on the GPU (`src/d3d/d3d8_resources.c:1584`), and
   `texture_write()` (`src/hle/hle_d3d8_record.c:178`) records `sys_mem` into captures.

   A 4x BC7 replacement doubles in host RAM and makes captures large. Proposal: a
   replaced texture drops `sys_mem` after upload, and the capture records a
   `TEXTURE_REF <key>` chunk instead of the texels. That also gives the A/B loop for
   free (§6).

### 1.5 Linear textures: skip them in v1

The NV2A addresses `LIN_` formats (and YUY2/UYVY) in texels. Both pixel paths scale the
coordinates by `1/size` of the **bound host texture**:

- `src/d3d/d3d8_combiners.c:1634-1641`;
- `src/d3d/d3d8_shaders.c:1303-1310`;
- both through `d3d8_base_size()`, `src/d3d/d3d8_resources.c:1739`.

A linear texture replaced at 4x scale would therefore be sampled over its top-left
quarter. A replacement in a non-linear format would lose the scaling entirely, and every
coordinate would land outside 0..1.

To get it right, `D3D8Texture` would need a *logical* width and height for `tex_scale`
that is separate from the stored size. Every size-based heuristic in `src/d3d` would
then have to use the logical size too. One already exists:

- `draw_is_screen_pass()` (`src/d3d/d3d8_device.c:2313-2341`) treats a stage-0 texture
  of at most 4x4 as flat colour, and one at least the guest's size as a picture of the
  screen.

It is not worth it for v1. In the TimeSplitters 2 frames measured, the only linear
textures were render targets (`modding-models-textures.md`), and in other titles they
are movies (YUY2) and screen copies. **Rule: linear formats are never replaced in v1**,
and the ≤4 px skip rule in §2 keeps the 4x4 heuristic intact.

### 1.6 Mip chains and samplers

The host keeps the chain in the Xbox's own packing, back to back. Each level is
unswizzled and converted at `UnlockRect` (`d3d8_upload_mip_level()`,
`src/d3d/d3d8_resources.c:1446`). A level-count of 0 generates a full chain
(`:1573-1581`), but `host_texture()` always passes the guest's count.

A replacement at scale s = 2^k has `levels + k` levels:

- the new levels 0..k-1;
- then the shipped chain, starting from the guest's level 0.

Two things in the sampler matter here:

- **`MIPFILTER NONE` already mip-selects.** `d3d8_to_rhi_filter()`
  (`src/d3d/d3d8_states.c:268-287`) maps it to point mip filtering with
  `max_lod = FLT_MAX` (`:407`). A UI texture that had no mips therefore steps between
  the 4x, 2x and 1x levels by distance without anything being added, which suppresses
  minification shimmer. For replaced textures it would look better to force linear mip
  filtering with `max_lod = k + guest_levels - 1`.
- **`D3DTSS_MAXMIPLEVEL` and `MIPMAPLODBIAS` are forwarded but not applied.** They
  reach the host stage-state array (`src/hle/hle_d3d8_state.c:450-451`), but the
  sampler builder ignores both. Whoever implements them must add k to `MAXMIPLEVEL` for
  a replaced texture, or the title's clamp lands two levels too high.

### 1.7 A different size and format through the upload path

**Size is free.** `host_CreateTexture()` takes any dimensions, and sampling is
normalised for non-linear formats.

**Format is not.** `d3d8_CreateTextureImpl()` takes an Xbox `D3DFORMAT`. BC7 needs all
of the following:

- `RHI_FORMAT_BC7_UNORM` (DXGI 98) in `rhi.h`, `rhi.c` (block bytes 16),
  `rhi_d3d11.c` (`SAME` mapping, `is_block_compressed` at `:506`) and `rhi_vulkan.c`
  (`VK_FORMAT_BC7_UNORM_BLOCK`, and the block list at `:434-436`);
- a host-only `D3DFORMAT` code outside the Xbox range, e.g. `0xF0`, in
  `d3d8_to_dxgi_format`, `d3d8_format_bpp` (8), `d3d8_format_is_compressed` and
  `d3d8_row_pitch` (16-byte blocks, which `:436-444` already gives);
- not swizzled and not linear, so `d3d8_format_is_swizzled` and
  `d3d8_format_is_linear` leave it alone;
- the replay tool learns it for free, because it links the same `src/d3d`.

No sRGB formats exist anywhere in the RHI, so the renderer filters stored values. That
matters for §4.

**Unverified:** BC7 on MoltenVK. Apple documents BC as always supported on macOS, and
BC1-3 demonstrably work there (TimeSplitters 2's DXT1 textures render on the Mac), but
I have not seen BC7 sampled through MoltenVK.

### 1.8 The identity key

The key is a 64-bit hash (XXH3-64, or the existing 64-bit FNV-style mix at `:362-372`
*before* it is folded to 32 bits) over the following:

```
key = H64( "xtex1" | fmt | width | height | levels | canonical chain bytes )
```

"Canonical" means the bytes `upload()` puts in `sys_mem`: levels back to back, swizzled
as the guest has them, linear rows without the 64-byte padding. That is also exactly what
a capture's `D3D8CAP_TEXTURE` chunk holds (`texture_write()`), so the replay tool can
compute the same key from a capture with no game running.

Two notes on the key:

- **The palette is not in it.** P8 replacements stay P8 (§7).
- **Why 64 bits.** With 32 bits and a few thousand textures per title, a collision is
  about 0.1% likely per title (estimate), and a shared recipe will meet many titles.

---

## 2. Classification

Each texture gets a route, recorded in the manifest with the signals that chose it. The
rules below are meant to be testable. "Usage" means the §1.3 counters, and "pixels"
means statistics of the decoded level 0. The rules are checked in this order, and the
first match wins.

| Route | Rule | Reliability |
|---|---|---|
| **never** | rendered / frame buffer / depth-as-colour entry; linear format; content changed after upload (`dynamic`); volume texture | Exact: these are runtime facts |
| **skip: not an image** | min(w,h) ≤ 4 or max(w,h)/min(w,h) ≥ 16 with min ≤ 8 (ramps, strips, 1-px LUTs); ≤ 2 distinct colours; every row (or every column) identical, i.e. a 1D ramp; bump formats (`V8U8`, `Q8W8V8U8`, `L6V5U5`, `X8L8V8U8`), `DXN`, `CTX1`, depth and float formats; used as a `BUMPENVMAP` source or by a dot-product combiner mode | Reliable for formats and usage; the size and ramp rules are reliable |
| **skip: normal map** (until a normal-aware path exists) | over 90% of texels decode to a vector with \|n\| within 0.1 of 1, mean near (0.5, 0.5, 1.0). TimeSplitters 2 ships DXT1 normal maps (`.xbt` format code "Morton-swizzled DXT1 normal map") | Reliable on stats; check a sample |
| **skip: generated cube** | a cube whose faces match a generated normalisation cube within 2/255 RMS | Reliable (analytic test) |
| **ui** | ≥ 90% of draws screen-space, *or* `MAGFILTER POINT` on ≥ 90% of draws | Reliable. The screen-space test already decides the widescreen squeeze |
| **ui: font** | ui, and alpha mostly 0 or 1 (≥ 95% of texels within 8/255 of 0 or 255), and connected alpha components of similar size on a grid | Medium. Glyph-grid detection fails on proportional atlases; review |
| **lightmap** | appears only on stage ≥ 1, with `TEXCOORDINDEX` ≠ stage 0's, `COLOROP` `MODULATE`/`MODULATE2X`/`MODULATE4X`, no alpha use, clamp addressing; and pixels: mean saturation < 0.15, little high-frequency energy | Medium. Combiner programs hide the modulate; use the coordinate set and the stage, and review those cases |
| **review** | faces, logos, signs, text inside 3D textures. Heuristics: drawn on a character (via T6 names, e.g. `chr.pak`), OCR finds text, high-contrast flat-colour regions | **Unreliable** on its own: a face texture is an unwrapped atlas, not a frontal face. It is a flag for a human, never a decision |
| **main** | everything else | — |

**Where a key has mixed usage** (the same content drawn for two purposes, or two ids
sharing a hash as in the modding doc's 1 in 102), it takes the most conservative route
it qualified for, in the order never > skip > ui > lightmap > review > main.

What each route does:

| Route | Processing |
|---|---|
| never, skip | Not in the pack |
| ui | ScaleFX or xBRZ (MIT and GPL-3, both compatible with this GPL-3 fork), or untouched. A font stays untouched by default |
| lightmap | The smooth model or Lanczos, then a gentle denoise, then back-projection. Never a GAN model |
| review | Processed as main and held back until accepted |
| main | §3 |

---

## 3. The per-texture pipeline

1. **Decode** the chain from the dump (unswizzle, DXT decode, P8 through the palette
   only for viewing).
2. **Alpha split.**
   - *Cut-out*: alpha test on, or DXT1 1-bit. Keep the coverage mask, and fill the
     colour under transparent texels outward (push-pull) so that the model never sees
     black fringes.
   - *Blended*: carry alpha as its own channel.
3. **Pad** by the address mode: wrap for tiling textures, mirror for mirror, edge
   replicate for clamp. The proposal was a 3x3 tiling. The model's receptive field
   needs only a margin of max(16 px, ¼ size), which is cheaper; crop afterwards.
4. **1x cleanup**, DXT sources only, projected back onto the original compression
   (§4.3).
5. **Super-resolution** at the route's scale: 2x by default, 4x where §6 allows.
6. **Back-projection** (§4) on colour, on premultiplied colour for blended textures, and
   on alpha. Then re-threshold cut-out alpha at the recorded `ALPHAREF` (0.5 for DXT1).
7. **Build the chain.** The new levels 1..k-1 are box downsamples of the new level 0, in
   the same domain as step 6. After them comes the shipped chain: verbatim in
   same-format output, decoded and re-encoded in BC7 output.
8. **Encode** BC7, or DXT1 for same-format output, and **check** (§4.4). If the check
   fails, take the fallback: Lanczos plus back-projection, and if that fails too, no
   replacement.

Step 8's fallback is worth building first. Lanczos or bicubic with back-projection adds
no invented detail at all, and it is a useful pack on its own: sharper than the GPU's
bilinear filter when magnified, and consistent with the original by construction.

---

## 4. Back-projection

### 4.1 Algorithm

This is iterative back-projection (Irani and Peleg, 1991), with the downsampling operator
chosen to match how the texture is minified.

```
y  = cleaned original level 0           (h x w)
x0 = SR(y)                              (sh x sw)
for k in 0..K-1:
    r = y - D_s(x_k)                    D_s: s x s box mean
    if max|r| < 0.25/255: break
    x_{k+1} = clamp01(x_k + U_s(r))     U_s: bicubic, padded by address mode
x  = x_K + N_s(r_K)                      N_s: nearest; forces D_s(x) = y exactly
```

Details:

- **Iterations:** K = 10 at most. With step 1 and a bicubic `U_s`, it is expected
  (estimate) to converge in 4-8 iterations.
- **The last step** spreads a residual under 0.25/255 evenly over each block, which
  makes the mean exact and is invisible.
- **Clamping:** `clamp01` can stop convergence next to saturated texels. Those blocks
  are reported, not forced.

### 4.2 Colour space

Back-projection runs on **stored (gamma-encoded) values** by default. Two reasons:

- the host renderer has no sRGB formats, so the GPU's own mip and bilinear averaging is
  done on stored values;
- the shipped chain is what the GPU drops to at the original texel density.

Linear light is the more perceptually correct average, so the recipe carries
`bp.space = stored | linear`.

**Settle it by measurement, per title.** The dump holds the shipped mips. Compare the
shipped level 1 against box(level 0) computed in each space, and against Kaiser and
bicubic downsamples too. Whichever matches best is how the artist's toolchain made its
mips, and the operator and space to use (unverified for any XDK-era tool). The same
comparison gives a natural tolerance: back-projection cannot be asked to match the
original better than the toolchain matched itself.

The models are fed stored values, which is how they were trained.

### 4.3 Interaction with alpha and with the cleanup

- **Blended alpha:** back-project α. Back-project colour as α·c, with the mean in
  `D_s` weighted by α, so transparent texels do not pull a block's colour.
- **Cut-out alpha:** back-project colour only over the opaque region. Threshold the
  upscaled α at `ALPHAREF`. Coverage check: in at most 2% of original texels may the
  majority state of its s×s block differ from the original texel's state.
- **DXT-cleaned input:** this is the step that keeps the cleanup honest.

  Each DXT1 block stores two endpoints and a 2-bit index per texel, and the four
  palette colours lie on one line. So each index stands for a slab of colour space
  along that line. Project each cleaned texel into its own index's slab, and to within
  τ of the line, where τ is the spacing of the block's palette.

  After that, re-encoding the cleaned image *with the original endpoints* reproduces
  the original indices. The cleanup can then only choose among images the disc's data
  allows. This is the DXT analogue of quantisation-constraint deblocking for JPEG. It
  is a design proposal; I have not seen it applied to BCn.

  Back-projection then targets the cleaned y, so D(x) differs from the raw DXT decode
  by at most one quantisation step.
- **Normal maps,** if ever: back-project in vector space and renormalise after each
  step.

### 4.4 Pass/fail metric (starting values, to be calibrated)

The values are measured on the *encoded* result, decoded back:

| Check | Pass |
|---|---|
| Consistency: \|D_s(x̂) − y\| | max ≤ 2/255, RMS ≤ 0.75/255 (BC7 error included) |
| Halo: new texels outside the [min, max] of their 3x3 original neighbourhood, widened by 4/255 | ≤ 0.5% of texels |
| Added detail: A = RMS(x̂ − bicubic(y)) / RMS(y − blur_σ=1(y)) | smooth path: flag at A > 1.0, expect ≤ 0.5. GAN preset: report, never pass automatically |
| Coverage (cut-out alpha) | ≤ 2% (§4.3) |
| Mean colour drift per 16x16 region against the original | ≤ 1/255 |

The first check is satisfied by construction and catches bugs. The second and third are
the ones that catch invented detail, because zero-mean detail passes the first (point 3
at the top of this note). A texture that fails goes down the fallback chain in §3 and is
listed in the report.

---

## 5. Models and tools

The model facts below were read from the OpenModelDB data repository
(`github.com/OpenModelDB/open-model-database`, snapshot of 14 Aug 2026): licence, host,
size, and the training loss where the author states it. The site itself could not be
reached from here.

OpenModelDB does not record a model's training loss. Most community ESRGAN texture
models are trained with the usual perceptual+GAN recipe, so **treat any model that does
not state its loss as a GAN model.**

### 5.1 Super-resolution (non-GAN)

| Model | Scale | Arch | Licence | Host | Loss / data | Notes |
|---|---|---|---|---|---|---|
| `4x-pix-span-pretrain` (Hofmann) | 4 | SPAN, 9 MB | CC-BY-4.0 | GitHub | **L1 only**, nomos_uni, degraded with box, linear, Mitchell, Lanczos, Gauss | **Default candidate.** Fast. A "pretrain", so lower peak quality than a finished model |
| `4x-mssim-span-pretrain` | 4 | SPAN, 9 MB | CC-BY-4.0 | GitHub | MS-SSIM only, same data | The A/B partner of the one above |
| `2x/4x-classicalSR-DF2K-s64w8-SwinIR-M` | 2, 4 | SwinIR, 67 MB | Apache-2.0 | GitHub | L1, bicubic degradation (paper) | Slow. A bicubic-trained model meets box back-projection; measure |
| `4x-HAT-S-SRx4`, `2x/4x-DAT-2`, `2x/4x-OmniSR-DF2K`, `2x/4x-spanx*-ch48` | 2, 4 | various | Apache-2.0 | Google Drive | L1 classical (papers) | Manual download only |
| `4x-SRFormer-SRx4-DF2K` | 4 | SRFormer | weights Apache-2.0 | Drive | L1 | **Avoid.** Its architecture is only in `spandrel_extra_arches`, the package for restrictively licensed code |

### 5.2 1x DXT cleanup

| Model | Licence | Host | Notes |
|---|---|---|---|
| `1x-BCGone-Smooth` | WTFPL | Drive | BC1. DIV2K and anime data. Loss not stated |
| `1x-DXTDecompressor-Source-V3` | WTFPL | Drive | DXT1, Source-engine textures including normals. Also corrects a green tint, which is a *colour change*: wrong here unless the title's DXT has the same bias |
| `1x-DEDXT` | WTFPL | Mega | 150 Paladins textures. Fine-tuned from BCGone-DetailedV2 |
| `1x-BC1-smooth2`, `1x-SpongeBC1-Lite`, `1x-artifacts-bc1-free-alsa` | CC-BY-NC(-SA) | Drive/pCloud | Fine for a player's own use; the tool must not mirror them |

**Recommendation:**

1. Start with the non-ML DXT-consistent smoothing in §4.3: a TV-regularised decode
   projected onto the block constraints. It cannot invent anything.
2. If a model is wanted, train one that the project owns: SPAN, 1x, L1, on CC0
   textures (ambientCG, Poly Haven: the sources Kim2091's PBRify used) compressed with
   an era-like DXT1 encoder. **Estimate: 3-5 days including the dataset**, a few GPU
   hours. The same data trained at 2x and 4x with box degradation would give a default
   super-resolution model whose licence and training are both known.

### 5.3 GAN preset (opt-in only)

- **`4x-PBRify-UpscalerV4`:** CC0, DAT2, 140 MB, GitHub. 2000s game textures.
  Described as adding "natural detail"; loss not stated.
- **`4x-HDCube`:** CC0, ESRGAN, GitHub. GameCube and Wii DXT textures, "not suitable
  for pixel art, small icons and text under 16 pixel". Its pretrain is NMKD Siax, a
  GAN model.

Both are CC0 on GitHub, so the tool may download them.

### 5.4 Runtime and encoders

| Tool | Licence | Platforms | Use |
|---|---|---|---|
| `spandrel` 0.4.2 (Feb 2026) | MIT; core holds only MIT/Apache/public-domain architectures | wherever PyTorch runs: CUDA on Windows and Linux, MPS on macOS | Loads `.pth`/`.safetensors` and detects the architecture. Running SPAN and SwinIR on MPS is **unverified** |
| chaiNNer | GPL-3.0 | Windows, macOS, Linux | For experimenting by hand only; the tool calls spandrel directly |
| bc7e (in `bc7enc_rdo`, and as scalar C++ in basis_universal v2.5) | bc7e.ispc Apache-2.0; the rest MIT or public domain | portable C++; ISPC has an ARM target | **Vendor it** as a small CLI built with the project: no binary downloads, compatible with GPL-3 |
| Compressonator 4.x | MIT (core and CLI) | Windows, Linux; macOS CLI per its revision notes | Alternative |
| DirectXTex `texconv` | MIT | Windows (GPU BC7 through DirectCompute) | Fast on Windows only |
| NVIDIA Texture Tools 3 | proprietary SDK; licence terms **not verified** | Windows, Linux x64 and aarch64, CUDA; no macOS | Do not depend on it |
| Intel ISPC Texture Compressor | MIT | — | Discontinued by Intel; prefer bc7e |
| xBRZ / ScaleFX | GPL-3.0 / MIT | CPU code or shaders | The ui route |

**Licence policy for the tool:**

- The built-in catalogue lists only models whose weights are CC0, CC-BY, Apache, MIT or
  WTFPL and whose architecture is in core `spandrel`.
- It downloads only from stable URLs (GitHub releases), and verifies the SHA-256 that
  OpenModelDB records.
- A recipe may name any model by name and SHA-256. Naming is not distribution. A model
  outside the catalogue needs the player to supply the file (`--model-file`), and the
  recipe records its licence string so the player can see it.

---

## 6. Costs

### 6.1 Memory per texture

The multipliers are exact arithmetic, using DXT1 at 0.5 bytes per texel, BC7 and DXT3/5
at 1, and A8R8G8B8 at 4, with a full chain at 4/3 of level 0.

| Original | 2x same format | 2x BC7 | 4x same format | 4x BC7 |
|---|---|---|---|---|
| DXT1 | 4x | **8x** | 16x | **32x** |
| DXT3/DXT5 | 4x | 4x | 16x | 16x |
| A8R8G8B8 (swizzled) | 4x | 1x | 16x | 4x |

So a 256x256 DXT1 texture with its chain (43.7 KB) becomes 350 KB at 2x BC7 and 1.40 MB
at 4x BC7. Today the host also keeps a CPU copy of every chain (§1.4 point 6), which
doubles host RAM unless replaced textures drop it.

### 6.2 A TimeSplitters 2 level

**The resident texture bytes are one run away and were not read for this note.** Every
run prints `[HLE-D3D8] shadow texture memory: X MB in N uploaded entries` every five
seconds (`src/hle/hle_d3d8_texture.c:1768`). Measured so far: the whole process uses
65-81 MB of GPU memory in the level (`ts2-resource-use.md`), and one in-level frame
binds 102 textured assets, mostly DXT1 (`modding-models-textures.md`).

As an **assumption**, take T = 10-20 MB of resident textures, DXT1-dominated, on a
console with 64 MB in all:

| Pack | Extra GPU memory | Against today's 65-81 MB |
|---|---|---|
| 2x BC7 | 80-160 MB | 1-2x |
| 4x BC7 | 320-640 MB | 4-8x |
| 4x DXT1 (opaque only) | 160-320 MB | 2-4x |

**So 2x is the default, and 4x is capped per texture.** Allow 4x only where the input is
at most 256 on its long side and the output at most 1024, and keep a pack-wide budget.

The existing `RECOMP_HLE_D3D8_TEX_BUDGET_MB` (256 by default, `:493`) would then evict
replacement entries, and each re-bind would read the file again. Replacement entries
want their own budget, and an in-memory index of the pack loaded at start-up.

The test machine's RTX 3050 Ti has 4 GB, so 4x would fit there. On Iris Xe, the adapter
TimeSplitters 2 actually ran on, memory is shared with the system.

### 6.3 Disk

The pack holds the distinct keys a player has dumped. It is roughly the same multipliers
applied to the dumped originals; LZ shrinks BC7 by only 10-20%, and bc7e's RDO mode
claims 10-50%.

**Estimate:** 3,000 textures with a 128x128 DXT1 average (`chr.pak` alone holds 981
`.xbt`) are about 33 MB of originals. That gives about 260 MB at 2x BC7 and about
1.05 GB at 4x BC7.

### 6.4 Offline processing time

All of this is estimated, per 1,000 textures with an average input of 128-256 px, on an
RTX 3060-class GPU:

| Step | 2x | 4x |
|---|---|---|
| SPAN inference | ~1 min | ~1-2 min |
| SwinIR-M inference (tiled) | ~10 min | ~20-30 min |
| Back-projection, 10 iterations on the GPU through torch | < 1 min | ~1 min |
| BC7 encode with bc7e, high quality, all cores | ~3-5 min | ~10-30 min |
| **Total, SPAN** | **~5-10 min** | **~15-35 min** |

- Apple Silicon over MPS: assume 2-4x slower.
- CPU only: SPAN stays practical (~15-30 min per 1,000 at 2x); SwinIR does not.
- Inference memory: SPAN fits in under 1 GB at 256 px tiles; SwinIR-M wants 128 px
  tiles with overlap on a 4 GB card.

---

## 7. Review tooling and the recipe

`py -3 -m tools.texpack build` writes `report.html` into the pack folder. It is a local
file with relative image links. **It contains game content, so it is never published,
attached or committed**, and neither is the pack.

For each texture, the report shows:

- the original (nearest-upscaled to the output size), the result, and the
  bicubic-plus-back-projection baseline, side by side, with synchronised zoom and pan;
- a heat map of x̂ − bicubic(y), which is where the model added anything;
- the §4.4 numbers and the route, with the signals that chose it ("97% screen-space,
  POINT filter");
- flags: review, failed check, fallback taken, dynamic, mixed usage.

The list filters by route, flag and failure, sorts by A (most added detail first), and
takes accept, reject, use-safe and re-route from the keyboard.

Decisions are saved as `<title>.overrides.json` beside the pack. The command
`texpack recipe merge` folds them into the recipe, and the recipe then carries them to
every player who rebuilds.

### 7.1 Recipe

The recipe holds no pixels, only keys, settings and decisions:

```json
{
  "recipe": 1, "title_id": "4553000A", "key": "xtex1-xxh3-64",
  "pipeline": {
    "cleanup": {"method": "dxt-consistent-tv", "tau": 1.0},
    "sr": {"model": "4x-pix-span-pretrain", "sha256": "...", "licence": "CC-BY-4.0"},
    "scale": 2, "max_out": 1024,
    "bp": {"op": "box", "space": "stored", "iters": 10},
    "output": "bc7", "encoder": {"name": "bc7e", "version": "...", "level": 5}
  },
  "textures": {
    "9f3c...": {"route": "main", "scale": 4, "decision": "accept"},
    "51aa...": {"route": "ui", "decision": "untouched"},
    "c0de...": {"decision": "safe", "note": "face, invented pores"}
  }
}
```

**GPU inference is not bit-exact across GPUs and drivers**, so a rebuilt pack will not
have the same bytes as the recipe author's. The recipe promises the same *decisions*,
and the build checks every texture against §4.4 again. A texture that passed for the
author and fails for a player is reported, not shipped.

### 7.2 The A/B loop with no game running

`d3d8_replay` gains `--tex-pack <dir>`. It computes each captured texture's key from the
`D3D8CAP_TEXTURE` bytes (§1.8) and substitutes the pack's texture. One F11 capture then
replays twice, with and without the pack, and `scripts/` compares the two BMPs (side by
side, plus a diff). This needs no game running and no second capture, and it works for a
player reporting "this wall looks wrong".

---

## 8. Risks specific to Xbox-era content

- **DXT1 1-bit alpha.** In the 3-colour mode the transparent texel decodes as black.
  Unfilled, it becomes a dark fringe after any interpolating upscale. Step 2's push-pull
  fill is mandatory, not polish.
- **P8.** A model cannot work in index space, and the palette is chosen per draw
  (Marvel vs Capcom 2 draws one sheet under several palettes). So P8 goes through the
  ui route: scale over the palette's colours, then map each new texel back to the
  nearest palette index. The replacement **stays P8**, at the larger size, and the
  existing stage-palette expansion (`sync_palette`) keeps palette swaps working.
  Measured: there is no P8 in TimeSplitters 2.
- **One texture, several jobs.** Content drawn as a decal and as a UI element, or two
  ids sharing a hash: take the most conservative route (§2).
- **Textures the title modifies after upload.** Streamed sprite sheets, decoded movies,
  slots reused for new content. `texels_changed()` already notices. Such a key is
  marked `dynamic` and never replaced, and an entry that drifts while replaced reverts
  (§1.4 point 1).
- **Render to texture.** It is never replaced; the `rendered` and `framebuffer` flags
  already identify it. A render target whose memory a later asset reuses is a new key
  and is handled as an asset.
- **Hand-filled push buffers.** Textures bound through `BeginPush` reach none of this
  (`modding-models-textures.md`). Textures named through `SwitchTexture` and the
  push-buffer walk *are* covered: they go through `host_texture()` by synthetic key.
- **Atlases.** Packed sprite sheets and UI atlases bleed across sprite borders under any
  interpolating scaler. The ui route works per pixel and does not interpolate.
  Detecting atlases inside 3D textures is unsolved; that is what review is for.
- **Hitches.** Loading a 1.4 MB BC7 file at first bind, on the main thread, stalls.
  The pack index loads at start-up, and files are read ahead from the manifest's
  per-level co-occurrence (later work).
- **Vendor DXT decode differences** make "exact" a property of our own decoder (point 2
  at the top of this note), not of the hardware.

---

## 9. Staged plan

Days are for someone fluent in this codebase, including tests and the doc. All of them
are estimates. "Toolkit" means it works for any title.

| Stage | Item | Days | Kind | Result |
|---|---|---|---|---|
| 0 | Try `RECOMP_ANISO=16` and `RECOMP_RES_SCALE=2` in TS2; read the `shadow texture memory` line in the level | 0.5 | per title | Says what upscaling is for, and pins down §6.2 |
| 1 | **T4**: 64-bit canonical key, `RECOMP_TEXPACK_DUMP` (DDS by key, chain as shipped), manifest with the §1.3 usage counters | 2-3 | toolkit | Dumps every TS2 texture with its usage |
| 2 | **T5a**: same-format replacement (any size, the guest's format), the six guards of §1.4, replacement budget, pack index loaded at start-up | 2-3 | toolkit | A hand-edited or Lanczos DXT1 pack shows in game |
| 2 | Replay `--tex-pack` and the compare script | 1-1.5 | toolkit | The A/B loop of §7.2 |
| 3 | `tools.texpack` v0: decode, the classifier (pixel and usage rules), padding, alpha split, Lanczos plus back-projection, the DXT1 encode path, the report, the recipe | 4-6 | toolkit | **A faithful pack with no ML at all**, and the fallback for everything after |
| 4 | **T5b**: BC7 through the RHI (D3D11 and Vulkan), a host-only `D3DFORMAT`, the capture `TEXTURE_REF` chunk, dropping `sys_mem` for replaced textures | 1.5-2 | toolkit | BC7 packs |
| 4 | texpack v1: spandrel runner with tiling, the model catalogue with SHA-256s, back-projection on the GPU, DXT-consistent cleanup, vendored bc7e | 3-4 | toolkit | The smooth-model pipeline |
| 5 | Review viewer keys, overrides and `recipe merge`; `texpack build --recipe` checking every texture again | 2-3 | toolkit | Shareable recipes |
| 6 | **T7**: replacement on static cubes (`static_cube()`), P8 in index space, volume textures in the mirror (`read_layout()` `:244`) | 3-5 | toolkit | Coverage |
| opt | Own L1 SPAN models (1x cleanup, 2x and 4x), trained on CC0 textures | 3-5 | toolkit | Models whose licence and loss are both known |
| — | TS2 recipe: calibrate the §4.4 thresholds, review the flagged textures (~5-10 s each), set the 4x list | 1-3 | **per title** | The first shared recipe |

**Toolkit total, stages 1-5: about 16-23 days**, with T7 and the trained models on top.
The per-title cost is review time, and the §2 rules exist to keep it small.

The smallest end-to-end result is stages 1, 2 and 3: about 9-13 days. It gives a pack
that is sharper when magnified and contains nothing the artist did not paint.

---

## What was verified, and what was not

**Read in the code (this worktree):**

- every file:line above;
- that `RECOMP_TEX_DUMP` is in use;
- that the checksum covers the full chain and is folded to 32 bits;
- that the RHI has no BC7 and no sRGB formats;
- that static cubes and P8 are mirrored and volume textures are not;
- that `tex_scale` uses the host size;
- that MIPFILTER NONE maps to point mip filtering with an unbounded LOD;
- that `MAXMIPLEVEL` and `MIPMAPLODBIAS` are forwarded but not applied;
- that the fork is GPL-3.0.

**Read from primary sources:**

- every model's licence, host and size (OpenModelDB data repository, 14 Aug 2026);
- the stated losses of the SPAN pretrains and the classical research models;
- the `spandrel` 0.4.2 licence and the core versus `spandrel_extra_arches` split (PyPI);
- the licence files of bc7enc_rdo, basis_universal, Compressonator, DirectXTex, ISPC TC
  and chaiNNer, and the licence headers of xBRZ and ScaleFX (their repositories);
- that BC formats are always supported on macOS (Apple developer forum).

**Not verified:**

- the training loss of every community model whose author does not state one;
- the NVTT 3 licence terms;
- BC7 through MoltenVK;
- SPAN and SwinIR on PyTorch MPS;
- every timing in §6.4;
- TimeSplitters 2's resident texture bytes and its distinct texture count;
- which mip filter the XDK-era tools used;
- that DXT-consistent projection has been done before for BCn.

Every TS2 number above is either quoted from another note or marked as an assumption.
