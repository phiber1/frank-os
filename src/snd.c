/*
 * FRANK OS — Sound Mixer
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Multi-channel ring-buffer mixer with nearest-neighbour resampling.
 * The DMA IRQ handler reads all active channels, resamples to 44100 Hz,
 * mixes (sum + clamp), and fills the DMA ping-pong buffer.  Playback is
 * completely decoupled from task scheduling.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "snd.h"
#include "audio.h"
#include "board_config.h"
#include "tlv320dac3100.h"

#include <string.h>
#include <math.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "FreeRTOS.h"
#include "task.h"

/* DMA chunk size in stereo frames — IRQ fires every ~23 ms at 44100 Hz */
#define SND_DMA_FRAMES   1024

/* Per-channel ring buffer size (must be power of 2) */
#define SND_CHAN_FRAMES   2048
#define SND_CHAN_MASK     (SND_CHAN_FRAMES - 1)

typedef struct {
    int16_t  buf[SND_CHAN_FRAMES * 2];  /* stereo L/R interleaved, 8 KB */
    volatile uint32_t rd;               /* read position  (IRQ increments) */
    volatile uint32_t wr;               /* write position (task increments) */
    uint32_t rate;                      /* source sample rate */
    uint32_t phase;                     /* fixed-point resampling accumulator */
    uint32_t phase_inc;                 /* (source_rate << 16) / 44100 */
    int16_t  hold_l;                    /* last output sample (sample-and-hold) */
    int16_t  hold_r;
    bool     active;
} snd_channel_t;

static snd_channel_t channels[SND_MAX_CHANNELS];

/*==========================================================================
 * Synth voice bank — see snd.h for the API contract.
 *
 * Control fields are written from task context and read by the IRQ; the
 * strike flag defers phase resets to the top of a fill so the IRQ never
 * sees half-initialized state.  Phase accumulators are Q32 at 44100 Hz.
 *==========================================================================*/
typedef struct {
    volatile uint8_t  wave;     /* SNW_* */
    volatile uint8_t  sides;    /* bit0 = L, bit1 = R */
    volatile uint8_t  strike;   /* reset phase state at next fill */
    volatile int32_t  amp;      /* mapping[] taper value, 0..2000 */
    volatile uint32_t inc;      /* Q32 phase inc (SNW_NOISE: period, samples) */
    volatile uint32_t mod;      /* RINGMOD: SNV_OSC | mod start/center Q32 inc;
                                   SIREN: LFO rate Q32 inc */
    volatile uint32_t span;     /* SIREN: sweep span Q32 inc */
    /* IRQ-private state */
    uint32_t acc, cacc, macc, lacc, minc;
    int ndwell, nval;
} snd_synth_voice_t;

#define SNV_OSC 0x80000000u

/* ring-mod constants (Q32 @ 44100): 700Hz carrier; the mod-freq ramp
 * reaches the 4kHz cap over ~250ms; wobble LFO = half the gate rate */
#define SNV_CARRIER    68173766u   /* 700Hz */
#define SNV_MOD_MAX   389564380u   /* 4kHz */
#define SNV_MOD_RAMP      15872u   /* per-sample inc growth */

static snd_synth_voice_t synthv[SND_SYNTH_VOICES];
static int16_t snd_sintab[64];    /* 64 entries: SRAM is precious here */

static inline uint32_t snd_prng(void) {
    static uint32_t s = 0x2F6E2B1u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

/* Global volume as right-shift (0 = max, 4 = mute).
 * Volatile because snd_fill_dma reads it from DMA IRQ context
 * while snd_set_volume writes it from task context. */
static volatile uint8_t snd_volume = 0;

static i2s_config_t snd_i2s_config;

/*==========================================================================
 * snd_fill_dma — called from DMA IRQ to mix all channels into one buffer
 *
 * Runs in IRQ context — must be fast and must not block.
 * Placed in RAM so it works even during flash operations.
 *==========================================================================*/
static void __not_in_flash_func(snd_fill_dma)(int buf_index,
                                               uint32_t *buf,
                                               uint32_t frames) {
    (void)buf_index;
    int16_t *out = (int16_t *)buf;

    /* Latch synth strikes: phase resets requested from task context are
     * applied here, before any synthesis, so a fresh strike always opens
     * with its gate on and its modulator at the programmed start. */
    for (int v = 0; v < SND_SYNTH_VOICES; v++) {
        snd_synth_voice_t *s = &synthv[v];
        if (s->strike) {
            s->acc = s->cacc = s->macc = s->lacc = 0;
            s->minc = s->mod & ~SNV_OSC;
            s->ndwell = 0;
            s->strike = 0;
        }
    }

    /* Snapshot available frames per channel (wr only changes from task
     * context, rd only from here — both are stable for this fill). */
    uint32_t ch_avail[SND_MAX_CHANNELS];
    for (int ch = 0; ch < SND_MAX_CHANNELS; ch++) {
        if (channels[ch].active)
            ch_avail[ch] = channels[ch].wr - channels[ch].rd;
        else
            ch_avail[ch] = 0;
    }

    for (uint32_t i = 0; i < frames; i++) {
        int32_t left = 0, right = 0;

        for (int ch = 0; ch < SND_MAX_CHANNELS; ch++) {
            snd_channel_t *c = &channels[ch];
            if (!c->active) continue;

            if (ch_avail[ch] == 0) {
                /* Buffer empty — hold last sample to avoid DC-offset click */
                left  += c->hold_l;
                right += c->hold_r;
                continue;
            }

            uint32_t src_idx = c->phase >> 16;
            if (src_idx >= ch_avail[ch]) {
                /* Exhausted available data — hold last sample */
                left  += c->hold_l;
                right += c->hold_r;
                continue;
            }

            uint32_t idx0 = (c->rd + src_idx) & SND_CHAN_MASK;
            int16_t l0 = c->buf[idx0 * 2];
            int16_t r0 = c->buf[idx0 * 2 + 1];

            int16_t sl, sr;
            if (src_idx + 1 < ch_avail[ch]) {
                /* Linear interpolation between current and next sample */
                uint32_t idx1 = (c->rd + src_idx + 1) & SND_CHAN_MASK;
                int32_t frac = (int32_t)((c->phase & 0xFFFF) >> 1);
                sl = l0 + (((int32_t)(c->buf[idx1 * 2]     - l0) * frac) >> 15);
                sr = r0 + (((int32_t)(c->buf[idx1 * 2 + 1] - r0) * frac) >> 15);
            } else {
                sl = l0;
                sr = r0;
            }

            c->hold_l = sl;
            c->hold_r = sr;
            left  += sl;
            right += sr;

            c->phase += c->phase_inc;
        }

        /* Synth voices — see snd.h.  wave values are +/-1900 around 0,
         * contribution = wave * gain / 128, the MMBasic loudness
         * pipeline.  The gain SLEWS toward its target (~1.4ms full
         * scale) so no on/off/volume change can step the output — a
         * PLAY SOUND ...,"O" cutting a 60Hz square mid-swing was a
         * full-amplitude click ("twang"), worst when late off-timers
         * landed on still-loud voices during heavy frames. */
        for (int v = 0; v < SND_SYNTH_VOICES; v++) {
            snd_synth_voice_t *s = &synthv[v];
            int wv = s->wave;
            if (wv == SNW_OFF) continue;
            int camp = s->amp;
            int w;
            switch (wv) {
            case SNW_SQUARE:
                w = (s->acc & 0x80000000u) ? 1900 : -1900;
                s->acc += s->inc;
                break;
            case SNW_TRIANGLE: {
                int t = (int)(s->acc >> 19) & 8191;
                t = t < 4096 ? t : 8191 - t;
                w = ((t - 2048) * 1900) >> 11;
                s->acc += s->inc;
                break; }
            case SNW_SAW:
                w = (((int)(s->acc >> 20) - 2048) * 1900) >> 11;
                s->acc += s->inc;
                break;
            case SNW_SINE:
                w = snd_sintab[s->acc >> 26];
                s->acc += s->inc;
                break;
            case SNW_NOISE:
                if (--s->ndwell <= 0) {
                    s->ndwell = (int)s->inc;
                    s->nval = (int)(snd_prng() % 3801) - 1900;
                }
                w = s->nval;
                break;
            case SNW_GATED_NOISE:
                if (--s->ndwell <= 0) {
                    s->ndwell = 6;   /* ~7kHz change rate */
                    s->nval = (int)(snd_prng() % 3801) - 1900;
                }
                w = (s->acc & 0x80000000u) ? 0 : s->nval;
                s->acc += s->inc;
                break;
            case SNW_THUMP: {
                /* drum hit with body: sine falls from 2x to 1x pitch in
                 * ~12ms (the "chest" attack), then sustains — nominal
                 * decay ~180ms, so the caller's off ends it in time
                 * with whatever it accompanies */
                uint32_t l = s->lacc;
                if (l >= 65536) { w = 0; break; }   /* hit spent */
                /* a deep voice clearing its throat WITH THE MOUTH
                 * CLOSED: dominant slow rough noise (~260 changes/sec)
                 * run through a one-pole low-pass (~220Hz) — gliding
                 * between held values instead of stepping kills the
                 * edge brightness = muffled.  Faint triangle under it. */
                if (--s->ndwell <= 0) {
                    s->ndwell = 170;  /* ~260 changes/sec */
                    s->nval = (int)(snd_prng() % 3801) - 1900;
                }
                int cur = (int)s->macc;
                cur += (s->nval - cur) >> 5;   /* LPF ~220Hz: deeper */
                s->macc = (uint32_t)cur;
                int tp = (int)(s->acc >> 19) & 8191;
                tp = tp < 4096 ? tp : 8191 - tp;
                /* LINEAR decay over ~50ms: loudest at the strike (the
                 * attack accent), fading steadily but keeping body */
                int e = (int)(65536 - l);
                w = cur + (((tp - 2048) * 475) >> 11);
                w = (w * e) >> 16;
                s->acc += s->minc;
                s->minc -= (s->minc - s->inc) >> 9;
                s->lacc = l + 30;   /* ~50ms to silence */
                break; }
            case SNW_SIREN: {
                /* square sweeping base..base+span on a per-sample
                 * triangle LFO — smooth wail, no frame-rate staircase */
                int t = (int)(s->lacc >> 19) & 8191;
                t = t < 4096 ? t : 8191 - t;
                uint32_t mi = s->inc + (uint32_t)(((uint64_t)s->span * (uint32_t)t) >> 12);
                /* 25% duty pulse, not 50% square: narrow pulses push
                 * energy into the upper harmonics — shriller, sharper */
                w = ((s->acc >> 24) < 64) ? 1900 : -1900;
                s->acc += mi;
                s->lacc += s->mod;
                break; }
            default: {  /* SNW_RINGMOD */
                int cbit = (int)(s->cacc >> 31), mbit = (int)(s->macc >> 31);
                w = (s->acc & 0x80000000u) ? 0 : ((cbit ^ mbit) ? 1900 : -1900);
                s->cacc += SNV_CARRIER;
                s->acc  += s->inc;
                uint32_t ci = s->minc;
                if (s->mod & SNV_OSC) {
                    int t = (int)(s->lacc >> 19) & 8191;
                    t = t < 4096 ? t : 8191 - t;
                    s->macc += (ci >> 1) + (uint32_t)(((uint64_t)ci * (uint32_t)t) >> 12);
                    s->lacc += s->inc >> 1;
                    if (ci < SNV_MOD_MAX) s->minc = ci + (SNV_MOD_RAMP >> 1);
                } else {
                    s->macc += ci;
                    if (ci < SNV_MOD_MAX) s->minc = ci + SNV_MOD_RAMP;
                }
                break; }
            }
            int32_t con = (w * camp) >> 7;
            int sd = s->sides;
            if (sd & 1) left  += con;
            if (sd & 2) right += con;
        }

        /* Attenuate mix to prevent clipping when multiple channels
         * are active, then apply volume control. */
        left  >>= 1;
        right >>= 1;

        if (snd_volume >= 4) {
            left  = 0;
            right = 0;
        } else if (snd_volume) {
            left  >>= snd_volume;
            right >>= snd_volume;
        }

        /* Clamp to int16_t range */
        if (left  >  32767) left  =  32767;
        if (left  < -32768) left  = -32768;
        if (right >  32767) right =  32767;
        if (right < -32768) right = -32768;

        out[i * 2]     = (int16_t)left;
        out[i * 2 + 1] = (int16_t)right;
    }

    /* Advance read pointers by the number of source frames consumed */
    for (int ch = 0; ch < SND_MAX_CHANNELS; ch++) {
        snd_channel_t *c = &channels[ch];
        if (ch_avail[ch] == 0) continue;

        uint32_t consumed = c->phase >> 16;
        c->rd    += consumed;
        c->phase &= 0xFFFF;  /* keep fractional part */
    }
}

/*==========================================================================
 * snd_init — start I2S at 44100 Hz, DMA plays silence until channels open
 *==========================================================================*/
void snd_init(void) {
    memset(channels, 0, sizeof(channels));
    memset(synthv, 0, sizeof(synthv));
    for (int i = 0; i < 64; i++)
        snd_sintab[i] = (int16_t)(1900.0f * sinf((float)i * 6.2831853f / 64.0f));

    memset(&snd_i2s_config, 0, sizeof(snd_i2s_config));
    snd_i2s_config.sample_freq    = SND_SYSTEM_RATE;
    snd_i2s_config.channel_count  = 2;
    snd_i2s_config.data_pin       = I2S_DATA_PIN;
    snd_i2s_config.clock_pin_base = I2S_CLOCK_PIN_BASE;
#ifdef FRANK_BOARD_FRUITJAM
    /* Fruit Jam: PIO0/PIO1 belong exclusively to PIO-USB (it assumes sole
     * ownership of its PIOs); audio shares PIO2 with the netcard UART. */
    snd_i2s_config.pio            = pio2;
#else
    snd_i2s_config.pio            = pio1;
#endif
    snd_i2s_config.dma_trans_count = SND_DMA_FRAMES;
    snd_i2s_config.volume         = 0;

    i2s_init(&snd_i2s_config);
    i2s_set_fill_callback(snd_fill_dma);
    i2s_start();

    /* Fruit Jam: the TLV320DAC3100 codec derives its clocks from the now-
     * running BCLK and must be configured over I2C (no-op on other boards) */
    tlv320dac3100_init();
}

/*==========================================================================
 * snd_synth — program one synth voice (task context; see snd.h)
 *==========================================================================*/
void snd_synth(int voice, int sides, int wave, uint32_t freq_mhz,
               uint32_t mod_hz, int amp) {
    if (voice < 0 || voice >= SND_SYNTH_VOICES) return;
    snd_synth_voice_t *s = &synthv[voice];
    if (wave <= SNW_OFF || wave > SNW_THUMP) {
        s->wave = SNW_OFF;
        return;
    }
    uint32_t inc;
    if (wave == SNW_NOISE) {
        /* freq is the change rate: store the dwell period in samples */
        uint32_t rate = freq_mhz / 1000;
        if (rate < 1) rate = 1;
        inc = SND_SYSTEM_RATE / rate;
        if (inc < 1) inc = 1;
    } else {
        inc = (uint32_t)(((uint64_t)freq_mhz << 32) / (SND_SYSTEM_RATE * 1000ull));
    }
    uint32_t mod = 0;
    if (wave == SNW_THUMP)
        mod = inc << 1;   /* strike pitch = 2x the floor; the strike
                             loads it into minc via the reset path */
    if (wave == SNW_SIREN) {
        uint32_t rate = mod_hz ? (mod_hz >> 8) : 8;
        uint32_t span_hz = mod_hz ? (mod_hz & 255) * 16 : 800;
        if (rate < 1) rate = 1;
        if (rate > 30) rate = 30;
        mod = (uint32_t)(((uint64_t)rate << 32) / SND_SYSTEM_RATE);
        s->span = (uint32_t)(((uint64_t)span_hz << 32) / SND_SYSTEM_RATE);
    }
    if (wave == SNW_RINGMOD) {
        uint32_t hz = mod_hz;
        if (hz >= 5000)
            mod = SNV_OSC | (uint32_t)(((uint64_t)(hz - 5000) << 32) / SND_SYSTEM_RATE);
        else {
            if (hz < 1) hz = 200;
            mod = (uint32_t)(((uint64_t)hz << 32) / SND_SYSTEM_RATE);
        }
    }
    int fresh = (s->wave != wave) || wave == SNW_THUMP;
    s->inc = inc;
    s->mod = mod;
    s->amp = amp;
    s->sides = (uint8_t)(sides & 3);
    if (fresh) s->strike = 1;   /* before wave: the IRQ latches strikes
                                   at fill start, gating on wave */
    s->wave = (uint8_t)wave;
}

/*==========================================================================
 * snd_open — allocate a mixing channel
 *==========================================================================*/
int snd_open(uint32_t sample_rate) {
    for (int ch = 0; ch < SND_MAX_CHANNELS; ch++) {
        if (!channels[ch].active) {
            snd_channel_t *c = &channels[ch];
            memset(c->buf, 0, sizeof(c->buf));
            c->rd        = 0;
            c->wr        = 0;
            c->rate      = sample_rate;
            c->phase     = 0;
            c->phase_inc = (sample_rate << 16) / SND_SYSTEM_RATE;
            c->hold_l    = 0;
            c->hold_r    = 0;
            c->active    = true;
            return ch;
        }
    }
    return -1;  /* all channels in use */
}

/*==========================================================================
 * snd_write — copy stereo frames into a channel's ring buffer
 *
 * Blocks (vTaskDelay) if the ring buffer is full.  Handles wrap-around
 * with up to two memcpy calls per iteration.
 *==========================================================================*/
void snd_write(int ch, const int16_t *samples, int frames) {
    if (ch < 0 || ch >= SND_MAX_CHANNELS) return;
    snd_channel_t *c = &channels[ch];

    while (frames > 0) {
        uint32_t used  = c->wr - c->rd;
        uint32_t space = SND_CHAN_FRAMES - used;

        if (space == 0) {
            vTaskDelay(1);
            continue;
        }

        uint32_t n = (uint32_t)frames;
        if (n > space) n = space;

        uint32_t wr_idx = c->wr & SND_CHAN_MASK;
        uint32_t first  = SND_CHAN_FRAMES - wr_idx;
        if (first > n) first = n;

        /* Each stereo frame = 2 x int16_t = 4 bytes */
        memcpy(&c->buf[wr_idx * 2], samples, first * 4);
        if (n > first) {
            memcpy(&c->buf[0], samples + first * 2, (n - first) * 4);
        }

        __dmb();     /* ensure sample data visible before wr update */
        c->wr += n;

        samples += n * 2;
        frames  -= (int)n;
    }
}

/*==========================================================================
 * snd_close — release a mixing channel
 *==========================================================================*/
void snd_close(int ch) {
    if (ch < 0 || ch >= SND_MAX_CHANNELS) return;
    channels[ch].active = false;
    channels[ch].rd     = 0;
    channels[ch].wr     = 0;
    channels[ch].phase  = 0;
}

/*==========================================================================
 * snd_deinit — shut down the entire sound system (I2S + DMA)
 *==========================================================================*/
void snd_deinit(void) {
    for (int ch = 0; ch < SND_MAX_CHANNELS; ch++)
        channels[ch].active = false;
    for (int v = 0; v < SND_SYNTH_VOICES; v++)
        synthv[v].wave = SNW_OFF;
    i2s_deinit(&snd_i2s_config);
}

uint8_t snd_get_volume(void) {
    return snd_volume;
}

void snd_set_volume(uint8_t vol) {
    if (vol > 4) vol = 4;
    snd_volume = vol;
}
