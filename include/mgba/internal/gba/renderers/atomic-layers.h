/* Copyright (c) 2026 Atomic contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ATOMIC_LAYERS_H
#define ATOMIC_LAYERS_H

#include <mgba-util/common.h>

CXX_GUARD_START

/* Atomic per-layer capture (Phase 2.1): the software renderer records
 * every background/sprite pixel BEFORE blending, so a frontend can
 * upscale each layer separately and re-apply the GBA composite rules
 * (priority, BLDCNT alpha blend, BLDY brighten/darken) at output
 * resolution instead of upscaling an already-blended 240x160 frame.
 *
 * Layer order: 0-3 = BG0-BG3, 4 = OBJ (sprites).
 *
 * Colour planes: ATOMIC_LAYER_PIXELS mColor (RGB565) values per layer,
 * packed 240x160, row-major, no stride. A colour is the PRE-BLEND
 * colour: it has never passed through mColorMix5Bit, _brighten or
 * _darken. (The composited frame's fade/alpha is re-applied by the
 * frontend from the per-scanline metadata, in float, after upscaling.)
 * Backdrop is not a layer: it is normalPalette[0] in the metadata.
 *
 * Flag planes: one byte per pixel per layer:
 *   bit 7  covered — this layer drew a pixel here
 *   bit 5  reblend — FLAG_REBLEND (see software-private.h)
 *   bit 4  semi-transparent — OBJ only: the winning sprite is in
 *          semi-transparent mode
 *   bit 3  target2 — BLDCNT target 2 (for OBJ: target2Obj)
 *   bit 2  target1 — BLDCNT target 1
 *   bits 0-1  OBJ priority (OBJ layer only; BG priority is per-scanline
 *          metadata, bgPriority[])
 * A pixel is only "covered" when the renderer actually attempted to
 * composite it: a transparent tile pixel, a pixel outside the layer's
 * window, or a pixel the OBJWIN gate excluded leaves the flag clear.
 * (A pixel blocked by IS_WRITABLE — a plain priority-0 sprite already
 * won — is likewise uncovered: it could never have won the composite
 * and cannot be a blend partner, since it carries no TARGET_1.)
 *
 * Affine supersampling (Phase 2.5): the software renderer also
 * records, per scanline, the BG2CNT/BG3CNT map geometry (bgCnt[2]
 * in the metadata, offset 132) and the full 512-entry palette
 * (atomicPaletteHistory, one row per scanline — see
 * video-software.h), and the frontend reads the game's VRAM
 * (96 KB) through atomic_affine_get_vram. The GPU affine
 * rasteriser evaluates the hardware's own affine transform per
 * output pixel from those three inputs; the 1x layer planes above
 * stay the coverage authority (see the coverage rule in the
 * Phase 2.5 plan). The palette history and the VRAM are versioned
 * (atomic_affine_get_versions): the frontend re-uploads its
 * textures only when a version moves.
 *
 * Per-scanline metadata: one AtomicScanlineMeta per scanline, in
 * scanline order. Layout is FIXED (144 bytes); the fields are ordered
 * so the struct has no compiler padding on any target (32-bit fields
 * first, then 16-bit, then 8-bit), and the bridge and the Kotlin side
 * read fields at the offsets below. The host harness oracle decodes
 * them with the same offsets. Do not reorder fields without bumping
 * the layout — there is no version field; the size check in
 * atomic_layers_metadata_size() is the guard.
 *
 * Offsets (bytes):
 *    0  dispcnt        u16  raw DISPCNT (mode bits 0-2, forced blank
 *                           bit 6, BG enables 8-11, OBJ 12, WIN0 13,
 *                           WIN1 14, OBJWIN 15)
 *    2  bldcnt         u16  raw BLDCNT
 *    4  bldalpha       u16  raw BLDALPHA (EVA low byte, EVB high byte)
 *    6  bldy           u16  raw BLDY (EVY low 5 bits)
 *    8  backdrop       u16  normalPalette[0], RGB565, NEVER blend-faded
 *                           (the frontend re-applies the fade from
 *                           blendEffect/bldy). When the core's colour
 *                           correction is active, get_metadata returns
 *                           the colour-corrected backdrop here (same
 *                           LUT as the layers and the frame); the
 *                           snapshot itself always stores the raw one.
 *   10  mosaic         u16  raw MOSAIC register
 *   12  bgRefX[2]      s32  BG2/BG3 refx (affine origin, fixed point)
 *   20  bgRefY[2]      s32  BG2/BG3 refy
 *   28  bgSx[2]        s32  BG2/BG3 sx (affine origin this scanline)
 *   36  bgSy[2]        s32  BG2/BG3 sy
 *   44  bgHofs[4]      u16  BGxHOFS per BG (text-mode scroll)
 *   52  bgVofs[4]      u16  BGxVOFS per BG
 *   60  bgPa[2]        s16  BG2/BG3 dx (affine matrix a)
 *   64  bgPb[2]        s16  BG2/BG3 dmx (matrix b)
 *   68  bgPc[2]        s16  BG2/BG3 dy (matrix c)
 *   72  bgPd[2]        s16  BG2/BG3 dmy (matrix d)
 *   76  blendEffect    u8   0 none, 1 alpha, 2 brighten, 3 darken
 *   77  forceTarget1   u8   renderer forceTarget1 (see PostprocessBuffer)
 *   78  forcedBlank    u8   1 when this scanline was forced blank
 *                           (composited output is white; layers are empty)
 *   79  stereo         u8   1 when STEREOCNT stereo is on (the layer
 *                           path does NOT reproduce the green-channel
 *                           swap; the bit only marks the frame)
 *   80  nWindows       u8   entries used in winEndX/winControl (1-5)
 *   81  reserved0      u8
 *   82  bgPriority[4]  u8   BGCNT priority per BG
 *   86  bgEnabled[4]   u8   1 when bg[i].enabled == ENABLED_MAX
 *   90  winEndX[5]     u8   window segment end x (last is 240)
 *   95  winControl[5]  u8   window control packed per segment
 *                           (bits 0-3 BG enables, 4 OBJ, 5 blend)
 *  100  winoutControl  u8   WINOUT low half (outside any window)
 *  101  objwinControl  u8   WINOUT high half (inside the OBJ window)
 *  102  objwinMask[30] u8   240 bits, 1 = pixel inside the OBJ window
 *  132  bgCnt[2]       u16  BG2CNT/BG3CNT (affine map geometry:
 *                           screen base, char base, size, overflow,
 *                           mosaic — Phase 2.5). 0 on an old dump
 *                           means unknown geometry; the frontend
 *                           falls back to the MMPX pass.
 *  136  mosaicEnable   u8   per-BG mosaic enable, bit i =
 *                           BGCNT[i] bit 6 (Phase 2.6). 0 on an old
 *                           dump means no mosaic info; the frontend
 *                           renders today's picture.
 *  137  reserved[7]    u8   zero
 *  Total: 144 bytes.
 */

#define ATOMIC_LAYER_COUNT 5
#define ATOMIC_LAYER_WIDTH 240
#define ATOMIC_LAYER_HEIGHT 160
#define ATOMIC_LAYER_PIXELS (ATOMIC_LAYER_WIDTH * ATOMIC_LAYER_HEIGHT)
#define ATOMIC_MAX_WINDOWS 5
#define ATOMIC_META_SIZE 144

#define ATOMIC_FLAG_COVERED 0x80
#define ATOMIC_FLAG_MOSAIC 0x40
#define ATOMIC_FLAG_REBLEND 0x20
#define ATOMIC_FLAG_SEMITRANS 0x10
#define ATOMIC_FLAG_TARGET2 0x08
#define ATOMIC_FLAG_TARGET1 0x04
#define ATOMIC_FLAG_OBJ_PRIO_MASK 0x03

struct AtomicScanlineMeta {
	uint16_t dispcnt;
	uint16_t bldcnt;
	uint16_t bldalpha;
	uint16_t bldy;
	uint16_t backdrop;
	uint16_t mosaic;
	int32_t bgRefX[2];
	int32_t bgRefY[2];
	int32_t bgSx[2];
	int32_t bgSy[2];
	uint16_t bgHofs[4];
	uint16_t bgVofs[4];
	int16_t bgPa[2];
	int16_t bgPb[2];
	int16_t bgPc[2];
	int16_t bgPd[2];
	uint8_t blendEffect;
	uint8_t forceTarget1;
	uint8_t forcedBlank;
	uint8_t stereo;
	uint8_t nWindows;
	uint8_t reserved0;
	uint8_t bgPriority[4];
	uint8_t bgEnabled[4];
	uint8_t winEndX[ATOMIC_MAX_WINDOWS];
	uint8_t winControl[ATOMIC_MAX_WINDOWS];
	uint8_t winoutControl;
	uint8_t objwinControl;
	uint8_t objwinMask[30];
	uint16_t bgCnt[2];
	uint8_t mosaicEnable;
	uint8_t reserved[7];
};

CXX_GUARD_START

#endif
