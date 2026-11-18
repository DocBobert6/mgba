/* Copyright (c) 2026 Atomic contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef LIBRETRO_ATOMIC_H
#define LIBRETRO_ATOMIC_H

/* Atomic per-layer framebuffers (Phase 2.1).
 *
 * The software renderer records every background/sprite pixel BEFORE
 * blending (see include/mgba/internal/gba/renderers/atomic-layers.h
 * for the buffer layout, flag-byte format and per-scanline metadata).
 * This header is deliberately standalone (stdint/stddef only): the
 * Atomic bridge includes it with only src/platform/libretro on its
 * include path, so it must not pull in any mgba-internal header. The
 * metadata struct is forward-declared here; its layout lives in
 * atomic-layers.h and its size is available at runtime through
 * atomic_layers_metadata_size().
 *
 * The functions below are the whole API, and the only atomic_*
 * symbols the core exports (see link.T). The bridge calls them under
 * its core mutex, after retro_run, on the emulation thread.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AtomicScanlineMeta;

/* Tile deduplication (Phase 2.2). The software renderer records the
   identity of every text-mode background tile it fetches into a
   per-scanline key grid (see atomic-tiles.h for the layout, the key
   packing and the version scheme), and bumps per-palette / per-VRAM-
   slot generations on palette / VRAM writes plus a global epoch on
   renderer reset (including state load). The frontend reads the grid
   and the versions once per frame (after retro_run, under its core
   mutex) and builds its atlas keys from them. A key is
   hash(grid keys || 12x12 coverage mask || palette versions ||
   VRAM versions || epoch); the grid, the flag planes and the
   versions together determine every texel of every 12x12 MMPX patch,
   so a matching key means an identical 16x16 upscaled block. */

/* Grid geometry, kept in sync with atomic-tiles.h by the host harness
   (which compiles both headers and asserts equality). */
#define ATOMIC_TILES_LAYERS 4
#define ATOMIC_TILES_ROWS 160
#define ATOMIC_TILES_COLS 32
#define ATOMIC_TILES_CELLS \
	(ATOMIC_TILES_LAYERS * ATOMIC_TILES_ROWS * ATOMIC_TILES_COLS)
#define ATOMIC_TILES_PALETTE_GENS 32
#define ATOMIC_TILES_VRAM_SLOTS 3072

/* 1 when the tile grids are allocated (i.e. the layer capture is
   allocated). 0 before a game is loaded or on any other renderer.
   Frontends fall back to the full-screen MMPX pass when 0. */
int atomic_tiles_available(void);

/* Copy the 4 BG tile-key grids (ATOMIC_TILES_CELLS u32 keys, layer 0-3,
   row-major: layer, scanline, column+1) into the caller's array, which
   must hold atomic_tiles_grid_size() bytes. Returns 1 on success, 0
   when tiles are unavailable. */
int atomic_tiles_get_grid(uint32_t* out);

/* Copy the invalidation state into the caller's arrays: the 32
   per-palette generations, the 3072 per-VRAM-slot generations, the
   global epoch, and a flags word (bit 0: a tile's lattice column fell
   outside [-1, 30] since the last read — proven impossible for a
   drawn tile; the frontend must drop dedup for the frame if set).
   The flags word is cleared on read. Returns 1 on success, 0 when
   tiles are unavailable. */
int atomic_tiles_get_versions(uint32_t* outPaletteGen,
                              uint32_t* outVramGen,
                              uint32_t* outEpoch,
                              uint32_t* outFlags);

/* Size in bytes of the tile-key grids (81920). Frontends size their
   grid buffer from this, never from a hardcoded constant. */
size_t atomic_tiles_grid_size(void);

/* Layer count / geometry. Kept in sync with atomic-layers.h by the
   host harness (which compiles both headers and asserts equality). */
#define ATOMIC_LAYERS_COUNT 5
#define ATOMIC_LAYERS_WIDTH 240
#define ATOMIC_LAYERS_HEIGHT 160
#define ATOMIC_LAYERS_PIXELS (ATOMIC_LAYERS_WIDTH * ATOMIC_LAYERS_HEIGHT)

/* 1 when this core build captures per-layer framebuffers (i.e. the
   loaded game's renderer is the software renderer with its layer
   buffers allocated). 0 before a game is loaded or on any other
   renderer. Frontends fall back to the composited frame when 0. */
int atomic_layers_available(void);

/* Enable or disable capture. Capture defaults OFF and costs nothing
   while off. Enabling marks every scanline dirty, so the next frame
   fully redraws and the layer buffers are complete from the next
   frame on (a static scene would otherwise leave stale rows). */
void atomic_layers_set_enabled(int enabled);
int atomic_layers_get_enabled(void);

/* Copy the 5 layer colour planes (RGB565, 240x160 each) and the 5
   flag planes (1 byte/px) into the caller's arrays. outColors[i] and
   outFlags[i] must point at ATOMIC_LAYERS_PIXELS entries each.
   Returns 1 on success, 0 when layers are unavailable.
   When the core's colour correction is enabled, the returned colours
   are the colour-corrected ones (same LUT as the composited frame);
   otherwise they are the raw captured colours. */
int atomic_layers_get_pixels(uint16_t* outColors[ATOMIC_LAYERS_COUNT],
                             uint8_t* outFlags[ATOMIC_LAYERS_COUNT]);

/* Copy the 160 per-scanline metadata structs (in scanline order) into
   the caller's buffer, which must hold 160 * the size returned by
   atomic_layers_metadata_size() bytes. Returns 1 on success, 0 when
   layers are unavailable. When the core's colour correction is
   enabled, the backdrop field of each struct is the colour-corrected
   backdrop (same LUT as the composited frame and the layer planes) —
   the backdrop is not a layer, so without this it would be the one
   colour the frontend composites uncorrected. The backdrop is never
   blend-faded here; the frontend re-applies the fade from
   blendEffect/bldy, exactly as for the layers. */
int atomic_layers_get_metadata(struct AtomicScanlineMeta* out);

/* Size in bytes of one AtomicScanlineMeta (144). Frontends size their
   metadata buffer from this, never from a hardcoded constant. */
size_t atomic_layers_metadata_size(void);

/* Colour-correction hook, called from libretro.c's retro_run after the
   frame's post-processing. When lut is non-NULL, the layer planes are
   run through the same LUT as the composited frame into a second set
   of corrected planes (never in place: non-redrawn scanlines would
   otherwise be corrected twice); get_pixels then returns the corrected
   planes. When lut is NULL, the corrected planes are marked invalid
   and get_pixels returns the raw planes. */
void atomic_layers_apply_color_correction(const uint16_t* lut);
void atomic_layers_clear_color_correction(void);

/* Affine supersampling (Phase 2.5). The software renderer records,
   per scanline, the BG2CNT/BG3CNT map geometry (bgCnt[2] in the
   metadata) and the full 512-entry palette (one row per scanline),
   and the frontend reads the game's VRAM (96 KB) through
   atomic_affine_get_vram. The GPU affine rasteriser evaluates the
   hardware's own affine transform per output pixel from those three
   inputs; the 1x layer planes stay the coverage authority (see the
   coverage rule in the Phase 2.5 plan). The versions word gates the
   frontend's texture uploads: the VRAM texture is re-uploaded when
   the VRAM version (or the epoch, which bumps on renderer reset
   including state load) changes; the palette texture is
   re-uploaded when any of the 32 per-palette generations changes. */

/* Geometry, kept in sync with the renderer by the host harness. */
#define ATOMIC_AFFINE_VRAM_SIZE 98304
#define ATOMIC_AFFINE_PALETTE_ROWS 160
#define ATOMIC_AFFINE_PALETTE_COLS 512
#define ATOMIC_AFFINE_PALETTE_GENS 32
/* Versions word: vramVersion + epoch + 32 per-palette generations. */
#define ATOMIC_AFFINE_VERSIONS_COUNT 34

/* 1 when the affine data is available (i.e. the layer capture is
   allocated and the palette history is allocated). 0 before a game
   is loaded or on any other renderer. Frontends fall back to the
   MMPX pass when 0. */
int atomic_affine_available(void);

/* Copy the game's VRAM (ATOMIC_AFFINE_VRAM_SIZE bytes) into the
   caller's buffer and write the single VRAM version counter into
   *outVersion (bumped on every VRAM write). Returns 1 on success,
   0 when affine data is unavailable. */
int atomic_affine_get_vram(uint8_t* out, uint32_t* outVersion);

/* Copy the per-scanline palette history (ATOMIC_AFFINE_PALETTE_ROWS
   x ATOMIC_AFFINE_PALETTE_COLS mColor, row-major: scanline, palette
   index) into the caller's buffer, which must hold
   atomic_affine_palette_history_size() bytes. Returns 1 on success,
   0 when affine data is unavailable. When the core's colour
   correction is enabled, the returned colours are the
   colour-corrected ones (same LUT as the composited frame and the
   layer planes); otherwise they are the raw captured colours. */
int atomic_layers_get_palette_history(uint16_t* out);

/* Copy the invalidation state into the caller's array, which must
   hold atomic_affine_versions_size() bytes (ATOMIC_AFFINE_VERSIONS_
   COUNT u32): [0] the VRAM version, [1] the global epoch, [2..33]
   the 32 per-palette generations. Returns 1 on success, 0 when
   affine data is unavailable. */
int atomic_affine_get_versions(uint32_t* out);

/* Sizes in bytes of the VRAM block (98304), the palette history
   (163840) and the versions word (136). Frontends size their
   buffers from these, never from hardcoded constants. */
size_t atomic_affine_vram_size(void);
size_t atomic_affine_palette_history_size(void);
size_t atomic_affine_versions_size(void);

#ifdef __cplusplus
}
#endif

#endif
