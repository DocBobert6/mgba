/* Copyright (c) 2026 Atomic contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "libretro-atomic.h"

#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/audio.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Atomic MP2K HLE (alpha.55): a faithful C translation of
 * NanoBoyAdvance's MP2K HLE mixer (nba/src/hw/apu/hle/mp2k.cc,
 * mp2k.hh — GPL-3.0-or-later, The NanoBoyAdvance Authors), ported
 * into the mGBA core and exposed through the atomic_mp2k_* API.
 *
 * What it does, in NBA's shape: when the emulated CPU is about to
 * execute the first instruction of the game's SoundMainRAM (the
 * hook installed by atomic_mp2k_set_hook, checked in the ARM step
 * functions against NanoBoyAdvance's r15 convention), this module
 * reads the SoundInfo address from IWRAM 0x03007FF0 and runs the
 * game's own envelope state machine in float, rendering at
 * 65,536 Hz into a 7-frame ring buffer. The audio splice in
 * GBAAudioSample then replaces the DMA FIFO (chA/chB) contributions
 * with the HLE pair for that output sample; PSG is untouched in
 * both paths, and the real SoundMainRAM still executes on the ARM
 * CPU, so cycle accounting and game state are unchanged.
 *
 * Deliberate deviations from NBA (all noted where they occur):
 *  - Cubic interpolation is always ON (the proof ran cubic on);
 *    there is no cubic toggle and no force_reverb — the game's own
 *    reverb value is used, never forced (proof requirement).
 *  - NBA panics if pcm_samples_per_vblank is zero on first engage;
 *    this port returns without engaging instead (never crash the
 *    core for a malformed SoundInfo).
 *  - Reset also clears the ring buffer (NBA leaves stale floats in
 *    it, which its reverb would read on the first frame after a
 *    state load).
 *  - The HLE state is NOT serialized (NBA's design): reset on
 *    state load, rebuild from ROM on the next SoundMainRAM call.
 *    A brief audio glitch on load is expected.
 *  - Upstream mGBA's audio.h carries a GBAMP2kContext struct and a
 *    MP2K_MAGIC of 0x68736D53; the layout matches NBA's, but the
 *    magic is wrong (NBA's proven value is 0x68736D54, "Tsmh"), so
 *    this file defines its own structs with NBA's field names and
 *    its own magic constant, and never uses the upstream macro.
 *    The struct layouts are compile-time asserted below.
 *
 * Threading: the hook callback and the sample-pair getter run on
 * the emulation thread during retro_run; the atomic_mp2k_* setters
 * and reset run under the core mutex (bridge) or from libretro.c's
 * retro_* entry points, while the core is not running. No lock is
 * taken here beyond that discipline.
 */

#define MP2K_SAMPLE_RATE 65536
#define MP2K_SAMPLES_PER_FRAME (MP2K_SAMPLE_RATE / 60 + 1) /* 1093 */
#define MP2K_TOTAL_FRAME_COUNT 7
#define MP2K_MAX_SOUND_CHANNELS 12
#define MP2K_SOUND_INFO_MAGIC 0x68736D54 /* "Tsmh", little-endian */
#define MP2K_SOUND_INFO_ADDRESS 0x03007FF0
#define MP2K_HOOK_DISABLED 0xFFFFFFFF

#define MP2K_S8_TO_FLOAT(value) ((float) (int8_t) (value) / 127.0f)
#define MP2K_U8_TO_FLOAT(value) ((float) (value) / 256.0f)

enum Mp2kSoundChannelStatus {
	MP2K_CHANNEL_START = 0x80,
	MP2K_CHANNEL_STOP = 0x40,
	MP2K_CHANNEL_LOOP = 0x10,
	MP2K_CHANNEL_ECHO = 0x04,

	MP2K_CHANNEL_ENV_MASK = 0x03,
	MP2K_CHANNEL_ENV_ATTACK = 0x03,
	MP2K_CHANNEL_ENV_DECAY = 0x02,
	MP2K_CHANNEL_ENV_SUSTAIN = 0x01,
	MP2K_CHANNEL_ENV_RELEASE = 0x00,

	MP2K_CHANNEL_ON = MP2K_CHANNEL_START | MP2K_CHANNEL_STOP | MP2K_CHANNEL_ECHO | MP2K_CHANNEL_ENV_MASK
};

/* NBA's SoundChannel / SoundInfo, field for field (mp2k.hh). The
   game writes these bytes into GBA RAM; the layout must match
   exactly, hence the static asserts. */
struct Mp2kSoundChannel {
	uint8_t status;
	uint8_t type;
	uint8_t volume_r;
	uint8_t volume_l;
	uint8_t envelope_attack;
	uint8_t envelope_decay;
	uint8_t envelope_sustain;
	uint8_t envelope_release;
	uint8_t unknown0;
	uint8_t envelope_volume;
	uint8_t envelope_volume_r;
	uint8_t envelope_volume_l;
	uint8_t echo_volume;
	uint8_t echo_length;
	uint8_t unknown1[18];
	uint32_t frequency;
	uint32_t wave_address;
	uint32_t unknown2[6];
};

struct Mp2kSoundInfo {
	uint32_t magic;
	uint8_t pcm_dma_counter;
	uint8_t reverb;
	uint8_t max_channels;
	uint8_t master_volume;
	uint8_t unknown0[8];
	int32_t pcm_samples_per_vblank;
	int32_t pcm_sample_rate;
	uint32_t unknown1[14];
	struct Mp2kSoundChannel channels[MP2K_MAX_SOUND_CHANNELS];
};

struct Mp2kWaveInfo {
	uint16_t type;
	uint16_t status;
	uint32_t frequency;
	uint32_t loop_position;
	uint32_t number_of_samples;
};

typedef char mp2k_channel_size_check[sizeof(struct Mp2kSoundChannel) == 64 ? 1 : -1];
typedef char mp2k_info_size_check[sizeof(struct Mp2kSoundInfo) == 848 ? 1 : -1];
typedef char mp2k_wave_info_size_check[sizeof(struct Mp2kWaveInfo) == 16 ? 1 : -1];
typedef char mp2k_samples_per_frame_check[MP2K_SAMPLES_PER_FRAME == 1093 ? 1 : -1];

struct Mp2kSampler {
	bool compressed;
	bool should_fetch_sample;
	uint32_t current_position;
	float resample_phase;
	float sample_history[4];
	struct Mp2kWaveInfo wave_info;
	uint8_t* wave_data;
};

struct Mp2kEnvelope {
	float volume;
	float volume_l[2];
	float volume_r[2];
};

/* HLE state (one instance: a libretro core runs one game). The ring
   buffer is static storage (1093 * 7 * 2 floats = 61,208 bytes)
   rather than NBA's heap allocation on first engage, so engagement
   cannot fail. */
static bool mp2kEngaged = false;
static bool mp2kEnabled = false;
static uint32_t mp2kHookAddress = MP2K_HOOK_DISABLED;
/* True between retro_load_game and retro_unload_game. Guards
   _mp2kApplyToCore: after unload the libretro core pointer dangles
   (the frontend frees the core without NULLing it), so the apply
   helper must not dereference it until the next load. */
static bool mp2kGameLoaded = false;
static struct Mp2kSoundInfo mp2kSoundInfo;
static struct Mp2kSampler mp2kSamplers[MP2K_MAX_SOUND_CHANNELS];
static struct Mp2kEnvelope mp2kEnvelopes[MP2K_MAX_SOUND_CHANNELS];
static float mp2kBuffer[MP2K_SAMPLES_PER_FRAME * MP2K_TOTAL_FRAME_COUNT * 2];
static int mp2kCurrentFrame = 0;
static int mp2kBufferReadIndex = 0;
/* Consumption accumulator for _mp2kSamplePair (see there): render
   samples owed, plus the last pair handed out (held on calls where
   no new render sample is due). */
static float mp2kConsumeDue = 0.0f;
static float mp2kLastRight = 0.0f;
static float mp2kLastLeft = 0.0f;
/* The running game's GBA (NBA's MP2K holds a Bus& for the same
   purpose): set by the hook callback before SoundMainRAM runs, and
   by the apply helper on game load, so the mixer can resolve wave
   addresses to host pointers. One core runs one game, so a single
   static is the whole story. */
static struct GBA* mp2kGba = NULL;

/* libretro.c owns the core pointer (see libretro-atomic.c). */
extern struct mCore* libretroCoreInstance(void);

static void _mp2kRenderFrame(void);
static float* _mp2kReadSample(void);

/* NBA's Bus::GetHostAddress, for the mGBA memory map: map a GBA
   address to a host pointer, or NULL when the [address,
   address + size) range is not fully backed. BIOS/EWRAM/IWRAM
   mirror within their region; ROM mirrors every romSize bytes
   (romMask is the power-of-two mirror mask). Wave data and
   SoundInfo live in ROM/RAM; anything else (IO, VRAM, SRAM) has no
   host mapping and returns NULL, exactly as NBA's version does for
   unmapped pages. */
static uint8_t* _mp2kHostAddress(struct GBA* gba, uint32_t address, size_t size) {
	uint32_t page = address >> 24;
	uint32_t offset;
	if (!gba) {
		return NULL;
	}
	switch (page) {
	case 0x00: /* BIOS */
		offset = address & 0x00FFFFFF;
		if (gba->memory.bios && offset + size <= GBA_SIZE_BIOS) {
			return (uint8_t*) gba->memory.bios + offset;
		}
		break;
	case 0x02: /* EWRAM */
		offset = address & 0x0003FFFF;
		if (offset + size <= GBA_SIZE_EWRAM) {
			return (uint8_t*) gba->memory.wram + offset;
		}
		break;
	case 0x03: /* IWRAM */
		offset = address & 0x00007FFF;
		if (offset + size <= GBA_SIZE_IWRAM) {
			return (uint8_t*) gba->memory.iwram + offset;
		}
		break;
	case 0x08: /* ROM (WS0/WS1/WS2, 0x08-0x0D) */
	case 0x09:
	case 0x0A:
	case 0x0B:
	case 0x0C:
	case 0x0D:
		offset = address & 0x01FFFFFF;
		if (!gba->memory.rom || gba->memory.romSize == 0) {
			return NULL;
		}
		if (offset >= gba->memory.romSize) {
			offset &= gba->memory.romMask;
		}
		if (offset + size <= gba->memory.romSize) {
			return (uint8_t*) gba->memory.rom + offset;
		}
		break;
	default:
		break;
	}
	return NULL;
}

static void _mp2kResetState(void) {
	int i;
	mp2kEngaged = false;
	mp2kCurrentFrame = 0;
	mp2kBufferReadIndex = 0;
	mp2kConsumeDue = 0.0f;
	mp2kLastRight = 0.0f;
	mp2kLastLeft = 0.0f;
	for (i = 0; i < MP2K_MAX_SOUND_CHANNELS; ++i) {
		memset(&mp2kSamplers[i], 0, sizeof(mp2kSamplers[i]));
		memset(&mp2kEnvelopes[i], 0, sizeof(mp2kEnvelopes[i]));
	}
	memset(mp2kBuffer, 0, sizeof(mp2kBuffer));
}

/* Faithful translation of NBA MP2K::SoundMainRAM (mp2k.cc). The
   input is a copy of the game's SoundInfo bytes: the HLE keeps its
   own member copy (mp2kSoundInfo) and never writes back to GBA RAM
   — the real SoundMainRAM, still executing on the ARM CPU, owns the
   game's copy. */
static void _mp2kSoundMainRAM(const struct Mp2kSoundInfo* soundInfo) {
	int i;
	uint8_t maxChannels;

	if (soundInfo->magic != MP2K_SOUND_INFO_MAGIC) {
		return;
	}

	if (!mp2kEngaged) {
		if (soundInfo->pcm_samples_per_vblank == 0) {
			/* NBA panics here; the port refuses to engage instead
			   — a malformed SoundInfo must never crash the core. */
			return;
		}
		mp2kEngaged = true;
	}

	maxChannels = soundInfo->max_channels < MP2K_MAX_SOUND_CHANNELS
		? soundInfo->max_channels : MP2K_MAX_SOUND_CHANNELS;

	mp2kSoundInfo = *soundInfo;

	/* Update the channel state and envelope volume for this audio
	   frame. */
	for (i = 0; i < maxChannels; ++i) {
		struct Mp2kSoundChannel* channel = &mp2kSoundInfo.channels[i];
		struct Mp2kSampler* sampler = &mp2kSamplers[i];
		uint32_t envelopeVolume;
		uint8_t envelopePhase;
		float hqEnvelopeVolume[2];

		if ((channel->status & MP2K_CHANNEL_ON) == 0) {
			continue;
		}

		envelopeVolume = (uint32_t) channel->envelope_volume;
		envelopePhase = channel->status & MP2K_CHANNEL_ENV_MASK;

		hqEnvelopeVolume[0] = mp2kEnvelopes[i].volume;

		if (channel->status & MP2K_CHANNEL_START) {
			const struct Mp2kWaveInfo* waveInfo;
			uint8_t* waveInfoBytes;
			if (channel->status & MP2K_CHANNEL_STOP) {
				channel->status = 0;
				continue;
			}

			envelopeVolume = channel->envelope_attack;
			if (envelopeVolume == 0xFF) {
				channel->status = MP2K_CHANNEL_ENV_DECAY;
			} else {
				channel->status = MP2K_CHANNEL_ENV_ATTACK;
			}
			hqEnvelopeVolume[0] = MP2K_U8_TO_FLOAT(channel->envelope_attack);

			waveInfoBytes = _mp2kHostAddress(mp2kGba, channel->wave_address, sizeof(*waveInfo));
			if (!waveInfoBytes) {
				/* Disable the channel; there is no good way to
				   deal with this. */
				channel->status = 0;
				continue;
			}
			waveInfo = (const struct Mp2kWaveInfo*) waveInfoBytes;
			memset(sampler, 0, sizeof(*sampler));
			sampler->wave_info = *waveInfo;
			if (sampler->wave_info.status & 0xC000) {
				channel->status |= MP2K_CHANNEL_LOOP;
			}
		} else if (channel->status & MP2K_CHANNEL_ECHO) {
			if (channel->echo_length-- == 0) {
				channel->status = 0;
				continue;
			}
		} else if (channel->status & MP2K_CHANNEL_STOP) {
			envelopeVolume = (envelopeVolume * channel->envelope_release) >> 8;
			hqEnvelopeVolume[0] *= MP2K_U8_TO_FLOAT(channel->envelope_release);

			if (envelopeVolume <= channel->echo_volume) {
				if (channel->echo_volume == 0) {
					channel->status = 0;
					continue;
				}

				channel->status |= MP2K_CHANNEL_ECHO;
				envelopeVolume = (uint32_t) channel->echo_volume;
				hqEnvelopeVolume[0] = MP2K_U8_TO_FLOAT(channel->echo_volume);
			}
		} else if (envelopePhase == MP2K_CHANNEL_ENV_ATTACK) {
			envelopeVolume += channel->envelope_attack;
			hqEnvelopeVolume[0] += MP2K_U8_TO_FLOAT(channel->envelope_attack);
			if (hqEnvelopeVolume[0] > 1.0f) {
				hqEnvelopeVolume[0] = 1.0f;
			}

			if (envelopeVolume > 0xFE) {
				channel->status = (channel->status & ~MP2K_CHANNEL_ENV_MASK) | MP2K_CHANNEL_ENV_DECAY;
				envelopeVolume = 0xFF;
			}
		} else if (envelopePhase == MP2K_CHANNEL_ENV_DECAY) {
			uint8_t envelopeSustain;
			envelopeVolume = (envelopeVolume * channel->envelope_decay) >> 8;
			hqEnvelopeVolume[0] *= MP2K_U8_TO_FLOAT(channel->envelope_decay);

			envelopeSustain = channel->envelope_sustain;
			if (envelopeVolume <= envelopeSustain) {
				if (envelopeSustain == 0 && channel->echo_volume == 0) {
					channel->status = 0;
					continue;
				}

				channel->status = (channel->status & ~MP2K_CHANNEL_ENV_MASK) | MP2K_CHANNEL_ENV_SUSTAIN;
				envelopeVolume = envelopeSustain;
				hqEnvelopeVolume[0] = MP2K_U8_TO_FLOAT(envelopeSustain);
			}
		}

		channel->envelope_volume = (uint8_t) envelopeVolume;
		envelopeVolume = (envelopeVolume * (mp2kSoundInfo.master_volume + 1)) >> 4;
		channel->envelope_volume_r = (uint8_t) ((envelopeVolume * channel->volume_r) >> 8);
		channel->envelope_volume_l = (uint8_t) ((envelopeVolume * channel->volume_l) >> 8);

		/* Try to predict the envelope's value at the start of the
		   next audio frame, so that we can linearly interpolate
		   the envelope between the current and next frame. */
		if (channel->status & MP2K_CHANNEL_STOP) {
			if (((envelopeVolume * channel->envelope_release) >> 8) <= channel->echo_volume) {
				hqEnvelopeVolume[1] = MP2K_U8_TO_FLOAT(channel->echo_volume);
			} else {
				hqEnvelopeVolume[1] = hqEnvelopeVolume[0] * MP2K_U8_TO_FLOAT(channel->envelope_release);
			}
		} else if ((channel->status & MP2K_CHANNEL_ENV_MASK) == MP2K_CHANNEL_ENV_ATTACK) {
			hqEnvelopeVolume[1] = hqEnvelopeVolume[0] + MP2K_U8_TO_FLOAT(channel->envelope_attack);
			if (hqEnvelopeVolume[1] > 1.0f) {
				hqEnvelopeVolume[1] = 1.0f;
			}
		} else if ((channel->status & MP2K_CHANNEL_ENV_MASK) == MP2K_CHANNEL_ENV_DECAY) {
			if (((envelopeVolume * channel->envelope_decay) >> 8) <= channel->envelope_sustain) {
				hqEnvelopeVolume[1] = MP2K_U8_TO_FLOAT(channel->envelope_sustain);
			} else {
				hqEnvelopeVolume[1] = hqEnvelopeVolume[0] * MP2K_U8_TO_FLOAT(channel->envelope_decay);
			}
		} else {
			hqEnvelopeVolume[1] = hqEnvelopeVolume[0];
		}

		{
			float hqMasterVolume = (soundInfo->master_volume + 1) / 16.0f;
			float hqVolumeR = hqMasterVolume * MP2K_U8_TO_FLOAT(channel->volume_r);
			float hqVolumeL = hqMasterVolume * MP2K_U8_TO_FLOAT(channel->volume_l);
			int j;

			mp2kEnvelopes[i].volume = hqEnvelopeVolume[0];

			for (j = 0; j < 2; ++j) {
				mp2kEnvelopes[i].volume_r[j] = hqEnvelopeVolume[j] * hqVolumeR;
				mp2kEnvelopes[i].volume_l[j] = hqEnvelopeVolume[j] * hqVolumeL;
			}
		}
	}
}

/* Faithful translation of NBA MP2K::RenderFrame (mp2k.cc). Cubic
   interpolation is always ON (the proof ran cubic on); there is no
   toggle. Reverb uses the game's own reverb value only —
   force_reverb is NOT implemented, it is always off. */
static void _mp2kRenderReverb(float* destination, uint8_t strength) {
	static const float kEarlyCoefficient = 0.0015f;
	static const float kLateCoefficients[3][2] = {
		{ 1.0f, 0.1f },
		{ 0.6f, 0.25f },
		{ 0.35f, 0.35f }
	};
	/* NBA computes this as 1.0 / the float sum of the six late
	   coefficients, accumulated in order; written the same way
	   here so the value matches bit for bit. */
	static const float kNormalizeCoefficients = 1.0f / (1.0f + 0.1f + 0.6f + 0.25f + 0.35f + 0.35f);
	const float* earlyBuffer = &mp2kBuffer[((mp2kCurrentFrame + MP2K_TOTAL_FRAME_COUNT - 1) % MP2K_TOTAL_FRAME_COUNT) * MP2K_SAMPLES_PER_FRAME * 2];
	const float* lateBuffers[3] = {
		&mp2kBuffer[((mp2kCurrentFrame + 2) % MP2K_TOTAL_FRAME_COUNT) * MP2K_SAMPLES_PER_FRAME * 2],
		&mp2kBuffer[((mp2kCurrentFrame + 1) % MP2K_TOTAL_FRAME_COUNT) * MP2K_SAMPLES_PER_FRAME * 2],
		destination
	};
	float factor = strength / 128.0f;
	int l;
	int j;

	for (l = 0; l < MP2K_SAMPLES_PER_FRAME * 2; l += 2) {
		int r = l + 1;
		float earlyReflectionL = earlyBuffer[l] * kEarlyCoefficient;
		float earlyReflectionR = earlyBuffer[r] * kEarlyCoefficient;
		float lateReflectionL = 0.0f;
		float lateReflectionR = 0.0f;

		for (j = 0; j < 3; ++j) {
			float sampleL = lateBuffers[j][l];
			float sampleR = lateBuffers[j][r];

			lateReflectionL += sampleL * kLateCoefficients[j][0] + sampleR * kLateCoefficients[j][1];
			lateReflectionR += sampleL * kLateCoefficients[j][1] + sampleR * kLateCoefficients[j][0];
		}

		lateReflectionL *= kNormalizeCoefficients;
		lateReflectionR *= kNormalizeCoefficients;

		destination[l] = (earlyReflectionL + lateReflectionL) * factor;
		destination[r] = (earlyReflectionR + lateReflectionR) * factor;
	}
}

static void _mp2kRenderFrame(void) {
	/* Differential LUT for DPCM samples (NBA's kDifferentialLUT). */
	static const float kDifferentialLUT[16] = {
		MP2K_S8_TO_FLOAT(0x00), MP2K_S8_TO_FLOAT(0x01), MP2K_S8_TO_FLOAT(0x04), MP2K_S8_TO_FLOAT(0x09),
		MP2K_S8_TO_FLOAT(0x10), MP2K_S8_TO_FLOAT(0x19), MP2K_S8_TO_FLOAT(0x24), MP2K_S8_TO_FLOAT(0x31),
		MP2K_S8_TO_FLOAT(0xC0), MP2K_S8_TO_FLOAT(0xCF), MP2K_S8_TO_FLOAT(0xDC), MP2K_S8_TO_FLOAT(0xE7),
		MP2K_S8_TO_FLOAT(0xF0), MP2K_S8_TO_FLOAT(0xF7), MP2K_S8_TO_FLOAT(0xFC), MP2K_S8_TO_FLOAT(0xFF)
	};
	uint8_t reverbStrength;
	uint8_t maxChannels;
	float* destination;
	int i;

	mp2kCurrentFrame = (mp2kCurrentFrame + 1) % MP2K_TOTAL_FRAME_COUNT;

	/* The game's own reverb value only. NBA's force_reverb (which
	   forces reverb >= 48 even when the game sets 0) is NOT
	   implemented: the proof requires it off. */
	reverbStrength = mp2kSoundInfo.reverb;
	maxChannels = mp2kSoundInfo.max_channels < MP2K_MAX_SOUND_CHANNELS
		? mp2kSoundInfo.max_channels : MP2K_MAX_SOUND_CHANNELS;
	destination = &mp2kBuffer[mp2kCurrentFrame * MP2K_SAMPLES_PER_FRAME * 2];

	if (reverbStrength > 0) {
		_mp2kRenderReverb(destination, reverbStrength);
	} else {
		memset(destination, 0, MP2K_SAMPLES_PER_FRAME * 2 * sizeof(float));
	}

	for (i = 0; i < maxChannels; ++i) {
		struct Mp2kSoundChannel* channel = &mp2kSoundInfo.channels[i];
		struct Mp2kSampler* sampler = &mp2kSamplers[i];
		struct Mp2kEnvelope* envelope = &mp2kEnvelopes[i];
		struct Mp2kWaveInfo* waveInfo;
		float angularStep;
		bool compressed;
		float* sampleHistory;
		uint8_t* waveData;
		int j;

		if ((channel->status & MP2K_CHANNEL_ON) == 0) {
			continue;
		}

		if (channel->type & 8) {
			angularStep = (float) mp2kSoundInfo.pcm_sample_rate / (float) MP2K_SAMPLE_RATE;
		} else {
			angularStep = (float) channel->frequency / (float) MP2K_SAMPLE_RATE;
		}

		compressed = (channel->type & 32) != 0;
		sampleHistory = sampler->sample_history;

		waveInfo = &sampler->wave_info;

		if (sampler->compressed != compressed || !sampler->wave_data) {
			uint32_t waveSize = waveInfo->number_of_samples;
			uint32_t waveDataBegin;
			if (compressed) {
				waveSize *= 33;
				waveSize = (waveSize + 63) / 64;
			}
			waveDataBegin = channel->wave_address + sizeof(struct Mp2kWaveInfo);
			sampler->wave_data = _mp2kHostAddress(mp2kGba, waveDataBegin, waveSize);
			if (!sampler->wave_data) {
				/* Disable the channel; there is no good way to
				   deal with this. */
				channel->status = 0;
				continue;
			}
			sampler->compressed = compressed;
		}

		waveData = sampler->wave_data;

		for (j = 0; j < MP2K_SAMPLES_PER_FRAME; ++j) {
			float t = j / (float) MP2K_SAMPLES_PER_FRAME;
			float volumeL = envelope->volume_l[0] * (1.0f - t) + envelope->volume_l[1] * t;
			float volumeR = envelope->volume_r[0] * (1.0f - t) + envelope->volume_r[1] * t;
			float sample;
			float mu;

			if (sampler->should_fetch_sample) {
				if (compressed) {
					uint32_t blockOffset = sampler->current_position & 63;
					uint32_t blockAddress = (sampler->current_position >> 6) * 33;
					uint32_t address;
					uint8_t lutIndex;

					if (blockOffset == 0) {
						sample = MP2K_S8_TO_FLOAT(waveData[blockAddress]);
					} else {
						sample = sampleHistory[0];
					}

					address = blockAddress + (blockOffset >> 1) + 1;
					lutIndex = waveData[address];

					if (blockOffset & 1) {
						lutIndex &= 15;
					} else {
						lutIndex >>= 4;
					}

					sample += kDifferentialLUT[lutIndex];
				} else {
					sample = MP2K_S8_TO_FLOAT(waveData[sampler->current_position]);
				}

				/* Cubic interpolation keeps two samples of
				   history ahead; linear keeps one. */
				sampleHistory[3] = sampleHistory[2];
				sampleHistory[2] = sampleHistory[1];
				sampleHistory[1] = sampleHistory[0];
				sampleHistory[0] = sample;

				sampler->should_fetch_sample = false;
			}

			mu = sampler->resample_phase;

			/* Cubic (Catmull-Rom) interpolation, always on — see
			   the file header. http://paulbourke.net/miscellaneous/interpolation/ */
			{
				float mu2 = mu * mu;
				float a0 = sampleHistory[0] - sampleHistory[1] - sampleHistory[3] + sampleHistory[2];
				float a1 = sampleHistory[3] - sampleHistory[2] - a0;
				float a2 = sampleHistory[1] - sampleHistory[3];
				float a3 = sampleHistory[2];
				sample = a0 * mu * mu2 + a1 * mu2 + a2 * mu + a3;
			}

			destination[j * 2 + 0] += sample * volumeR;
			destination[j * 2 + 1] += sample * volumeL;

			sampler->resample_phase += angularStep;

			if (sampler->resample_phase >= 1.0f) {
				int n = (int) sampler->resample_phase;
				sampler->resample_phase -= n;
				sampler->current_position += (uint32_t) n;
				sampler->should_fetch_sample = true;

				if (sampler->current_position >= waveInfo->number_of_samples) {
					if (channel->status & MP2K_CHANNEL_LOOP) {
						sampler->current_position = waveInfo->loop_position + (uint32_t) n - 1;
					} else {
						sampler->current_position = waveInfo->number_of_samples;
						sampler->should_fetch_sample = false;
					}
				}
			}
		}
	}
}

/* Faithful translation of NBA MP2K::ReadSample: renders the next
   frame lazily when the ring buffer wraps, then hands out one
   stereo pair [R, L]. */
static float* _mp2kReadSample(void) {
	float* sample;
	if (mp2kBufferReadIndex == 0) {
		_mp2kRenderFrame();
	}

	sample = &mp2kBuffer[(mp2kCurrentFrame * MP2K_SAMPLES_PER_FRAME + mp2kBufferReadIndex) * 2];

	if (++mp2kBufferReadIndex == MP2K_SAMPLES_PER_FRAME) {
		mp2kBufferReadIndex = 0;
	}

	return sample;
}

/* Render samples are consumed at the HLE's own 65,536 Hz — one
   per 256 CPU cycles, exactly NanoBoyAdvance's engaged mixer step
   (APU::StepMixer reschedules itself every 256 cycles and calls
   ReadSample once) — however fast the core's audio output runs.
   The core's output rate follows the game's SOUNDBIAS resolution:
   sampleInterval is 0x200 >> resolution, i.e. 512 cycles at res 0
   (32,768 Hz), 256 at res 1 (65,536 Hz), 128 at res 2, 64 at
   res 3. So this getter consumes sampleInterval/256 render
   samples per output sample: 2 at res 0, 1 at res 1, one every
   other call at res 2 (holding the last pair in between), one
   every fourth call at res 3. The previous code consumed two
   render samples per call unconditionally, which is correct only
   at res 0. Guranbo sets res 1 (sampleInterval 256, measured
   live), so the old code consumed the render at 131,072
   samples/s: RenderFrame fired 120x/s instead of 60x/s and the
   music played at twice the pitch — the 4,736 Hz note landed at
   9,472 Hz and the 8-16 kHz floor rose above stock. The lazy
   RenderFrame-on-wrap semantics are preserved exactly; only the
   consumption rate is fixed. Returns false (and consumes
   nothing) when the HLE is not enabled or not yet engaged, so
   the stock FIFO path runs. */
static bool _mp2kSamplePair(void* context, float* outRight, float* outLeft) {
	const float* pair = NULL;
	int sampleInterval;
	(void) context;
	if (!mp2kEnabled || !mp2kEngaged) {
		return false;
	}
	/* The core's audio sample interval in CPU cycles. Falls back
	   to the reset default (512, res 0) if the core pointer is
	   not available yet; the hook callback sets it on engage. */
	sampleInterval = mp2kGba ? mp2kGba->audio.sampleInterval : 512;
	if (sampleInterval <= 0) {
		sampleInterval = 512;
	}
	mp2kConsumeDue += (float) sampleInterval / 256.0f;
	while (mp2kConsumeDue >= 1.0f) {
		pair = _mp2kReadSample();
		mp2kConsumeDue -= 1.0f;
	}
	if (pair) {
		mp2kLastRight = pair[0];
		mp2kLastLeft = pair[1];
	}
	*outRight = mp2kLastRight;
	*outLeft = mp2kLastLeft;
	return true;
}

/* The ARM hook callback: invoked from the ARM step functions just
   before the CPU executes the instruction whose r15 equals the hook
   address — i.e. the first instruction of the game's SoundMainRAM.
   Reads the SoundInfo pointer from IWRAM 0x03007FF0 and runs the
   HLE's SoundMainRAM on a copy. The real SoundMainRAM still
   executes on the ARM CPU immediately afterwards; this runs in
   addition, exactly as in NBA's Core::Run. */
static void _mp2kHookCallback(struct ARMCore* cpu, void* context) {
	struct mCore* core = (struct mCore*) context;
	struct GBA* gba;
	uint32_t soundInfoAddress;
	const uint8_t* infoBytes;
	struct Mp2kSoundInfo soundInfo;
	(void) cpu;
	if (!core || core->platform(core) != mPLATFORM_GBA) {
		return;
	}
	gba = (struct GBA*) core->board;
	if (!gba || !gba->memory.iwram) {
		return;
	}
	mp2kGba = gba;
	/* IWRAM base is 0x03000000; 0x03007FF0 is byte offset 0x7FF0,
	   u32 index 0x1FFC. */
	soundInfoAddress = gba->memory.iwram[0x1FFC];
	infoBytes = _mp2kHostAddress(gba, soundInfoAddress, sizeof(soundInfo));
	if (!infoBytes) {
		return;
	}
	memcpy(&soundInfo, infoBytes, sizeof(soundInfo));
	_mp2kSoundMainRAM(&soundInfo);
}

/* Apply the remembered hook/enabled state to the running core:
   arm the CPU hook (or disarm it) and install the audio splice
   callback on the GBA audio core. The splice callback is installed
   unconditionally — it returns false until the HLE is enabled and
   engaged, so the stock path runs byte-identically meanwhile. */
static void _mp2kApplyToCore(void) {
	struct mCore* core = libretroCoreInstance();
	struct GBA* gba;
	struct ARMCore* cpu;
	if (!mp2kGameLoaded || !core || core->platform(core) != mPLATFORM_GBA) {
		return;
	}
	gba = (struct GBA*) core->board;
	if (!gba) {
		return;
	}
	mp2kGba = gba;
	cpu = (struct ARMCore*) core->cpu;
	if (cpu) {
		if (mp2kHookAddress == MP2K_HOOK_DISABLED) {
			/* Disarm: address first, callback last, so the step
			   functions can never observe a set address with a
			   NULL callback. */
			cpu->mp2kHookAddress = MP2K_HOOK_DISABLED;
			cpu->mp2kHookCallback = NULL;
			cpu->mp2kHookContext = NULL;
		} else {
			/* Arm: callback first, address last, for the same
			   reason. */
			cpu->mp2kHookCallback = _mp2kHookCallback;
			cpu->mp2kHookContext = core;
			cpu->mp2kHookAddress = mp2kHookAddress;
		}
	}
	gba->audio.mp2kSample = _mp2kSamplePair;
	gba->audio.mp2kContext = NULL;
}

/* --- Atomic API (see libretro-atomic.h) --- */

int atomic_mp2k_available(void) {
	return 1;
}

void atomic_mp2k_set_hook(uint32_t address) {
	mp2kHookAddress = address;
	_mp2kApplyToCore();
}

void atomic_mp2k_set_enabled(int enabled) {
	mp2kEnabled = enabled ? true : false;
}

void atomic_mp2k_reset(void) {
	_mp2kResetState();
}

int atomic_mp2k_is_engaged(void) {
	return mp2kEngaged ? 1 : 0;
}

/* --- Internal hooks for libretro.c (not part of the bridge API) --- */

/* Called from retro_load_game once the new ROM is loaded, before
   the first frame runs. A new game starts with the HLE off, the
   hook disarmed and the state reset: the bridge must call
   atomic_mp2k_set_hook() (with the detected hook address, or
   0xFFFFFFFF for a non-MP2K game) and atomic_mp2k_set_enabled()
   after retro_load_game returns, before the first retro_run — the
   CPU executes no instructions between load and those calls, so
   no stale hook can fire. */
void libretroMp2kOnGameLoaded(void) {
	mp2kGameLoaded = true;
	mp2kHookAddress = MP2K_HOOK_DISABLED;
	mp2kEnabled = false;
	_mp2kResetState();
	_mp2kApplyToCore();
}

/* Called from retro_unload_game. Marks the core gone so the apply
   helper never dereferences the freed core, and drops the
   remembered hook/enabled (the next load starts clean; the bridge
   sets them again after it). */
void libretroMp2kOnGameUnloaded(void) {
	mp2kGameLoaded = false;
	mp2kGba = NULL;
	mp2kHookAddress = MP2K_HOOK_DISABLED;
	mp2kEnabled = false;
	_mp2kResetState();
}

/* Called from retro_reset and from retro_unserialize after a
   successful state load. Same game, so the hook and the enabled
   flag are kept — only the HLE state is reset (NBA's design: the
   HLE is not serialized, it rebuilds from ROM on the next
   SoundMainRAM call; a brief audio glitch on load is expected). */
void libretroMp2kResetState(void) {
	_mp2kResetState();
}
