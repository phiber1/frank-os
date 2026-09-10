/*
 * FRANK OS — Sound Mixer
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Multi-channel mixing with per-channel resampling.  DMA IRQ pulls from
 * ring buffers so buffered audio keeps playing even when tasks stall.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>

#define SND_MAX_CHANNELS  4
#define SND_SYSTEM_RATE   44100

/* Call once at boot — starts I2S, DMA plays silence */
void snd_init(void);

/* Open a sound channel.  Returns channel ID (0-3) or -1 if full.
 * sample_rate: source sample rate (e.g. 15625, 22050, 44100) */
int  snd_open(uint32_t sample_rate);

/* Write stereo interleaved frames to a channel.  Blocks if ring buffer full. */
void snd_write(int ch, const int16_t *samples, int frames);

/* Close a channel, freeing it for reuse. */
void snd_close(int ch);

/* Shut down the entire sound system (stops I2S DMA + PIO). */
void snd_deinit(void);

/* Volume control — right-shift value (0 = max, 4 = muted).
 * snd_get_volume() returns the current value.
 * snd_set_volume() clamps to 0-4. */
uint8_t snd_get_volume(void);
void    snd_set_volume(uint8_t vol);

/*==========================================================================
 * Synth voice bank — OS-side "sound chip" (syscall 559)
 *
 * Eight continuous voices synthesized inside the DMA IRQ (SRAM code, zero
 * cost to apps).  Motivation: apps execute from uncached PSRAM, where an
 * app-side synth voice costs ~10 fps of game time in QMI instruction
 * fetches (measured; see the MMBasic PLAY SOUND pump).
 *
 * wave types (SNW_*):
 *   OFF          voice silent
 *   SQUARE/TRIANGLE/SAW/SINE  classic oscillators at freq_mhz (milli-Hz)
 *   NOISE        white noise; freq_mhz = change rate in mHz
 *   GATED_NOISE  noise chopped by a square gate at freq_mhz
 *   RINGMOD      700Hz square ring-modulated (XOR) by a swept square,
 *                gated at freq_mhz.  mod_hz encodes the modulator:
 *                < 5000  = rising-ramp sweep starting at mod_hz Hz
 *                          (0 -> 200Hz default)
 *                >= 5000 = wobble: mod swings +/-50% around
 *                          (mod_hz-5000) Hz, LFO at half the gate rate,
 *                          center climbing gently; ramp cap 4kHz
 *   SIREN        square sweeping from freq_mhz upward on a per-sample
 *                triangle LFO — a smooth wail.  mod_hz: 0 = default
 *                (8Hz wail, 800Hz span), else bit-packed:
 *                rate = mod_hz >> 8 (Hz, clamped 1-30),
 *                span = (mod_hz & 255) * 16 Hz
 *   THUMP        one-shot drum hit: a sine striking at 4x freq_mhz,
 *                pitch dropping to freq_mhz in ~12ms, amplitude dying
 *                in ~70ms, then silent.  Every set re-strikes.
 *
 * sides: bit0 = left, bit1 = right.
 * amp:   PicoMite mapping[] taper value (0..2000 scale); the per-sample
 *        contribution is wave(+/-1900) * amp / 128, matching the MMBasic
 *        PLAY SOUND loudness pipeline exactly.
 * A wave-type change (incl. OFF -> on) restarts the voice phases (gate
 * opens, ring-mod sweep restarts); volume/freq-only updates keep phase.
 *==========================================================================*/
enum {
    SNW_OFF = 0, SNW_SQUARE, SNW_TRIANGLE, SNW_SAW, SNW_SINE,
    SNW_NOISE, SNW_GATED_NOISE, SNW_RINGMOD, SNW_SIREN, SNW_THUMP
};
#define SND_SYNTH_VOICES 8

void snd_synth(int voice, int sides, int wave, uint32_t freq_mhz,
               uint32_t mod_hz, int amp);
