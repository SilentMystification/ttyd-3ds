# Notes from Star3D (Star Fox Adventures on New 3DS)

Source: https://github.com/EzequielLuppino/Star3D, studied 2026-11-22 (shallow clone, read only). **The repository has no licence, so we use ideas only and copy no code.** File references are relative to its `3DS/` folder. Some of its own plan documents are older than its code; where they disagree, these notes follow the code.

## The ideas that matter most for us, ranked

1. **Display-list decode cache with a trust window** (`gx/gx_dlcache.c`).
   - Each GX display list is decoded once into PICA-ready vertices and u16 indices in linear memory, and replayed after that.
   - **Cache key:** list address, size, a sampled hash of the list (32 words), and a hash of the vertex state (descriptors, formats, the current PN matrix, array sizes and strides). The POS and NRM array *pointers* are left out of the key, because the game double-buffers animated models. Also in the key: the texgen source per texture unit, and sampled hashes of the arrays, memoised per frame.
   - **Dynamic lists:** after 4 re-records with the same key, a list is treated as dynamic and runs uncached.
   - **Trust window:** an entry that validated once skips hashing for 512 frames, then gets a full re-check.
   - **Animated meshes:** a "gather" mode re-reads only positions and normals.
   - **Budget:** 2048 slots and 12 MB of linear memory, plus a guarded heap for side tables.
   - **Packed vertices:** 36 bytes, with s16 positions and texcoords scaled per entry (lossless), u8 colour, a u8 matrix index and s16 normals. On hardware this was 17% less GPU vertex time than floats.
   - Lines and points are expanded to quads on the CPU, and lists that use them aren't cached.
2. **Transform in the vertex shader** (`gx/default.v.pica`).
   - A `posmtx[30]` palette: 10 GX matrices × 3 rows, picked by a u8 index per vertex.
   - A pre-multiplied projection, and `posscale` for the fixed-point scales.
   - Normals use the 3×3 of the position matrix.
   - Lighting runs in the vertex shader (7 lights, material and ambient source selection).
   - Specular (colour channel 1) runs through PICA fragment lighting. Fog is a fragment-lighting trick: a light at the eye whose distance curve is the fog curve.
   - Texgen runs in the shader.
   - The shader uses 95 of the 96 float uniforms.
   - **Hardware rules:** vertex-shader bool b15 doesn't work, and each output register must be written exactly once and in full, or the GPU hangs.
   - **Costs on hardware:** about 0.027 µs per input attribute per vertex, regardless of size, so attributes that don't change become fixed attributes. About 0.1 µs per light per vertex. The post-transform cache is real: a reuse of 3 costs a third per index.
3. **Textures** (`gx/gx_tex.c`, `tools/tex_convert.py`).
   - **When to check for changes:** once per *generation* (a counter bumped by `GXInvalidateTexAll`), not once per bind.
   - **How:** a 16-byte header hash plus an 8-word sampled fingerprint, and a 128-entry memo of recent binds.
   - **Build-time conversion** to PICA formats (ETC1, RGB565, LA8 and so on) with mipmaps. `TEX_SCALE` halves every texture at build time, which cuts sampled texels to a quarter.
4. **APT hook in the GPU module** (`gx/gx_rec.c:937-969`). On suspend, sleep and exit it waits for the render thread to go idle and **ends the open GPU frame**. Their note: a frame left open across a suspend never completes afterwards, and the next wait hangs. This is a direct lead for our sleep hang.
5. **State-delta recording** (`gx/gx_rec_delta.c`). The game thread keeps a shadow copy of the GX state and sends one command with only the changed fields per draw, plus batched matrix loads and a record-time filter for redundant commands. A verify mode records both forms and compares them.

## Core 1 measurements (`port/worker.c:62-77`, New 3DS XL)

Giving the app *any* share of core 1 slows the system services that run there (GSP, DSP, FS):

| Core-1 limit for the app | Hub frame time | `GSPGPU_FlushDataCache` per call |
|---|---|---|
| 0% | 57.3 ms | 0.13 ms |
| 30% | 60.0 ms | 0.25 ms |
| 80% | 74.0 ms | 0.97 ms |

They default to 0% and run their worker and audio threads on core 2. **We run at 80%.** This needs measuring on our console with the `cpulimit` switch.

## Other notes

- **Cores:**
  - **Core 2** needs no special flag beyond extended mode: a plain `threadCreate(..., 2)`.
  - **Core 3:** their network thread *tries* core 3 and falls back to core 2. Nothing confirms it ever ran on core 3, and their own docs say core 3 belongs to the system.
- **Audio:** one MusyX voice per NDSP channel (21 voices), with the ADSR envelope and mix done in software every NDSP frame. Loops are re-aligned to 14-sample DSP-ADPCM frames at build time. No CPU mixing, but no reverb either.
- **Their ExHeader:**
  - `MaxCpu 0x9E` (multi mode, 30% maximum);
  - `SystemModeExt 178MB`, `CpuSpeed 804MHz`, L2 cache on;
  - VRAM mapped read-only, and **no QTM mapping**.
- **Tools:**
  - **Profiling:** `PROFILE=1` zone timers per thread, and live `S3D_*_AB` switches that alternate a feature every N frames and log per-arm frame times.
  - **Network:** an HTTP server for logs and settings, `PUT /app.3dsx`, and a restart through Rosalina's `hb:ldr` `HBLDR_SetTarget`. There's no CIA update path; that's what our `ctr-devkit` adds.
- **Robustness:**
  - a guarded heap with head and tail magic words;
  - adaptive frame pacing (no VBlank wait after a long frame);
  - a self-test of hardware quirks at boot;
  - unaligned RGB565 texture bases fail on hardware.
- **Exit order:**
  1. stop the network;
  2. stop the watchdog;
  3. stop the render thread;
  4. end the open frame;
  5. shut down citro3d (it panics if render targets are deleted inside a frame);
  6. close the log;
  7. `gfxExit`.
