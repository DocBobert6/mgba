/* Copyright (c) 2026 Atomic contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ATOMIC_TILES_H
#define ATOMIC_TILES_H

#include <mgba-util/common.h>

CXX_GUARD_START

/* Atomic tile deduplication (Phase 2.2): the software renderer records
 * the identity of every text-mode background tile it fetches, so a
 * frontend can upscale each unique 8x8 tile once (with its 2 px halo)
 * and stamp the result everywhere that tile occurs, instead of running
 * the MMPX 2x pass over the whole layer every frame. The grid is
 * scroll-independent by construction: it records SOURCE-tile identity,
 * never screen-space pixels, so a 1 px scroll moves stamp positions
 * but changes no key.
 *
 * Why a per-scanline grid (not the 30x20 band grid the plan first
 * sketched): a screen cell's 8x8 pixels come from up to 4 source tiles
 * when the scroll phase is non-zero (the map grid is offset from the
 * screen grid by HOFS/VOFS mod 8), and a cell's 12x12 MMPX patch spans
 * up to 3x3 source tiles around the tile holding the cell's centre
 * pixel. At the frame edges the patch's halo reaches a partial tile
 * that holds no cell centre at all (e.g. the leading partial tile
 * when HOFS % 8 >= 4), and at the top/bottom bands the patch reaches
 * a map row drawn on scanlines outside the band's centre scanline
 * (when VOFS % 8 >= 5). A band grid therefore cannot hold every tile
 * a patch depends on. The per-scanline grid records exactly what was
 * drawn on each scanline, so the app can assemble a patch's tile set
 * by reading grid rows [y-2, y+9] x cols [cx-1, cx+1] with no scroll
 * arithmetic at all — raster scroll, windows and wraparound are all
 * captured exactly, for free. Cost: 80 KiB/frame through the bridge
 * (4 layers x 160 rows x 32 cols x 4 B) vs 9.6 KiB for a band grid.
 *
 * Grid layout: per BG layer (0-3; OBJ has no grid and is never
 * deduped), one u32 key per (scanline, column). Columns are stored at
 * index col+1: col is the lattice column of the tile's logical span,
 * col = floor((sx+3)/8) where sx is the tile's logical screen start
 * (its source pixel 0); col ranges [-1, 30] (the -1 and 30 slots hold
 * the leading/trailing partial tiles, whose spans contain the
 * off-frame lattice centres -4 and 244). Rows are scanlines 0-159.
 * A cell holds 0 when no tile was recorded there (layer did not draw
 * on that scanline, or the scanline was not redrawn and the row was
 * cleared). Key 0 doubles as "tile 0, palette 0, 4bpp, no flip" and
 * "no tile": the two are indistinguishable in the grid but never in
 * the atlas key, because the 12x12 coverage mask (from the flag plane)
 * differs — a real tile-0 cell has covered pixels, a no-tile cell has
 * none. The atlas key is hash(grid keys || coverage mask || palette
 * versions || VRAM versions || epoch), so it is complete either way.
 *
 * Key layout (32 bits):
 *   bits  0-16  VRAM char byte-address of the tile (17 bits; VRAM is
 *                96 KiB = 0x18000, but the address can reach 0x1BFC0
 *                for an 8bpp tile in char block 3)
 *   bits 17-20  palette number (4 bits; 0 for 8bpp tiles, which use
 *                the whole 256-colour BG palette)
 *   bit  21     depth: 0 = 4bpp (16 colours, 32 B/tile), 1 = 8bpp
 *                (256 colours, 64 B/tile)
 *   bit  22     HFLIP
 *   bit  23     VFLIP
 *   bit  24     BG/OBJ tag: 0 = BG, 1 = OBJ (always 0 in 2.2; the bit
 *                separates the namespaces if OBJ dedup ever lands.
 *                BG char addresses CAN alias OBJ tile addresses in
 *                theory — char block 3 + a high tile index reads past
 *                0x10000 into OBJ VRAM — but OBJ tiles never enter
 *                the atlas, so the alias is harmless)
 *   bits 25-31  reserved (0)
 *
 * Versions (invalidation): the atlas entry is usable only while its
 * key matches the CURRENT versions. Palette RAM writes bump a
 * per-palette generation (32 x u32: 16 BG + 16 OBJ palettes, indexed
 * by palette number); VRAM writes bump a per-32-byte-slot generation
 * (3072 x u32: VRAM 96 KiB / 32 B; a 4bpp tile is one slot, an 8bpp
 * tile is two). Generations are u32, not u16: a u16 wraps after 65,536
 * writes to one slot, and a wrap could resurrect a stale entry; a u32
 * is effectively unwrappable. A global epoch (u32) bumps on every
 * renderer reset — including state load, which ends in reset — so a
 * state load retires the whole atlas even if VRAM/palette contents
 * happen to match the old generations. Colour-correction toggle and
 * GL context loss are app-side drops (the atlas is filled from the
 * corrected planes when CC is on, and the LUT is constant while on).
 */

#define ATOMIC_TILE_GRID_LAYERS 4
#define ATOMIC_TILE_GRID_ROWS 160
#define ATOMIC_TILE_GRID_COLS 32
#define ATOMIC_TILE_GRID_COL_OFFSET 1
#define ATOMIC_TILE_GRID_CELLS \
	(ATOMIC_TILE_GRID_LAYERS * ATOMIC_TILE_GRID_ROWS * ATOMIC_TILE_GRID_COLS)
#define ATOMIC_TILE_GRID_SIZE (ATOMIC_TILE_GRID_CELLS * 4)

#define ATOMIC_TILE_PALETTE_GENS 32
#define ATOMIC_TILE_VRAM_SIZE 0x18000
#define ATOMIC_TILE_VRAM_SLOTS (ATOMIC_TILE_VRAM_SIZE / 32)

#define ATOMIC_TILE_KEY_ADDR_MASK 0x1FFFFu
#define ATOMIC_TILE_KEY_ADDR_SHIFT 0
#define ATOMIC_TILE_KEY_PALETTE_MASK 0xFu
#define ATOMIC_TILE_KEY_PALETTE_SHIFT 17
#define ATOMIC_TILE_KEY_DEPTH_8BPP (1u << 21)
#define ATOMIC_TILE_KEY_HFLIP (1u << 22)
#define ATOMIC_TILE_KEY_VFLIP (1u << 23)
#define ATOMIC_TILE_KEY_OBJ (1u << 24)

CXX_GUARD_START

#endif
