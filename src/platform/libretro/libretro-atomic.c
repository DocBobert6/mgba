/* Copyright (c) 2026 Atomic contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "libretro-atomic.h"

#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/renderers/video-software.h>

#include <string.h>

/* libretro.c owns the core pointer; this accessor is internal to the
   .so (its name matches neither retro_* nor atomic_*, so the version
   script keeps it local). */
extern struct mCore* libretroCoreInstance(void);

/* Compile-time guards: the API's geometry must match the renderer's,
   and the metadata struct must have exactly the documented size (the
   bridge and the Kotlin side read fields at fixed offsets). */
typedef char atomic_layers_count_check[ATOMIC_LAYERS_COUNT == ATOMIC_LAYER_COUNT ? 1 : -1];
typedef char atomic_layers_width_check[ATOMIC_LAYERS_WIDTH == ATOMIC_LAYER_WIDTH ? 1 : -1];
typedef char atomic_layers_height_check[ATOMIC_LAYERS_HEIGHT == ATOMIC_LAYER_HEIGHT ? 1 : -1];
typedef char atomic_layers_pixels_check[ATOMIC_LAYERS_PIXELS == ATOMIC_LAYER_PIXELS ? 1 : -1];
typedef char atomic_meta_size_check[sizeof(struct AtomicScanlineMeta) == ATOMIC_META_SIZE ? 1 : -1];
typedef char atomic_tiles_layers_check[ATOMIC_TILES_LAYERS == ATOMIC_TILE_GRID_LAYERS ? 1 : -1];
typedef char atomic_tiles_rows_check[ATOMIC_TILES_ROWS == ATOMIC_TILE_GRID_ROWS ? 1 : -1];
typedef char atomic_tiles_cols_check[ATOMIC_TILES_COLS == ATOMIC_TILE_GRID_COLS ? 1 : -1];
typedef char atomic_tiles_cells_check[ATOMIC_TILES_CELLS == ATOMIC_TILE_GRID_CELLS ? 1 : -1];
typedef char atomic_tiles_palette_check[ATOMIC_TILES_PALETTE_GENS == ATOMIC_TILE_PALETTE_GENS ? 1 : -1];
typedef char atomic_tiles_vram_check[ATOMIC_TILES_VRAM_SLOTS == ATOMIC_TILE_VRAM_SLOTS ? 1 : -1];

/* Second set of layer planes holding the colour-corrected colours.
   Written by atomic_layers_apply_color_correction() once per frame
   (never in place), read by atomic_layers_get_pixels() when valid. */
static mColor atomicLayersCc[ATOMIC_LAYER_COUNT][ATOMIC_LAYER_PIXELS];
static bool atomicCcValid = false;
/* Colour-corrected backdrops, one per scanline. The backdrop is not a
   layer, so the layer planes above never carry it — but the composited
   frame's backdrop IS colour-corrected (it is part of outputBuffer),
   so the metadata's backdrop must be corrected too, or the frontend
   would composite an uncorrected backdrop under corrected layers.
   Written by the same hook, substituted by get_metadata when valid. */
static mColor atomicBackdropCc[GBA_VIDEO_VERTICAL_PIXELS];

/* The libretro core only associates the software renderer on the first
   retro_run (deferred setup -> core->reset), so a set_enabled() call
   before the first frame would find no renderer. Remember the request
   here and apply it the moment the renderer appears. */
static bool atomicLayersRequested = false;

static struct GBAVideoSoftwareRenderer* _atomicRenderer(void) {
	struct mCore* core = libretroCoreInstance();
	struct GBA* gba;
	struct GBAVideoSoftwareRenderer* renderer;
	if (!core || core->platform(core) != mPLATFORM_GBA) {
		return NULL;
	}
	gba = core->board;
	if (!gba || !gba->video.renderer) {
		return NULL;
	}
	if (gba->video.renderer->rendererId(gba->video.renderer) != SOFTWARE_MAGIC) {
		return NULL;
	}
	renderer = (struct GBAVideoSoftwareRenderer*) gba->video.renderer;
	/* Apply a pending enable request (see above). */
	if (atomicLayersRequested && !renderer->atomicLayersEnabled) {
		renderer->atomicLayersEnabled = true;
		memset(renderer->scanlineDirty, 0xFFFFFFFF, sizeof(renderer->scanlineDirty));
	}
	return renderer;
}

static bool _atomicLayersAllocated(struct GBAVideoSoftwareRenderer* renderer) {
	int i;
	if (!renderer || !renderer->atomicMeta) {
		return false;
	}
	for (i = 0; i < ATOMIC_LAYER_COUNT; ++i) {
		if (!renderer->atomicLayers[i] || !renderer->atomicLayerFlags[i]) {
			return false;
		}
	}
	return true;
}

static bool _atomicTilesAllocated(struct GBAVideoSoftwareRenderer* renderer) {
	int i;
	if (!_atomicLayersAllocated(renderer)) {
		return false;
	}
	for (i = 0; i < ATOMIC_TILE_GRID_LAYERS; ++i) {
		if (!renderer->atomicTileGrid[i]) {
			return false;
		}
	}
	return true;
}

int atomic_tiles_available(void) {
	return _atomicTilesAllocated(_atomicRenderer()) ? 1 : 0;
}

int atomic_tiles_get_grid(uint32_t* out) {
	struct GBAVideoSoftwareRenderer* renderer = _atomicRenderer();
	int i;
	if (!_atomicTilesAllocated(renderer) || !out) {
		return 0;
	}
	for (i = 0; i < ATOMIC_TILE_GRID_LAYERS; ++i) {
		memcpy(&out[(size_t) i * ATOMIC_TILE_GRID_ROWS * ATOMIC_TILE_GRID_COLS],
		       renderer->atomicTileGrid[i],
		       ATOMIC_TILE_GRID_ROWS * ATOMIC_TILE_GRID_COLS * sizeof(uint32_t));
	}
	return 1;
}

int atomic_tiles_get_versions(uint32_t* outPaletteGen,
                              uint32_t* outVramGen,
                              uint32_t* outEpoch,
                              uint32_t* outFlags) {
	struct GBAVideoSoftwareRenderer* renderer = _atomicRenderer();
	if (!_atomicTilesAllocated(renderer) || !outPaletteGen || !outVramGen || !outEpoch || !outFlags) {
		return 0;
	}
	memcpy(outPaletteGen, renderer->atomicPaletteGen, sizeof(renderer->atomicPaletteGen));
	memcpy(outVramGen, renderer->atomicVramGen, sizeof(renderer->atomicVramGen));
	*outEpoch = renderer->atomicEpoch;
	*outFlags = renderer->atomicTileGridOverflow ? 1u : 0u;
	renderer->atomicTileGridOverflow = false;
	return 1;
}

size_t atomic_tiles_grid_size(void) {
	return ATOMIC_TILE_GRID_CELLS * sizeof(uint32_t);
}

int atomic_layers_available(void) {
	return _atomicLayersAllocated(_atomicRenderer()) ? 1 : 0;
}

void atomic_layers_set_enabled(int enabled) {
	struct GBAVideoSoftwareRenderer* renderer;
	atomicLayersRequested = enabled ? true : false;
	renderer = _atomicRenderer();
	if (!renderer) {
		/* No software renderer yet (before the first retro_run);
		   the request is applied when it appears. */
		return;
	}
	if (renderer->atomicLayersEnabled != atomicLayersRequested) {
		renderer->atomicLayersEnabled = atomicLayersRequested;
		if (atomicLayersRequested) {
			/* Force a full redraw: scanlines that are not dirty would
			   otherwise keep stale (or empty) layer rows indefinitely
			   on a static scene, while the composited frame is fine. */
			memset(renderer->scanlineDirty, 0xFFFFFFFF, sizeof(renderer->scanlineDirty));
		}
	}
}

int atomic_layers_get_enabled(void) {
	struct GBAVideoSoftwareRenderer* renderer = _atomicRenderer();
	if (!renderer) {
		return atomicLayersRequested ? 1 : 0;
	}
	return renderer->atomicLayersEnabled ? 1 : 0;
}

int atomic_layers_get_pixels(uint16_t* outColors[ATOMIC_LAYERS_COUNT],
                             uint8_t* outFlags[ATOMIC_LAYERS_COUNT]) {
	struct GBAVideoSoftwareRenderer* renderer = _atomicRenderer();
	int i;
	if (!_atomicLayersAllocated(renderer)) {
		return 0;
	}
	for (i = 0; i < ATOMIC_LAYER_COUNT; ++i) {
		if (!outColors[i] || !outFlags[i]) {
			return 0;
		}
		if (atomicCcValid) {
			memcpy(outColors[i], atomicLayersCc[i], ATOMIC_LAYER_PIXELS * sizeof(mColor));
		} else {
			memcpy(outColors[i], renderer->atomicLayers[i], ATOMIC_LAYER_PIXELS * sizeof(mColor));
		}
		memcpy(outFlags[i], renderer->atomicLayerFlags[i], ATOMIC_LAYER_PIXELS * sizeof(uint8_t));
	}
	return 1;
}

int atomic_layers_get_metadata(struct AtomicScanlineMeta* out) {
	struct GBAVideoSoftwareRenderer* renderer = _atomicRenderer();
	int y;
	if (!_atomicLayersAllocated(renderer) || !out) {
		return 0;
	}
	memcpy(out, renderer->atomicMeta, GBA_VIDEO_VERTICAL_PIXELS * sizeof(struct AtomicScanlineMeta));
	if (atomicCcValid) {
		/* The backdrop travels in the metadata, not in a layer, so
		   it needs the same correction the layer planes got (see the
		   atomicBackdropCc comment above). The blend fade is NOT
		   applied here — the frontend re-applies it from
		   blendEffect/bldy, exactly as it does for the layers. */
		for (y = 0; y < GBA_VIDEO_VERTICAL_PIXELS; ++y) {
			out[y].backdrop = atomicBackdropCc[y];
		}
	}
	return 1;
}

size_t atomic_layers_metadata_size(void) {
	return sizeof(struct AtomicScanlineMeta);
}

void atomic_layers_apply_color_correction(const uint16_t* lut) {
	struct GBAVideoSoftwareRenderer* renderer = _atomicRenderer();
	int i;
	size_t p;
	int y;
	if (!lut || !_atomicLayersAllocated(renderer)) {
		atomicCcValid = false;
		return;
	}
	for (i = 0; i < ATOMIC_LAYER_COUNT; ++i) {
		for (p = 0; p < ATOMIC_LAYER_PIXELS; ++p) {
			atomicLayersCc[i][p] = lut[renderer->atomicLayers[i][p]];
		}
	}
	for (y = 0; y < GBA_VIDEO_VERTICAL_PIXELS; ++y) {
		atomicBackdropCc[y] = lut[renderer->atomicMeta[y].backdrop];
	}
	atomicCcValid = true;
}

void atomic_layers_clear_color_correction(void) {
	atomicCcValid = false;
}
