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
 * The five functions below are the whole API, and the only atomic_*
 * symbols the core exports (see link.T). The bridge calls them under
 * its core mutex, after retro_run, on the emulation thread.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AtomicScanlineMeta;

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

#ifdef __cplusplus
}
#endif

#endif
