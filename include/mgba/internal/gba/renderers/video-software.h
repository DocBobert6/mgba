/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef VIDEO_SOFTWARE_H
#define VIDEO_SOFTWARE_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/core.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/renderers/common.h>
#include <mgba/internal/gba/video.h>

struct GBAVideoSoftwareBackground {
	unsigned index;
	int enabled;
	unsigned priority;
	uint32_t charBase;
	int mosaic;
	int multipalette;
	uint32_t screenBase;
	int overflow;
	int size;
	int target1;
	int target2;
	uint16_t x;
	uint16_t y;
	int32_t refx;
	int32_t refy;
	int16_t dx;
	int16_t dmx;
	int16_t dy;
	int16_t dmy;
	int32_t sx;
	int32_t sy;
	int yCache;
	uint16_t mapCache[64];
	uint32_t flags;
	uint32_t objwinFlags;
	int objwinForceEnable;
	bool objwinOnly;
	bool variant;
	int32_t offsetX;
	int32_t offsetY;
	bool highlight;
};

enum {
	OFFSET_PRIORITY = 30,
	OFFSET_INDEX = 28,
};

#define FLAG_PRIORITY       0xC0000000
#define FLAG_INDEX          0x30000000
#define FLAG_IS_BACKGROUND  0x08000000
#define FLAG_UNWRITTEN      0xFC000000
#define FLAG_REBLEND        0x04000000
#define FLAG_TARGET_1       0x02000000
#define FLAG_TARGET_2       0x01000000
#define FLAG_OBJWIN         0x01000000
#define FLAG_ORDER_MASK     0xF8000000

/* Renderer id, as returned by GBAVideoSoftwareRendererId. */
#define SOFTWARE_MAGIC 0x6E727773

#define IS_WRITABLE(PIXEL) ((PIXEL) & 0xFE000000)

struct WindowControl {
	GBAWindowControl packed;
	int8_t priority;
};

#define MAX_WINDOW 5

struct Window {
	uint8_t endX;
	struct WindowControl control;
};

/* Atomic per-layer capture (Phase 2.1). See atomic-layers.h for the
   buffer layout, flag-byte format and per-scanline metadata. */
#include <mgba/internal/gba/renderers/atomic-layers.h>
/* Atomic tile deduplication (Phase 2.2). See atomic-tiles.h for the
   grid layout, key packing and version scheme. */
#include <mgba/internal/gba/renderers/atomic-tiles.h>

struct GBAVideoSoftwareRenderer {
	struct GBAVideoRenderer d;

	mColor* outputBuffer;
	int outputBufferStride;

	uint32_t* temporaryBuffer;

	GBARegisterDISPCNT dispcnt;

	uint32_t row[GBA_VIDEO_HORIZONTAL_PIXELS];
	uint32_t spriteLayer[GBA_VIDEO_HORIZONTAL_PIXELS];
	int32_t spriteCyclesRemaining;

	// BLDCNT
	unsigned target1Obj;
	unsigned target1Bd;
	unsigned target2Obj;
	unsigned target2Bd;
	bool blendDirty;
	enum GBAVideoBlendEffect blendEffect;
	mColor normalPalette[512];
	mColor variantPalette[512];
	mColor highlightPalette[512];
	mColor highlightVariantPalette[512];

	uint16_t blda;
	uint16_t bldb;
	uint16_t bldy;

	GBAMosaicControl mosaic;
	bool stereo;

	struct WindowN {
		struct GBAVideoWindowRegion h;
		struct GBAVideoWindowRegion v;
		struct WindowControl control;
		int16_t offsetX;
		int16_t offsetY;
		bool on;
	} winN[2];

	struct WindowControl winout;
	struct WindowControl objwin;

	struct WindowControl currentWindow;

	int nWindows;
	struct Window windows[MAX_WINDOW];

	struct GBAVideoSoftwareBackground bg[4];

	bool forceTarget1;
	bool oamDirty;
	int oamMax;
	struct GBAVideoRendererSprite sprites[128];
	int16_t objOffsetX;
	int16_t objOffsetY;

	uint32_t scanlineDirty[5];
	uint16_t nextIo[GBA_REG(SOUND1CNT_LO)];
	struct ScanlineCache {
		uint16_t io[GBA_REG(SOUND1CNT_LO)];
		int32_t scale[2][2];
		bool windowOn[2];
	} cache[GBA_VIDEO_VERTICAL_PIXELS];
	int nextY;

	int start;
	int end;

	uint8_t lastHighlightAmount;

	/* Atomic per-layer capture (Phase 2.1). Layer 0-3 = BG0-3,
	   layer 4 = OBJ. Colours are pre-blend RGB565; flags are one byte
	   per pixel (ATOMIC_FLAG_*). Allocated in Init, freed in Deinit;
	   NULL until then. Capture is off by default (atomicLayersEnabled)
	   and costs nothing while off. */
	mColor* atomicLayers[ATOMIC_LAYER_COUNT];
	uint8_t* atomicLayerFlags[ATOMIC_LAYER_COUNT];
	struct AtomicScanlineMeta* atomicMeta;
	int atomicScanline;
	bool atomicLayersEnabled;
	/* In-progress OBJ row, mirroring spriteLayer: the pre-blend colour
	   and flag byte of the winning sprite pixel per x. Copied into
	   layer 4 at the end of each drawn scanline. */
	mColor atomicObjRawColor[GBA_VIDEO_HORIZONTAL_PIXELS];
	uint8_t atomicObjRawFlags[GBA_VIDEO_HORIZONTAL_PIXELS];
	/* In-progress OBJWIN mask (30 bytes, bit x of byte x>>3), taken
	   from row[] BEFORE PostprocessBuffer: passes (a)/(b) replace
	   row entries with bare colours and drop the FLAG_OBJWIN bits,
	   so reading the mask after post-processing would lose pixels
	   the blend/fade passes touched. */
	uint8_t atomicObjwinMask[30];
	/* Atomic tile deduplication (Phase 2.2). Per-scanline tile-key
	   grids for BG0-3 (see atomic-tiles.h): one u32 key per
	   (scanline, lattice column), columns stored at index col+1.
	   Allocated in AtomicAlloc, freed in AtomicFree; a row is
	   cleared in AtomicClearScanline and filled by the mode-0 draw
	   as it fetches tiles. A NULL grid (allocation failed) means
	   the tiles API reports unavailable and the frontend falls back
	   to the full-screen MMPX pass. */
	uint32_t* atomicTileGrid[ATOMIC_TILE_GRID_LAYERS];
	/* Invalidation generations (see atomic-tiles.h): per-palette
	   (32: 16 BG + 16 OBJ) and per-32-byte-VRAM-slot (3072), bumped
	   in WritePalette / WriteVRAM. The epoch bumps on every renderer
	   reset (including state load). The grid-overflow flag is set by
	   the tile record if a tile's lattice column ever falls outside
	   [-1, 30] (proven impossible for a drawn tile; the flag is
	   defence in depth — the tiles getter reports it and clears it,
	   and the frontend drops dedup for that frame). */
	uint32_t atomicPaletteGen[ATOMIC_TILE_PALETTE_GENS];
	uint32_t atomicVramGen[ATOMIC_TILE_VRAM_SLOTS];
	uint32_t atomicEpoch;
	bool atomicTileGridOverflow;
	/* Atomic affine supersampling (Phase 2.5). Per-scanline palette
	   history: one 512-entry mColor row per scanline (160 KB),
	   snapshotted in AtomicSnapshotScanline from normalPalette
	   (raw, never variant/highlight — the colour-corrected copy
	   lives in libretro-atomic.c, mirroring the layer planes).
	   Allocated in AtomicAlloc, freed in AtomicFree; a NULL
	   history means the affine API reports unavailable and the
	   frontend falls back to the MMPX pass. The single VRAM
	   version counter bumps on every VRAM write (alongside the
	   per-slot gens); the frontend re-uploads its VRAM texture
	   when it (or the epoch) changes. */
	mColor* atomicPaletteHistory;
	uint32_t atomicVramVersion;
};

void GBAVideoSoftwareRendererCreate(struct GBAVideoSoftwareRenderer* renderer);

CXX_GUARD_START

#endif
