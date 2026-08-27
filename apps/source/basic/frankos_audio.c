/*
 * frankos_audio.c — MMBasic audio for Frank OS
 *
 * Replaces PicoMite's Audio.c (PWM/I2S hardware + IRQ synthesis, not
 * compiled here) with an implementation on top of the Frank OS sound
 * mixer (sys_table pcm_init/pcm_write/pcm_cleanup).  Architecture:
 *
 *   • A dedicated FreeRTOS "audio pump" task (created lazily on first
 *     use) owns the OS mixer channel and generates samples: tone and
 *     PLAY SOUND voices are synthesised at 44100 Hz with PicoMite's
 *     exact wavetable/phase-accumulator math; file playback decodes
 *     incrementally.  pcm_write blocks when the mixer ring is full,
 *     which paces the pump for free.
 *
 *   • File playback (WAV/MP3/FLAC/MODFILE) loads the COMPRESSED file
 *     into PSRAM on the interpreter task (FatFS is not reentrant, so
 *     the pump never touches the filesystem), then the pump decodes
 *     from memory: dr_wav/dr_mp3/dr_flac in memory mode, hxcmod for
 *     MOD.  Memory cost is the file size, not the PCM size, so full
 *     songs work.
 *
 *   • Completion signalling matches PicoMite: WAVcomplete=true fires
 *     the BASIC interrupt routine (via MM_Misc) set with the optional
 *     interrupt argument.
 *
 * Supported:  PLAY TONE / SOUND / WAV / MP3 / FLAC / MODFILE /
 *             STOP / PAUSE / RESUME / CLOSE / VOLUME / LOAD SOUND
 * Unsupported (error out): ARRAY, SAMPLE, STREAM, NOTE, MIDI, NEXT,
 *             PREVIOUS, MODSAMPLE, album-directory playback.
 *
 * Synthesis math, tables and grammar are from PicoMite MMBasic
 * (Audio.c / mmc_stm32.c), Copyright (c) 2021 Geoff Graham, Peter
 * Mather, BSD-3-Clause.  Frank OS glue Copyright (c) 2026 Mikhail
 * Matveev <xtreme@rh1.tech>.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico/stdlib.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"   /* pulls in Audio.h, ff.h */

/* dr_* decoders (WAV/FLAC), memory mode only (no stdio).  MP3 and MOD
 * decode via the OS's helix / HxCModPlayer syscalls instead: OS-resident
 * code runs from fast flash, while in-app decoders execute from PSRAM
 * and cannot hold real time (measured: the frankamp app plays both
 * formats perfectly through these same OS decoders). */
#define DR_WAV_NO_STDIO
#define DR_FLAC_NO_STDIO
#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include "dr_wav.h"
#include "dr_flac.h"

/* OS HxCModPlayer context/type layout (matches the engine behind
 * syscalls 486-490; NOT the PicoMite hxcmod bundled with this app) */
#include "../../../api/hxcmod.h"

#include "frankos_audio_tables.h"    /* mapping[], triangletable[], SineTable[] */

/* ════════════════════════════════════════════════════════════════════════
 * Frank OS sys_table access (see frankos_libc.c for the pattern; we do
 * not include m-os-api.h because of its stdio macro conflicts)
 * ════════════════════════════════════════════════════════════════════════ */
#ifndef _FRANK_SYS_TABLE_BASE
#define _FRANK_SYS_TABLE_BASE ((void * const *)(0x10000000ul + (16ul << 20) - (4ul << 10)))
#endif
static void * const * const _ost = _FRANK_SYS_TABLE_BASE;
#define OST(N, T) ((T)_ost[(N)])

static void os_pcm_init(int rate, int channels) {
    typedef void (*fn)(int, int); OST(449, fn)(rate, channels); }
static void os_pcm_write(const int16_t *samples, int frames) {
    typedef void (*fn)(const int16_t *, int); OST(450, fn)(samples, frames); }
static void os_pcm_cleanup(void) {
    typedef void (*fn)(void); OST(198, fn)(); }
static void *os_psram_alloc(size_t size) {
    typedef void *(*fn)(size_t); return OST(491, fn)(size); }
static void os_psram_free(void *p) {
    typedef void (*fn)(void *); OST(492, fn)(p); }

extern void vTaskDelay(uint32_t ticks);          /* frankos_libc.c, 1ms tick */

/* helix MP3 decoder syscalls (443-448) */
typedef void *HMP3Decoder;
typedef struct {
    int bitrate, nChans, samprate, bitsPerSample;
    int outputSamps, layer, version;
} MP3FrameInfo;
static HMP3Decoder os_mp3_init(void) {
    typedef HMP3Decoder (*fn)(void); return OST(443, fn)(); }
static void os_mp3_free(HMP3Decoder h) {
    typedef void (*fn)(HMP3Decoder); OST(444, fn)(h); }
static int os_mp3_sync(unsigned char *buf, int n) {
    typedef int (*fn)(unsigned char *, int); return OST(445, fn)(buf, n); }
static int os_mp3_decode(HMP3Decoder h, unsigned char **in, int *left, short *out) {
    typedef int (*fn)(HMP3Decoder, unsigned char **, int *, short *, int);
    return OST(446, fn)(h, in, left, out, 0); }
static void os_mp3_lastinfo(HMP3Decoder h, MP3FrameInfo *fi) {
    typedef void (*fn)(HMP3Decoder, MP3FrameInfo *); OST(447, fn)(h, fi); }
static int os_mp3_nextinfo(HMP3Decoder h, MP3FrameInfo *fi, unsigned char *buf) {
    typedef int (*fn)(HMP3Decoder, MP3FrameInfo *, unsigned char *);
    return OST(448, fn)(h, fi, buf); }

/* OS HxCModPlayer syscalls (486-490) */
static int os_hxcmod_init(modcontext *c) {
    typedef int (*fn)(modcontext *); return OST(486, fn)(c); }
static int os_hxcmod_setcfg(modcontext *c, int rate, int sep, int filt) {
    typedef int (*fn)(modcontext *, int, int, int);
    return OST(487, fn)(c, rate, sep, filt); }
static int os_hxcmod_load(modcontext *c, void *d, int n) {
    typedef int (*fn)(modcontext *, void *, int); return OST(488, fn)(c, d, n); }
static void os_hxcmod_fill(modcontext *c, msample *out, mssize n) {
    typedef void (*fn)(modcontext *, msample *, mssize, void *);
    OST(489, fn)(c, out, n, (void *)0); }
static void os_hxcmod_unload(modcontext *c) {
    typedef void (*fn)(modcontext *); OST(490, fn)(c); }

/* PRNG for the noise generators — newlib rand() isn't in the app image
 * (non-static: satisfies the stdlib.h declaration for any caller) */
static uint32_t rng_state = 0x12345678;
int rand(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (int)(rng_state & 0x7fffffff);
}

static int os_task_create(void (*code)(void *), const char *name,
                          uint32_t stack_words, uint32_t prio) {
    typedef int32_t (*fn)(void (*)(void *), const char *, uint32_t,
                          void *, uint32_t, void **);
    return (int)OST(0, fn)(code, name, stack_words, NULL, prio, NULL);
}

/* ════════════════════════════════════════════════════════════════════════
 * PicoMite audio state (definitions for the externs in Audio.h)
 * ════════════════════════════════════════════════════════════════════════ */
const char *const PlayingStr[] = {
    "PAUSED TONE", "PAUSED FLAC", "PAUSED MP3", "PAUSED SOUND",
    "PAUSED MOD", "PAUSED ARRAY", "PAUSED WAV", "PAUSED SAMPLE", "OFF",
    "OFF", "TONE", "SOUND", "WAV", "FLAC", "MP3",
    "MIDI", "", "MOD", "STREAM", "ARRAY", "SAMPLE", ""};

volatile e_CurrentlyPlaying CurrentlyPlaying = P_NOTHING;
volatile int vol_left = 100, vol_right = 100;
volatile float PhaseM_left, PhaseM_right;
volatile float PhaseAC_left, PhaseAC_right;
volatile uint64_t SoundPlay;
volatile uint8_t mono;
int PWM_FREQ = 44100;
char *WAVInterrupt = NULL;
bool WAVcomplete = false;
int WAV_fnbr = 0;
char *modbuff = NULL;                 /* referenced by core code */

/* wavetable markers (see getsound(): 97 = silence, 98 = saw, 99 = square) */
extern const unsigned short nulltable[1];

volatile int sound_v_left[MAXSOUNDS]  = {[0 ... MAXSOUNDS - 1] = 25};
volatile int sound_v_right[MAXSOUNDS] = {[0 ... MAXSOUNDS - 1] = 25};
volatile float sound_PhaseAC_left[MAXSOUNDS], sound_PhaseAC_right[MAXSOUNDS];
volatile float sound_PhaseM_left[MAXSOUNDS], sound_PhaseM_right[MAXSOUNDS];
/* parked at RUNTIME by audio_lazy_init() — static pointer initializers
 * are not relocated by the Frank OS ELF loader, so a build-time
 * "= nulltable" here would leave link-time garbage addresses that the
 * synth then reads as wavetables (the DC-rumble bug) */
volatile unsigned short *sound_mode_left[MAXSOUNDS];
volatile unsigned short *sound_mode_right[MAXSOUNDS];

const unsigned short nulltable[1]   = {97};
const unsigned short squaretable[1] = {99};
const unsigned short sawtable[1]    = {98};
const unsigned short whitenoise[2]  = {0};    /* matched by pointer */
unsigned short *noisetable = NULL;            /* periodic noise, setnoise() */
unsigned short *usertable  = NULL;            /* PLAY LOAD SOUND */

/* ════════════════════════════════════════════════════════════════════════
 * Pump ↔ interpreter shared state
 * ════════════════════════════════════════════════════════════════════════ */
#define PUMP_CHUNK 1024                      /* frames per pump iteration (23ms) */

typedef enum { SRC_NONE, SRC_WAV, SRC_MP3, SRC_FLAC, SRC_MOD } src_t;

static volatile src_t   src_kind = SRC_NONE;
static volatile int     src_rate;            /* mixer channel rate for file src */
static volatile uint8_t src_channels;        /* 1 or 2 */
static volatile uint8_t mod_noloop;
static volatile uint8_t pump_idle = 1;       /* pump is parked, channel closed */
static volatile uint8_t pump_created = 0;
static volatile uint8_t pump_kill = 0;       /* app exiting: pump self-deletes */

static uint8_t   *file_buf = NULL;           /* compressed file image (PSRAM) */
static drwav      wav_dec;
static drflac    *flac_dec = NULL;
static HMP3Decoder mp3_hdec = NULL;          /* OS helix decoder handle */
static uint8_t   *mp3_ptr;                   /* decode cursor into file_buf */
static int        mp3_left;
static modcontext *mod_ctx = NULL;           /* PSRAM, reused across plays */
static unsigned    mod_prev_tablepos;        /* song-wrap detection (noloop) */

/* ════════════════════════════════════════════════════════════════════════
 * Synthesis (verbatim PicoMite math, 44100 Hz)
 * ════════════════════════════════════════════════════════════════════════ */

/* PLAY TONE chunk synthesis — integer Q12 phase accumulators (app-side
 * float ops are library calls costing ~3700 cycles each frame; fixed
 * point runs the whole frame in ~20 cycles).  Returns frames produced;
 * fewer than requested means the duration expired. */
static int synth_tone_chunk(int16_t *buf, int nframes) {
    uint32_t accl = (uint32_t)(PhaseAC_left  * 4096.0f) & 0xFFFFFF;
    uint32_t accr = (uint32_t)(PhaseAC_right * 4096.0f) & 0xFFFFFF;
    uint32_t incl = (uint32_t)(PhaseM_left  * 4096.0f);
    uint32_t incr = (uint32_t)(PhaseM_right * 4096.0f);
    int ml = mapping[vol_left], mr = mapping[vol_right];
    int is_mono = mono;
    int n = 0;
    while (n < nframes) {
        if (!SoundPlay) break;
        SoundPlay--;
        int li = ((int)SineTable[accl >> 12] - 2000) * ml / 128;
        accl = (accl + incl) & 0xFFFFFF;         /* 4096.0 in Q12, mask wrap */
        int ri;
        if (is_mono) {
            accr = accl;
            ri = li;
        } else {
            ri = ((int)SineTable[accr >> 12] - 2000) * mr / 128;
            accr = (accr + incr) & 0xFFFFFF;
        }
        buf[n * 2]     = (int16_t)li;
        buf[n * 2 + 1] = (int16_t)ri;
        n++;
    }
    PhaseAC_left  = (float)accl / 4096.0f;
    PhaseAC_right = (float)accr / 4096.0f;
    return n;
}

/* PLAY SOUND chunk synthesis (4 voices).  The channel state lives in
 * volatile globals (interpreter-writable); reading them per-sample cost
 * ~1.8x real time and caused audible underrun warble — so hoist to
 * locals for the whole chunk and write the phases back afterwards. */
static void synth_sound_chunk(int16_t *buf, int nframes) {
    static int noisedwell[MAXSOUNDS * 2];
    static int noiseval[MAXSOUNDS * 2];

    /* Build the active-voice list ONCE per chunk.  The naive form walked
     * all 4 channels x 2 sides every frame; the big branchy loop body
     * fetched from PSRAM and cost ~22ms/chunk (95% CPU).  With the list,
     * each active voice runs a small tone-sized inner loop. */
    struct av {
        const unsigned short *t;   /* wavetable (or marker) */
        uint32_t acc, inc;         /* Q12 phase */
        int vol;                   /* mapping[] value */
        uint8_t chan, right, white;
    } av[MAXSOUNDS * 2];
    int nact = 0;
    for (int i = 0; i < MAXSOUNDS; i++) {
        if (sound_mode_left[i] != nulltable) {
            struct av *a = &av[nact++];
            a->t = (const unsigned short *)sound_mode_left[i];
            a->acc = (uint32_t)(sound_PhaseAC_left[i] * 4096.0f) & 0xFFFFFF;
            a->inc = (uint32_t)(sound_PhaseM_left[i] * 4096.0f);
            a->vol = mapping[sound_v_left[i]];
            a->chan = i; a->right = 0;
            a->white = (a->t == whitenoise);
        }
        if (sound_mode_right[i] != nulltable) {
            struct av *a = &av[nact++];
            a->t = (const unsigned short *)sound_mode_right[i];
            a->acc = (uint32_t)(sound_PhaseAC_right[i] * 4096.0f) & 0xFFFFFF;
            a->inc = (uint32_t)(sound_PhaseM_right[i] * 4096.0f);
            a->vol = mapping[sound_v_right[i]];
            a->chan = i; a->right = 1;
            a->white = (a->t == whitenoise);
        }
    }

    memset(buf, 0, nframes * 2 * sizeof(int16_t));
    if (nact == 0) return;

    /* accumulate voices in int32 per side, clip once at the end */
    static int32_t mix[2][PUMP_CHUNK];      /* [0]=L, [1]=R */
    memset(mix, 0, sizeof(mix[0][0]) * 2 * nframes);

    for (int k = 0; k < nact; k++) {
        struct av *a = &av[k];
        int32_t *m = mix[a->right];
        int vol = a->vol;
        if (a->white) {
            int di = a->chan * 2 + a->right;
            int dwell = noisedwell[di], nv = noiseval[di];
            int period = (int)(a->inc >> 12);
            for (int n = 0; n < nframes; n++) {
                if (dwell <= 0) { dwell = period; nv = rand() % 3800 + 100; }
                if (dwell) dwell--;
                m[n] += (nv - 2000) * vol / 2000;
            }
            noisedwell[di] = dwell; noiseval[di] = nv;
        } else if (a->t[0] == 99) {          /* square marker */
            uint32_t acc = a->acc, inc = a->inc;
            for (int n = 0; n < nframes; n++) {
                int j = ((acc >> 12) > 2047) ? 3900 : 100;
                acc = (acc + inc) & 0xFFFFFF;
                m[n] += (j - 2000) * vol / 2000;
            }
            a->acc = acc;
        } else if (a->t[0] == 98) {          /* sawtooth marker */
            uint32_t acc = a->acc, inc = a->inc;
            for (int n = 0; n < nframes; n++) {
                int j = (int)(acc >> 12) * 3800 / 4096 + 100;
                acc = (acc + inc) & 0xFFFFFF;
                m[n] += (j - 2000) * vol / 2000;
            }
            a->acc = acc;
        } else {                              /* real wavetable */
            const unsigned short *t = a->t;
            uint32_t acc = a->acc, inc = a->inc;
            for (int n = 0; n < nframes; n++) {
                m[n] += ((int)t[acc >> 12] - 2000) * vol / 2000;
                acc = (acc + inc) & 0xFFFFFF;
            }
            a->acc = acc;
        }
    }

    /* interleave, scale (PicoMite's 32-bit I2S value >> 16), clip */
    for (int n = 0; n < nframes; n++) {
        int l = mix[0][n] * 2000 / 128;
        int r = mix[1][n] * 2000 / 128;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        buf[n * 2]     = (int16_t)l;
        buf[n * 2 + 1] = (int16_t)r;
    }

    /* write phases back to the volatile API globals */
    for (int k = 0; k < nact; k++) {
        float ph = (float)av[k].acc / 4096.0f;
        if (av[k].right) sound_PhaseAC_right[av[k].chan] = ph;
        else             sound_PhaseAC_left[av[k].chan]  = ph;
    }
}

/* ════════════════════════════════════════════════════════════════════════
 * Audio pump task
 * ════════════════════════════════════════════════════════════════════════ */
static int16_t pump_buf[PUMP_CHUNK * 2];     /* interleaved stereo out */
/* decoder scratch — must hold max(PUMP_CHUNK frames, one MP3 frame of
 * 1152 samples x 2ch) */
static int16_t dec_buf[2304];

/* decode up to PUMP_CHUNK frames from the current file source into
 * pump_buf (stereo, volume applied); returns frames produced, 0 at EOF */
static int decode_chunk(void) {
    int frames = 0, ch = src_channels;
    switch (src_kind) {
    case SRC_WAV:
        frames = (int)drwav_read_pcm_frames_s16(&wav_dec, PUMP_CHUNK, dec_buf);
        break;
    case SRC_MP3: {
        /* helix decodes one MP3 frame (up to 1152 frames of PCM) */
        for (int tries = 0; tries < 4; tries++) {
            if (mp3_left < 16) return 0;              /* end of stream */
            int off = os_mp3_sync(mp3_ptr, mp3_left);
            if (off < 0) return 0;
            mp3_ptr += off; mp3_left -= off;
            if (os_mp3_decode(mp3_hdec, &mp3_ptr, &mp3_left, dec_buf) == 0) {
                MP3FrameInfo fi;
                os_mp3_lastinfo(mp3_hdec, &fi);
                frames = fi.nChans ? fi.outputSamps / fi.nChans : 0;
                ch = fi.nChans;
                break;
            }
            /* corrupt frame: skip a byte and resync */
            if (mp3_left > 0) { mp3_ptr++; mp3_left--; }
        }
        if (frames == 0) return 0;
        break;
    }
    case SRC_FLAC:
        frames = (int)drflac_read_pcm_frames_s16(flac_dec, PUMP_CHUNK, dec_buf);
        break;
    case SRC_MOD: {
        /* OS hxcmod renders interleaved stereo and loops forever; detect
         * song wrap via tablepos for the no-loop (interrupt) case */
        os_hxcmod_fill(mod_ctx, (msample *)dec_buf, PUMP_CHUNK);
        if (mod_noloop && mod_ctx->tablepos < mod_prev_tablepos) {
            frames = 0;                       /* completed one full pass */
        } else {
            mod_prev_tablepos = mod_ctx->tablepos;
            frames = PUMP_CHUNK;
        }
        ch = 2;
        break;
    }
    default:
        return 0;
    }
    int sl = mapping[vol_left], sr = mapping[vol_right];
    for (int i = 0; i < frames; i++) {
        int l, r;
        if (ch == 2) { l = dec_buf[i * 2]; r = dec_buf[i * 2 + 1]; }
        else         { l = r = dec_buf[i]; }
        pump_buf[i * 2]     = (int16_t)(l * sl / 2000);
        pump_buf[i * 2 + 1] = (int16_t)(r * sr / 2000);
    }
    return frames;
}

static void audio_pump_task(void *param) {
    (void)param;
    int chan_open = 0, chan_rate = 0;
    for (;;) {
        e_CurrentlyPlaying st = CurrentlyPlaying;
        int want_rate = (st == P_TONE || st == P_SOUND) ? 44100 :
                        (st == P_WAV || st == P_MP3 || st == P_FLAC || st == P_MOD)
                            ? src_rate : 0;
        if (want_rate == 0) {
            /* idle, paused or stopped: park with the channel closed */
            if (chan_open) {
                os_pcm_cleanup(); chan_open = 0;
            }
            pump_idle = 1;
            if (pump_kill) {                 /* app is exiting */
                typedef void (*fn)(void *);
                pump_created = 0;
                OST(1, fn)(NULL);            /* vTaskDelete(self) */
            }
            vTaskDelay(5);
            continue;
        }
        pump_idle = 0;
        if (!chan_open || chan_rate != want_rate) {
            os_pcm_init(want_rate, 2);       /* closes any previous channel */
            chan_open = 1;
            chan_rate = want_rate;
        }

        vTaskDelay(1);   /* hard pacing: never spin, even channel-less */
        if (st == P_TONE) {
            int n = synth_tone_chunk(pump_buf, PUMP_CHUNK);
            if (n) os_pcm_write(pump_buf, n);
            if (n < PUMP_CHUNK) {            /* duration expired */
                CurrentlyPlaying = P_NOTHING;
                WAVcomplete = true;          /* fires BASIC interrupt if set */
            }
        } else if (st == P_SOUND) {
            synth_sound_chunk(pump_buf, PUMP_CHUNK);
            os_pcm_write(pump_buf, PUMP_CHUNK);
        } else {                             /* file source */
            int n = decode_chunk();
            if (n > 0) {
                os_pcm_write(pump_buf, n);
            } else {                         /* end of track */
                CurrentlyPlaying = P_NOTHING;
                WAVcomplete = true;
                /* buffers are freed by the interpreter (CloseAudio /
                 * next PLAY) — the PSRAM allocator is not task-safe */
            }
        }
    }
}

static void ensure_pump(void) {
    if (pump_created) return;
    if (os_task_create(audio_pump_task, "bas_audio", 3072, 3) <= 0)
        error("Cannot start audio task");
    pump_created = 1;
}

/* wait until the pump has parked and closed the mixer channel */
static void wait_pump_idle(void) {
    if (!pump_created) return;
    for (int i = 0; i < 500 && !pump_idle; i++) vTaskDelay(1);
}

/* ════════════════════════════════════════════════════════════════════════
 * Resource management
 * ════════════════════════════════════════════════════════════════════════ */
static void free_file_source(void) {
    /* only call with the pump idle */
    switch (src_kind) {
    case SRC_WAV:  drwav_uninit(&wav_dec); break;
    case SRC_MP3:  if (mp3_hdec) { os_mp3_free(mp3_hdec); mp3_hdec = NULL; } break;
    case SRC_FLAC: if (flac_dec) { drflac_close(flac_dec); flac_dec = NULL; } break;
    case SRC_MOD:  if (mod_ctx) os_hxcmod_unload(mod_ctx); break;
    default: break;
    }
    src_kind = SRC_NONE;
    if (file_buf) { os_psram_free(file_buf); file_buf = NULL; }
}

void StopAudio(void) {
    if (CurrentlyPlaying != P_NOTHING) CurrentlyPlaying = P_STOP;
    SoundPlay = 0;
    wait_pump_idle();
    CurrentlyPlaying = P_NOTHING;
}

void CloseAudio(int all) {
    StopAudio();
    free_file_source();
    for (int i = 0; i < MAXSOUNDS; i++) {
        sound_mode_left[i]  = (unsigned short *)nulltable;
        sound_mode_right[i] = (unsigned short *)nulltable;
        sound_PhaseAC_left[i] = sound_PhaseAC_right[i] = 0;
    }
    if (all) {
        WAVInterrupt = NULL;
        WAVcomplete = false;
        if (noisetable) FreeMemorySafe((void **)&noisetable);
        usertable = NULL;
    }
}

/* App exit: stop playback, free everything, and take the pump task down
 * with us — it must not outlive the app's code and data. Called from
 * frankos_main.c after the interpreter returns. */
void frankos_audio_shutdown(void) {
    CloseAudio(1);
    if (mod_ctx) { os_psram_free(mod_ctx); mod_ctx = NULL; }
    if (pump_created) {
        pump_kill = 1;
        for (int i = 0; i < 200 && pump_created; i++) vTaskDelay(1);
    }
}

void initAudio(void) {
    PWM_FREQ = 44100;
    for (int i = 0; i < MAXSOUNDS; i++) {
        sound_mode_left[i]  = (unsigned short *)nulltable;
        sound_mode_right[i] = (unsigned short *)nulltable;
    }
}

/* PicoMite background-maintenance hooks — the pump task replaces them */
void CheckAudio(void)    { }
void checkWAVinput(void) { }
void audioInterrupt(void){ }

static void setnoise(void) {
    if (!noisetable) noisetable = GetMemory(4096 * sizeof(uint16_t));
    for (int i = 0; i < 4096; i++) noisetable[i] = rand() % 3800 + 100;
}

/* ════════════════════════════════════════════════════════════════════════
 * File loading (interpreter task only — FatFS is not reentrant)
 * ════════════════════════════════════════════════════════════════════════ */
/* ── OS-native file loading ──────────────────────────────────────────
 * The app-side FatFS types have a DIFFERENT memory layout from the OS
 * engine that frankos_ff.c routes to (ffconf.h mismatch: FS_TINY, LFN,
 * EXFAT, STR_VOLUME_ID all differ), so MMBasic's file layer cannot be
 * trusted here.  We call the OS FatFS syscalls directly with opaque
 * buffers, relying only on stable facts: FILINFO.fsize is the first
 * field (8 bytes, exFAT build), and the OS FIL fits in 512 bytes.
 * Paths are plain Frank OS SD paths; a leading A:/B: drive prefix is
 * stripped for compatibility with MMBasic habits. */
#define SYS_F_OPEN  46
#define SYS_F_CLOSE 47
#define SYS_F_READ  49
#define SYS_F_STAT  50

static uint8_t os_fil[512] __attribute__((aligned(8)));   /* opaque OS FIL */

static void norm_os_path(const char *in, char *out, int outlen) {
    if (in[0] && in[1] == ':') in += 2;      /* strip A:/B: prefix */
    int n = 0;
    if (in[0] != '/') out[n++] = '/';
    while (*in && n < outlen - 1) out[n++] = *in++;
    out[n] = 0;
}

static void load_file_to_psram(unsigned char *fname_arg, uint32_t *size_out) {
    typedef int (*fstat_t)(const char *, void *);
    typedef int (*fopen_t)(void *, const char *, uint8_t);
    typedef int (*fread_t)(void *, void *, unsigned int, unsigned int *);
    typedef int (*fclose_t)(void *);

    char *p = (char *)getFstring(fname_arg);
    static char path[256];
    norm_os_path(p, path, sizeof(path));

    static uint8_t info[1024] __attribute__((aligned(8)));  /* opaque FILINFO */
    memset(info, 0, 16);
    int fr = ((fstat_t)_ost[SYS_F_STAT])(path, info);
    uint64_t fsize64 = 0;
    memcpy(&fsize64, info, 8);                /* FILINFO.fsize at offset 0 */
    if (fr != 0 || fsize64 < 16) error("Cannot find file");
    if (fsize64 > (7u << 20)) error("File too large");
    uint32_t fsize = (uint32_t)fsize64;

    fr = ((fopen_t)_ost[SYS_F_OPEN])(os_fil, path, 0x01 /* FA_READ */);
    if (fr != 0) error("Cannot open file");
    file_buf = os_psram_alloc(fsize);
    if (!file_buf) {
        ((fclose_t)_ost[SYS_F_CLOSE])(os_fil);
        error("Out of memory");
    }
    uint32_t total = 0;
    while (total < fsize) {
        unsigned int rd = 0;
        unsigned int want = fsize - total > 32768 ? 32768 : fsize - total;
        fr = ((fread_t)_ost[SYS_F_READ])(os_fil, file_buf + total, want, &rd);
        if (fr != 0 || rd == 0) break;
        total += rd;
    }
    ((fclose_t)_ost[SYS_F_CLOSE])(os_fil);
    if (total != fsize) {
        os_psram_free(file_buf); file_buf = NULL;
        error("Cannot read file");
    }
    *size_out = fsize;
}

/* common preamble for the file-playing branches */
static void file_play_prologue(void) {
    if (CurrentlyPlaying == P_WAVOPEN) CloseAudio(1);
    if (CurrentlyPlaying != P_NOTHING)
        error("Sound output in use for $", PlayingStr[CurrentlyPlaying]);
    ensure_pump();
    wait_pump_idle();
    free_file_source();                  /* free any finished track */
    WAVInterrupt = NULL;
    WAVcomplete = false;
}

static void file_play_set_interrupt(unsigned char *arg) {
    if (!CurrentLinePtr) error("No program running");
    WAVInterrupt = (char *)GetIntAddress(arg);
    InterruptUsed = true;
}

/* ════════════════════════════════════════════════════════════════════════
 * PLAY command
 * ════════════════════════════════════════════════════════════════════════ */
void cmd_play(void) {
    unsigned char *tp;
    static uint8_t audio_inited = 0;
    if (!audio_inited) {          /* runtime table parking — see above */
        audio_inited = 1;
        initAudio();
    }

    if (checkstring(cmdline, (unsigned char *)"STOP")) {
        if (CurrentlyPlaying == P_NOTHING) return;
        CloseAudio(1);
        return;
    }
    if (checkstring(cmdline, (unsigned char *)"CLOSE")) {
        CloseAudio(1);
        return;
    }
    if (checkstring(cmdline, (unsigned char *)"PAUSE")) {
        if (CurrentlyPlaying < P_STOP) return;   /* already paused */
        if      (CurrentlyPlaying == P_TONE)  CurrentlyPlaying = P_PAUSE_TONE;
        else if (CurrentlyPlaying == P_SOUND) CurrentlyPlaying = P_PAUSE_SOUND;
        else if (CurrentlyPlaying == P_WAV)   CurrentlyPlaying = P_PAUSE_WAV;
        else if (CurrentlyPlaying == P_FLAC)  CurrentlyPlaying = P_PAUSE_FLAC;
        else if (CurrentlyPlaying == P_MP3)   CurrentlyPlaying = P_PAUSE_MP3;
        else if (CurrentlyPlaying == P_MOD)   CurrentlyPlaying = P_PAUSE_MOD;
        else error("Nothing playing");
        return;
    }
    if (checkstring(cmdline, (unsigned char *)"RESUME")) {
        if      (CurrentlyPlaying == P_PAUSE_TONE)  CurrentlyPlaying = P_TONE;
        else if (CurrentlyPlaying == P_PAUSE_SOUND) CurrentlyPlaying = P_SOUND;
        else if (CurrentlyPlaying == P_PAUSE_WAV)   CurrentlyPlaying = P_WAV;
        else if (CurrentlyPlaying == P_PAUSE_FLAC)  CurrentlyPlaying = P_FLAC;
        else if (CurrentlyPlaying == P_PAUSE_MP3)   CurrentlyPlaying = P_MP3;
        else if (CurrentlyPlaying == P_PAUSE_MOD)   CurrentlyPlaying = P_MOD;
        else error("Nothing to resume");
        return;
    }
    if ((tp = checkstring(cmdline, (unsigned char *)"VOLUME"))) {
        getcsargs(&tp, 3);
        if (argc < 1) error("Syntax");
        if (*argv[0]) vol_left = getint(argv[0], 0, 100);
        if (argc == 3) vol_right = getint(argv[2], 0, 100);
        else vol_right = vol_left;
        if (CurrentlyPlaying == P_TONE && vol_left != vol_right && mono)
            mono = 0;
        return;
    }
    if ((tp = checkstring(cmdline, (unsigned char *)"LOAD SOUND"))) {
        if (usertable != NULL) error("Already loaded");
        int64_t *aint;
        skipspace(tp);
        int size = parseintegerarray(tp, &aint, 1, 1, NULL, false, NULL);
        if (size != 1024) error("Array size must be 1024");
        usertable = (unsigned short *)aint;
        return;
    }
    if ((tp = checkstring(cmdline, (unsigned char *)"TONE"))) {
        float f_left, f_right, duration;
        uint64_t PlayDuration = 0xffffffffffffffffull;   /* forever */
        getcsargs(&tp, 7);
        if (!(argc == 3 || argc == 5 || argc == 7)) error("Syntax");
        mono = 0;
        if (!(CurrentlyPlaying == P_NOTHING || CurrentlyPlaying == P_TONE ||
              CurrentlyPlaying == P_PAUSE_TONE || CurrentlyPlaying == P_STOP ||
              CurrentlyPlaying == P_WAVOPEN))
            error("Sound output in use for $", PlayingStr[CurrentlyPlaying]);
        f_left  = getnumber(argv[0]);
        f_right = getnumber(argv[2]);
        if (f_left == f_right && vol_left == vol_right) mono = 1;
        if (f_left < 0.0 || f_left > 22050.0)  error("Valid is 0Hz to 20KHz");
        if (f_right < 0.0 || f_right > 22050.0) error("Valid is 0Hz to 20KHz");
        if (argc > 4)
            duration = (float)getint(argv[4], 0, INT_MAX) / 1000.0f;
        else
            duration = 1;
        if (argc == 7) {
            if (!CurrentLinePtr) error("No program running");
            WAVInterrupt = (char *)GetIntAddress(argv[6]);
            WAVcomplete = false;
            InterruptUsed = true;
        }
        if (duration == 0) return;
        if (argc > 4)
            PlayDuration = (uint64_t)(duration * (float)PWM_FREQ);
        ensure_pump();
        PhaseM_left  = f_left  / (float)PWM_FREQ * 4096.0f;
        PhaseM_right = f_right / (float)PWM_FREQ * 4096.0f;
        if (!(CurrentlyPlaying == P_PAUSE_TONE || CurrentlyPlaying == P_TONE)) {
            PhaseAC_left = 0.0f;
            PhaseAC_right = 0.0f;
        }
        SoundPlay = PlayDuration;
        CurrentlyPlaying = P_TONE;
        return;
    }
    if ((tp = checkstring(cmdline, (unsigned char *)"SOUND"))) {
        /* PLAY SOUND channel, position, type [, frequency [, volume]] */
        float f_in, PhaseM;
        int channel, left = 0, right = 0;
        int local_v_left = 0, local_v_right = 0;
        char *p;
        const unsigned short *tbl = NULL;
        getcsargs(&tp, 9);
        if (!(argc == 9 || argc == 7 || argc == 5)) error("Syntax");
        channel = getint(argv[0], 1, MAXSOUNDS) - 1;
        /* position: bare letter first (PLAY SOUND 1,B,S,...), then
         * string expression fallback ("B" or a string variable) */
        if      (checkstring(argv[2], (unsigned char *)"L")) left = 1;
        else if (checkstring(argv[2], (unsigned char *)"R")) right = 1;
        else if (checkstring(argv[2], (unsigned char *)"B")) left = right = 1;
        else if (checkstring(argv[2], (unsigned char *)"M")) left = right = 1;
        else {
            p = (char *)getCstring(argv[2]);
            if      (strcasecmp(p, "L") == 0) left = 1;
            else if (strcasecmp(p, "R") == 0) right = 1;
            else if (strcasecmp(p, "B") == 0 || strcasecmp(p, "M") == 0) left = right = 1;
            else error("Position must be L, R, or B");
        }
        if (!(CurrentlyPlaying == P_NOTHING || CurrentlyPlaying == P_SOUND ||
              CurrentlyPlaying == P_PAUSE_SOUND || CurrentlyPlaying == P_STOP ||
              CurrentlyPlaying == P_WAVOPEN))
            error("Sound output in use for $", PlayingStr[CurrentlyPlaying]);
        /* type: bare letter first, then string expression fallback */
        if      (checkstring(argv[4], (unsigned char *)"O")) tbl = nulltable;
        else if (checkstring(argv[4], (unsigned char *)"Q")) tbl = squaretable;
        else if (checkstring(argv[4], (unsigned char *)"T")) tbl = triangletable;
        else if (checkstring(argv[4], (unsigned char *)"W")) tbl = sawtable;
        else if (checkstring(argv[4], (unsigned char *)"S")) tbl = SineTable;
        else if (checkstring(argv[4], (unsigned char *)"P")) { setnoise(); tbl = noisetable; }
        else if (checkstring(argv[4], (unsigned char *)"N")) tbl = whitenoise;
        else if (checkstring(argv[4], (unsigned char *)"U")) {
            if (usertable == NULL) error("Not loaded");
            tbl = usertable;
        }
        else {
            p = (char *)getCstring(argv[4]);
            if      (strcasecmp(p, "O") == 0) tbl = nulltable;
            else if (strcasecmp(p, "Q") == 0) tbl = squaretable;
            else if (strcasecmp(p, "T") == 0) tbl = triangletable;
            else if (strcasecmp(p, "W") == 0) tbl = sawtable;
            else if (strcasecmp(p, "S") == 0) tbl = SineTable;
            else if (strcasecmp(p, "P") == 0) { setnoise(); tbl = noisetable; }
            else if (strcasecmp(p, "N") == 0) tbl = whitenoise;
            else if (strcasecmp(p, "U") == 0) {
                if (usertable == NULL) error("Not loaded");
                tbl = usertable;
            }
            else error("Invalid type");
        }
        f_in = 10.0f;
        if (argc >= 7) f_in = getnumber(argv[6]);
        if (f_in < 1.0 || f_in > 20000.0) error("Valid is 1Hz to 20KHz");
        int vparm = 25;
        if (argc == 9) vparm = getint(argv[8], 0, 100 / MAXSOUNDS);
        int vmapped = vparm * 41 / (100 / MAXSOUNDS);
        ensure_pump();
        if (left) {
            PhaseM = (tbl == whitenoise) ? f_in
                                         : f_in / (float)PWM_FREQ * 4096.0f;
            if (sound_mode_left[channel] != tbl) sound_PhaseAC_left[channel] = 0.0f;
            sound_PhaseM_left[channel] = PhaseM;
            sound_v_left[channel] = vmapped;
            local_v_left = vmapped;
            sound_mode_left[channel] = (unsigned short *)tbl;
        }
        if (right) {
            PhaseM = (tbl == whitenoise) ? f_in
                                         : f_in / (float)PWM_FREQ * 4096.0f;
            if (sound_mode_right[channel] != tbl) sound_PhaseAC_right[channel] = 0.0f;
            sound_PhaseM_right[channel] = PhaseM;
            sound_v_right[channel] = vmapped;
            local_v_right = vmapped;
            sound_mode_right[channel] = (unsigned short *)tbl;
        }
        (void)local_v_left; (void)local_v_right;
        CurrentlyPlaying = P_SOUND;
        return;
    }
    if ((tp = checkstring(cmdline, (unsigned char *)"WAV"))) {
        getcsargs(&tp, 3);
        if (!(argc == 1 || argc == 3)) error("Syntax");
        file_play_prologue();
        uint32_t size;
        load_file_to_psram(argv[0], &size);
        if (!drwav_init_memory(&wav_dec, file_buf, size, NULL)) {
            os_psram_free(file_buf); file_buf = NULL;
            error("Invalid WAV file");
        }
        if (wav_dec.channels < 1 || wav_dec.channels > 2) {
            drwav_uninit(&wav_dec);
            os_psram_free(file_buf); file_buf = NULL;
            error("Only mono or stereo supported");
        }
        if (argc == 3) file_play_set_interrupt(argv[2]);
        src_channels = wav_dec.channels;
        src_rate = wav_dec.sampleRate;
        src_kind = SRC_WAV;
        CurrentlyPlaying = P_WAV;
        return;
    }
    if ((tp = checkstring(cmdline, (unsigned char *)"MP3"))) {
        getcsargs(&tp, 3);
        if (!(argc == 1 || argc == 3)) error("Syntax");
        file_play_prologue();
        uint32_t size;
        load_file_to_psram(argv[0], &size);
        mp3_hdec = os_mp3_init();
        if (!mp3_hdec) {
            os_psram_free(file_buf); file_buf = NULL;
            error("Out of memory");
        }
        int off = os_mp3_sync(file_buf, (int)size);
        MP3FrameInfo fi;
        if (off < 0 ||
            os_mp3_nextinfo(mp3_hdec, &fi, file_buf + off) != 0 ||
            fi.nChans < 1 || fi.nChans > 2) {
            os_mp3_free(mp3_hdec); mp3_hdec = NULL;
            os_psram_free(file_buf); file_buf = NULL;
            error("Invalid MP3 file");
        }
        mp3_ptr  = file_buf + off;
        mp3_left = (int)size - off;
        if (argc == 3) file_play_set_interrupt(argv[2]);
        src_channels = fi.nChans;
        src_rate = fi.samprate;
        src_kind = SRC_MP3;
        CurrentlyPlaying = P_MP3;
        return;
    }
    if ((tp = checkstring(cmdline, (unsigned char *)"FLAC"))) {
        getcsargs(&tp, 3);
        if (!(argc == 1 || argc == 3)) error("Syntax");
        file_play_prologue();
        uint32_t size;
        load_file_to_psram(argv[0], &size);
        flac_dec = drflac_open_memory(file_buf, size, NULL);
        if (!flac_dec) {
            os_psram_free(file_buf); file_buf = NULL;
            error("Invalid FLAC file");
        }
        if (flac_dec->channels < 1 || flac_dec->channels > 2) {
            drflac_close(flac_dec); flac_dec = NULL;
            os_psram_free(file_buf); file_buf = NULL;
            error("Only mono or stereo supported");
        }
        if (argc == 3) file_play_set_interrupt(argv[2]);
        src_channels = flac_dec->channels;
        src_rate = flac_dec->sampleRate;
        src_kind = SRC_FLAC;
        CurrentlyPlaying = P_FLAC;
        return;
    }
    if ((tp = checkstring(cmdline, (unsigned char *)"MODFILE"))) {
        getcsargs(&tp, 3);
        if (!(argc == 1 || argc == 3)) error("Syntax");
        file_play_prologue();
        if (!mod_ctx) {
            mod_ctx = os_psram_alloc(sizeof(modcontext));
            if (!mod_ctx) error("Out of memory");
            memset(mod_ctx, 0, sizeof(modcontext));
        }
        uint32_t size;
        load_file_to_psram(argv[0], &size);
        os_hxcmod_init(mod_ctx);
        os_hxcmod_setcfg(mod_ctx, 44100, 1, 1);
        if (!os_hxcmod_load(mod_ctx, file_buf, (int)size)) {
            os_psram_free(file_buf); file_buf = NULL;
            error("Invalid MOD file");
        }
        mod_prev_tablepos = 0;
        mod_noloop = 0;
        if (argc == 3) {
            file_play_set_interrupt(argv[2]);
            mod_noloop = 1;                  /* PicoMite: interrupt implies no loop */
        }
        src_channels = 2;
        src_rate = 44100;
        src_kind = SRC_MOD;
        CurrentlyPlaying = P_MOD;
        return;
    }

    /* PicoMite subcommands with no Frank OS equivalent */
    {
        static const char *unsupported[] = {
            "ARRAY", "SAMPLE", "STREAM", "NOTE", "MIDI", "MIDIFILE",
            "NEXT", "PREVIOUS", "MODSAMPLE", "LIST", "HALT", NULL };
        for (int i = 0; unsupported[i]; i++)
            if (checkstring(cmdline, (unsigned char *)unsupported[i])) {
                char msg[64];
                snprintf(msg, sizeof(msg), "PLAY %s: not supported on Frank OS",
                         unsupported[i]);
                error(msg);
            }
    }
    error("Syntax");
}
