/*
 * ATSAMD21E18A → MAX98357A – 4-voice + ADSR presets
 *
 * I2S:     PA08=DIN  PA10=BCLK  PA11=LRCLK
 * LED:     PA17
 * Encoder: PA14/PA15 = A/B   PA22 = push (mode toggle)
 * TM1638:  PA16=STB  PA18=CLK  PA19=DIO
 *
 * Display left 4: VOL / ORG / PLK / PAD / BRS / PNO / SAX / VLN / DRM
 * Display right 4: volume 0–64 or preset index 1–8
 *
 * Encoder push: toggle volume ↔ preset select
 * Encoder turn: volume (fine) or preset (1 step / detent)
 *
 * S1…S8 → C5 D5 E5 F5 G5 A5 B5 C6  (melody presets)
 *         Kick Snare HH Clap Tom1 Tom2 Rim Crash  (drum mode)
 */

#include <Arduino.h>
#include "sam.h"
#include <math.h>
#include "audio_engine.h"

// I2S clocking: 48 MHz GCLK0 / (MCKDIV + 1) = 1.5 MHz SCK.
// Two 32-bit slots per frame => 64 BCLKs per audio sample.
// 23.4375 kHz leaves full-DSP headroom on the SAMD21 while keeping drum
// partials below Nyquist and within MAX98357A's supported audio-rate range.
static constexpr uint32_t I2S_GCLK_HZ       = 48000000;
static constexpr uint32_t I2S_MCK_DIV       = 31;
static constexpr uint32_t I2S_SLOTS         = 2;
static constexpr uint32_t I2S_BITS_PER_SLOT = 32;
static constexpr uint32_t SAMPLE_RATE_HZ =
    I2S_GCLK_HZ / (I2S_MCK_DIV + 1) / (I2S_SLOTS * I2S_BITS_PER_SLOT);
static constexpr uint32_t SINE_LEN       = 1024;
static constexpr uint32_t INSTRUMENT_TABLE_LEN = 512;
static constexpr int      VOLUME_MAX     = 64;
// Maximum simultaneous notes. The mixer renders every active voice into
// one PCM sample before the I2S ISR sends it to the MAX98357A.
static constexpr int      NUM_VOICES     = 4;
static constexpr int      NUM_PRESETS    = 8;
static constexpr int      PRESET_DRUM    = 7;  // index of drum mode
static constexpr int      PRESET_SAX     = 5;
static constexpr int      PRESET_VLN     = 6;

// Optional transport diagnostic; leave disabled for normal instrument DSP.
static constexpr bool AUDIO_DIAGNOSTIC_TONE = false;
// Enable only to isolate polyphony and audio transport from instrument DSP.
static constexpr bool AUDIO_POLYPHONY_TEST = false;
static constexpr uint32_t DIAG_PHASE_INC =
    (uint32_t)(((uint64_t)523 << 32) / SAMPLE_RATE_HZ);
static uint32_t diag_phase = 0;

static constexpr int32_t SINE_PEAK = 280000000;
static constexpr int32_t ENV_ONE   = 65536;
static constexpr uint32_t ENV_RATE_SHIFT = 8;
static constexpr uint32_t ENV_RATE_SCALE = 1u << ENV_RATE_SHIFT;

static int32_t sine_table[SINE_LEN];

static const uint16_t button_hz[8] = {
    523, 587, 659, 698, 784, 880, 988, 1047
};

/* Drum pitch map (Hz) – approximate classic kit tones */
/*
 * How each drum is synthesized:
 *   S1 Kick  – 190→55 Hz sine pitch sweep, gain 1.0x, no noise
 *   S2 Snare – sine 200 Hz, gain 1.0x, pure tone
 *   S3 HH    – band-limited noise with quiet inharmonic metallic partials
 *   S4 Clap  – sine 280 Hz, gain 1.0x
 *   S5 TomL  – sine 160 Hz, gain 1.0x
 *   S6 TomM  – sine 220 Hz, gain 1.0x
 *   S7 Rim   – sine 500 Hz, gain 1.0x
 *   S8 Crash – inharmonic 4.5 kHz metallic partials, noise-free
 * Drum envelopes are preset-specific; DRM is approximately 150 ms decay with no sustain.
 */
static constexpr uint32_t HH_METAL_HZ1 = 2800;
static constexpr uint32_t HH_METAL_HZ2 = 4100;
static constexpr uint32_t HH_METAL_HZ3 = 5300;
static constexpr uint32_t HH_METAL_HZ4 = 7600;
static constexpr uint32_t CRASH_METAL_HZ = 6200;
static constexpr uint32_t HH_METAL_INC1 =
    (uint32_t)(((uint64_t)HH_METAL_HZ1 << 32) / SAMPLE_RATE_HZ);
static constexpr uint32_t HH_METAL_INC2 =
    (uint32_t)(((uint64_t)HH_METAL_HZ2 << 32) / SAMPLE_RATE_HZ);
static constexpr uint32_t HH_METAL_INC3 =
    (uint32_t)(((uint64_t)HH_METAL_HZ3 << 32) / SAMPLE_RATE_HZ);
static constexpr uint32_t HH_METAL_INC4 =
    (uint32_t)(((uint64_t)HH_METAL_HZ4 << 32) / SAMPLE_RATE_HZ);
static constexpr int32_t HH_DECAY =
    (int32_t)(((ENV_ONE * 1000ULL) / ((uint64_t)115 * SAMPLE_RATE_HZ)) == 0
        ? 1 : ((ENV_ONE * 1000ULL) / ((uint64_t)115 * SAMPLE_RATE_HZ)));
static constexpr int32_t HH_NOISE_DECAY =
    (int32_t)(((ENV_ONE * 1000ULL) / ((uint64_t)220 * SAMPLE_RATE_HZ)) == 0
        ? 1 : ((ENV_ONE * 1000ULL) / ((uint64_t)220 * SAMPLE_RATE_HZ)));
static constexpr uint32_t CRASH_METAL_INC =
    (uint32_t)(((uint64_t)CRASH_METAL_HZ << 32) / SAMPLE_RATE_HZ);

static constexpr uint32_t KICK_START_HZ = 190;
static constexpr uint32_t KICK_END_HZ   = 55;
static constexpr uint32_t KICK_SWEEP_MS = 80;
static constexpr uint32_t KICK_SWEEP_SAMPLES =
    (SAMPLE_RATE_HZ * KICK_SWEEP_MS) / 1000;
static constexpr uint32_t KICK_START_INC =
    (uint32_t)(((uint64_t)KICK_START_HZ << 32) / SAMPLE_RATE_HZ);
static constexpr uint32_t KICK_END_INC =
    (uint32_t)(((uint64_t)KICK_END_HZ << 32) / SAMPLE_RATE_HZ);
static constexpr uint32_t KICK_INC_STEP =
    (KICK_START_INC - KICK_END_INC) / KICK_SWEEP_SAMPLES;

static const uint16_t drum_hz[8] = {
    150,  // Kick
    200,  // Snare
    440,  // Hi-hat diagnostic sine
    280,  // Clap
    160,  // Tom low
    220,  // Tom mid
    500,  // Rim
    4500  // Crash carrier
};

/* Relative gain 0–256 (256 = unity). */
static const uint16_t drum_gain[8] = {
    256,  // Kick
    256,  // Snare
    240,  // Hi-hat
    256,  // Clap
    256,  // Tom low
    256,  // Tom mid
    256,  // Rim
    256,  // Crash
};

enum EnvStage : uint8_t { ENV_IDLE = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

struct Adsr {
    int32_t attack;
    int32_t decay;
    int32_t sustain;
    int32_t release;
};

// Envelope rates are Q8 fixed-point increments per sample, derived from time
// and the current sample rate.
//
// attack_ms / decay_ms / release_ms are approximate times to traverse the
// corresponding envelope range. A zero value means an immediate transition.
static constexpr int32_t env_rate_from_ms(uint32_t ms, int32_t range = ENV_ONE)
{
    return (ms == 0)
                ? ENV_ONE * ENV_RATE_SCALE
                    : (int32_t)((((uint64_t)range * 1000ULL * ENV_RATE_SCALE) /
                             ((uint64_t)ms * SAMPLE_RATE_HZ)) == 0
                          ? 1
                                                    : (((uint64_t)range * 1000ULL * ENV_RATE_SCALE) /
                              ((uint64_t)ms * SAMPLE_RATE_HZ)));
}

/*
 * Instrument envelopes, expressed in milliseconds so timing is independent
 * of the I2S sample rate:
 *
 * ORG – fast attack, full sustain, ~2 s release
 * PLK – instant attack, ~180 ms decay, short release
 * PAD – ~1 s attack, full sustain, ~2 s release
 * BRS – ~150 ms attack, ~500 ms decay toward 50%, ~1 s release
 * PNO – fast attack, ~2 s decay to silence, ~1 s release
 * SAX – ~50 ms attack, ~250 ms decay toward 85%, ~450 ms release
 * VLN – ~100 ms attack, ~500 ms decay toward 80%, ~1.2 s release
 * DRM – instant attack, ~150 ms decay, no audible release
 */
static const Adsr PRESETS[NUM_PRESETS] = {
    { env_rate_from_ms(2),    env_rate_from_ms(0),   ENV_ONE,             env_rate_from_ms(2000) }, // ORG
    { env_rate_from_ms(1),    env_rate_from_ms(180),  0,                  env_rate_from_ms(250) }, // PLK
    { env_rate_from_ms(1000), env_rate_from_ms(0),   ENV_ONE,             env_rate_from_ms(2000) }, // PAD
    { env_rate_from_ms(150),  env_rate_from_ms(500), ENV_ONE / 2,         env_rate_from_ms(1000) }, // BRS
    { env_rate_from_ms(5),    env_rate_from_ms(2000), 0,                  env_rate_from_ms(250) }, // PNO
    { env_rate_from_ms(50),   env_rate_from_ms(250, ENV_ONE * 15 / 100), ENV_ONE * 85 / 100, env_rate_from_ms(450, ENV_ONE * 85 / 100) }, // SAX
        { env_rate_from_ms(150),  env_rate_from_ms(700, ENV_ONE / 4),  ENV_ONE * 3 / 4,  env_rate_from_ms(1400, ENV_ONE * 3 / 4) }, // VLN
    { env_rate_from_ms(1),    env_rate_from_ms(150), 0,                  env_rate_from_ms(300) }, // DRM
};

static volatile int     volume     = VOLUME_MAX / 4;
static volatile uint8_t preset_idx = 0;

struct Voice {
    volatile uint32_t phase;
    volatile uint32_t phase_inc;
    volatile int32_t  env_level;
    volatile uint8_t  env_stage;
    volatile uint8_t  env_rate_fraction;
    volatile int8_t   note;
    volatile uint32_t age;
    volatile uint8_t  is_noise;       // 1 = noise source (hi-hat / crash)
    volatile uint16_t gain;           // 256 = unity
    volatile uint32_t click_level;     // Hammond key-click transient, 0..ENV_ONE
    volatile uint32_t perc_phase;      // secondary percussion/metal oscillator phase
    volatile uint32_t drum_phase2;     // independent drum partial phase
    volatile uint32_t hat_phase3;       // 5.3 kHz hi-hat partial
    volatile uint32_t hat_phase4;       // 7.6 kHz hi-hat partial
    volatile int32_t hat_noise_hp;
    volatile int32_t hat_noise_bp;
    volatile uint32_t hat_noise_level; // hi-hat noise tail, 0..ENV_ONE
};

static inline int32_t env_increment(volatile uint8_t &fraction, int32_t rate_q8)
{
    uint32_t accumulated = (uint32_t)fraction + (uint32_t)rate_q8;
    fraction = (uint8_t)(accumulated & (ENV_RATE_SCALE - 1u));
    return (int32_t)(accumulated >> ENV_RATE_SHIFT);
}

static Voice voices[NUM_VOICES];
static volatile uint32_t voice_age_counter = 0;
static volatile uint32_t noise_lfsr = 0xACE1u;  // 16-bit LFSR seed
static volatile int32_t  noise_lpf  = 0;         // one-pole low-pass state

// Audio producer/consumer buffer. The main loop renders audio; the I2S ISR
// only moves already-rendered samples into the I2S DATA register.
// Ring-buffer indexing uses a bit mask, so the frame count MUST be a power of two.
// 1024 fits the SAMD21E18A SRAM and avoids the invalid wrap behavior of 1408.
static constexpr uint16_t AUDIO_BUFFER_FRAMES = 1024;
static constexpr uint16_t AUDIO_BUFFER_MASK   = AUDIO_BUFFER_FRAMES - 1;
static int32_t audio_buffer[AUDIO_BUFFER_FRAMES];
static volatile uint16_t audio_read_index  = 0;
static volatile uint16_t audio_write_index = 0;
static volatile uint32_t audio_underruns   = 0;
static volatile uint8_t i2s_slot = 0;
static volatile int32_t i2s_hold = 0;

/* 16-bit LFSR white noise, amplitude ~±SINE_PEAK/4, then mild LPF */
static inline int32_t next_noise(void)
{
    uint32_t l = noise_lfsr;
    uint32_t bit = ((l >> 0) ^ (l >> 2) ^ (l >> 3) ^ (l >> 5)) & 1u;
    l = (l >> 1) | (bit << 15);
    noise_lfsr = l;
    /* Keep the raw noise modest, then high-pass it for percussion. */
    int32_t n = (int32_t)(((int32_t)l - 32768) * (SINE_PEAK / 131072));
    noise_lpf += (n - noise_lpf) >> 3;
    return n - noise_lpf;
}

static void sine_table_init(void)
{
    for (uint32_t i = 0; i < SINE_LEN; i++) {
        float a = (2.0f * 3.14159265f * (float)i) / (float)SINE_LEN;
        sine_table[i] = (int32_t)(sinf(a) * (float)SINE_PEAK);
    }
}

static inline int32_t sine_lookup_fast(uint32_t ph)
{
    return sine_table[(ph >> 22) & (SINE_LEN - 1)];
}

static int32_t sax_table[INSTRUMENT_TABLE_LEN];
static int32_t vln_table[INSTRUMENT_TABLE_LEN];
static int32_t organ_harmonic_table[INSTRUMENT_TABLE_LEN];
static int32_t organ_sub_table[INSTRUMENT_TABLE_LEN];      // 16' (0.5x), weighted
static int32_t organ_5th3_table[INSTRUMENT_TABLE_LEN];     // 5 1/3' (1.5x), weighted

// Hammond-style modulation. A small ~6 Hz pitch wobble is much more
// characteristic of a tonewheel organ than a perfectly static oscillator.
static constexpr uint32_t ORGAN_VIBRATO_HZ = 6;
static constexpr uint32_t ORGAN_LFO_INC =
    (uint32_t)(((uint64_t)ORGAN_VIBRATO_HZ << 32) / SAMPLE_RATE_HZ);
static uint32_t organ_lfo_phase = 0;
static constexpr uint32_t ORGAN_TREMOLO_HZ = 7;
static constexpr uint32_t ORGAN_TREMOLO_INC =
    (uint32_t)(((uint64_t)ORGAN_TREMOLO_HZ << 32) / SAMPLE_RATE_HZ);
static uint32_t organ_tremolo_phase = 0;
static constexpr uint32_t SAX_VIBRATO_HZ = 5;
static constexpr uint32_t SAX_VIBRATO_INC =
    (uint32_t)(((uint64_t)SAX_VIBRATO_HZ << 32) / SAMPLE_RATE_HZ);
static uint32_t sax_vibrato_phase = 0;
static constexpr uint32_t VLN_VIBRATO_HZ = 6;
static constexpr uint32_t VLN_VIBRATO_INC =
    (uint32_t)(((uint64_t)VLN_VIBRATO_HZ << 32) / SAMPLE_RATE_HZ);
static uint32_t vln_vibrato_phase = 0;

static void instrument_tables_init()
{
    for (uint32_t i = 0; i < INSTRUMENT_TABLE_LEN; i++) {
        const double a = 2.0 * M_PI * (double)i / (double)INSTRUMENT_TABLE_LEN;

        // SAX and VLN are precomputed. ORG keeps the Hammond drawbar
        // spectrum, but the integer harmonics are also precomputed so the
        // real-time audio path does not perform 64-bit multiplies/divides.
        double sax = sin(a)
              + 0.18 * sin(2.0 * a)
              + 0.65 * sin(3.0 * a)
              + 0.12 * sin(4.0 * a)
              + 0.42 * sin(5.0 * a)
              + 0.10 * sin(6.0 * a)
              + 0.25 * sin(7.0 * a)
              + 0.08 * sin(8.0 * a)
              + 0.14 * sin(9.0 * a);

        double vln = sin(a)
                  + 0.85 * sin(2.0 * a)
                  + 0.65 * sin(3.0 * a)
                  + 0.50 * sin(4.0 * a)
                  + 0.40 * sin(5.0 * a)
                  + 0.30 * sin(6.0 * a)
                  + 0.22 * sin(7.0 * a)
                  + 0.15 * sin(8.0 * a);

        // Hammond-style drawbars relative to the played note:
        // 8' = 1x, 4' = 2x, 2 2/3' = 3x, 2' = 4x,
        // 1 3/5' = 5x, 1 1/3' = 6x.
        // The 16' (0.5x) and 5 1/3' (1.5x) partials remain separate
        // because they are not periodic over one fundamental cycle.
        double organ = sin(a) * 180.0
                     + sin(2.0 * a) * 110.0
                     + sin(3.0 * a) * 82.0
                     + sin(4.0 * a) * 62.0
                     + sin(5.0 * a) * 38.0
                     + sin(6.0 * a) * 26.0;

        sax_table[i] = (int32_t)(sax * (double)SINE_PEAK / 2.94);
        vln_table[i] = (int32_t)(vln * (double)SINE_PEAK / 4.07);
        organ_harmonic_table[i] = (int32_t)(organ * (double)SINE_PEAK / 744.0);

        // These are weighted one-cycle sine tables. The phase conversion is
        // done at lookup time so the fractional drawbars retain their proper
        // relationship to the fundamental:
        //   16'     = 0.5 x fundamental phase
        //   5 1/3'  = 1.5 x fundamental phase
        // The previous implementation indexed sin(0.5*a) and sin(1.5*a)
        // with the fundamental phase directly, which introduced a waveform
        // discontinuity at the table boundary.
        organ_sub_table[i] =
            (int32_t)(sin(a) * (96.0 / 744.0) * (double)SINE_PEAK);
        organ_5th3_table[i] =
            (int32_t)(sin(a) * (150.0 / 744.0) * (double)SINE_PEAK);
    }
}

static inline int32_t instrument_lookup(const int32_t *table, uint32_t ph)
{
    return table[(ph >> 23) & (INSTRUMENT_TABLE_LEN - 1)];
}

static inline int32_t organ_lookup(uint32_t ph)
{
    // All Hammond drawbars are precomputed. The two fractional drawbars
    // use transformed phase indices so their waveforms are continuous.
    const uint32_t fundamental_idx = (ph >> 23) & (INSTRUMENT_TABLE_LEN - 1);
    const uint32_t sub_idx =
        ((ph >> 1) >> 23) & (INSTRUMENT_TABLE_LEN - 1);
    const uint32_t fifth3_idx =
        ((ph + (ph >> 1)) >> 23) & (INSTRUMENT_TABLE_LEN - 1);

    return organ_harmonic_table[fundamental_idx]
         + organ_sub_table[sub_idx]
         + organ_5th3_table[fifth3_idx];
}


static uint32_t hz_to_inc(uint16_t hz)
{
    return (uint32_t)(((uint64_t)hz << 32) / SAMPLE_RATE_HZ);
}

static void voices_init(void)
{
    for (int i = 0; i < NUM_VOICES; i++) {
        voices[i].phase     = 0;
        voices[i].phase_inc = 0;
        voices[i].env_level = 0;
        voices[i].env_stage = ENV_IDLE;
        voices[i].env_rate_fraction = 0;
        voices[i].note      = -1;
        voices[i].age       = 0;
        voices[i].is_noise  = 0;
        voices[i].gain        = 256;
        voices[i].click_level = 0;
        voices[i].perc_phase  = 0;
        voices[i].drum_phase2 = 0;
        voices[i].hat_noise_level = 0;
        voices[i].hat_phase3 = 0;
        voices[i].hat_phase4 = 0;
        voices[i].hat_noise_hp = 0;
        voices[i].hat_noise_bp = 0;
    }
}

static int find_voice_for_note(int note)
{
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].note == note)
            return i;
    }
    return -1;
}

static int alloc_voice(void)
{
    /* Prefer fully idle */
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].note < 0 && voices[i].env_stage == ENV_IDLE)
            return i;
    }
    /* Prefer free note slot still decaying (note already cleared) */
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].note < 0)
            return i;
    }
    /* All voices tied to live notes — steal quietest releasing, else oldest */
    int best = -1;
    int32_t best_el = 0x7fffffff;
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].env_stage == ENV_RELEASE && voices[i].env_level < best_el) {
            best_el = voices[i].env_level;
            best = i;
        }
    }
    if (best >= 0)
        return best;

    int oldest = 0;
    uint32_t best_age = voices[0].age;
    for (int i = 1; i < NUM_VOICES; i++) {
        if (voices[i].age < best_age) {
            best_age = voices[i].age;
            oldest = i;
        }
    }
    return oldest;
}

static void note_on(int note)
{
    if (note < 0 || note > 7) return;

    int v = find_voice_for_note(note);
    if (v >= 0) {
        const uint8_t stage = voices[v].env_stage;
        if (stage == ENV_ATTACK || stage == ENV_DECAY || stage == ENV_SUSTAIN)
            return;
    } else {
        v = alloc_voice();
    }

    voices[v].note      = (int8_t)note;
    voices[v].phase     = 0;
    voices[v].env_level = 0;
    voices[v].env_stage = ENV_ATTACK;
    voices[v].env_rate_fraction = 0;
    voices[v].age        = ++voice_age_counter;
    voices[v].click_level = (preset_idx == 0) ? ENV_ONE : 0;
    voices[v].perc_phase  = 0;
    voices[v].drum_phase2 = 0;

    if (preset_idx == PRESET_DRUM) {
        /* Hi-hat and crash have dedicated metallic synthesis paths. */
        if (note == 2) {
            // S3 combines band-limited noise with inharmonic metal partials.
            voices[v].is_noise    = 0;
            voices[v].phase_inc   = 0;
            voices[v].perc_phase  = HH_METAL_INC1;
            voices[v].drum_phase2 = HH_METAL_INC2;
            voices[v].hat_phase3 = 0;
            voices[v].hat_phase4 = 0;
            voices[v].hat_noise_hp = 0;
            voices[v].hat_noise_bp = 0;
            voices[v].hat_noise_level = ENV_ONE;
            voices[v].gain        = 256;
        } else {
            voices[v].is_noise  = (note == 7) ? 1 : 0;
            voices[v].phase_inc = (note == 0) ? KICK_START_INC : hz_to_inc(drum_hz[note]);
            if (note == 7) {
                voices[v].perc_phase = CRASH_METAL_INC;
                voices[v].drum_phase2 = CRASH_METAL_INC;
            }
            voices[v].gain = drum_gain[note];
        }
    } else {
        voices[v].is_noise  = 0;
        voices[v].phase_inc = hz_to_inc(button_hz[note]);
        voices[v].gain      = 256;
    }
}

static void note_off(int note)
{
    int v = find_voice_for_note(note);
    if (v < 0) return;
    if (voices[v].env_stage != ENV_IDLE) {
        voices[v].env_stage = ENV_RELEASE;
        voices[v].env_rate_fraction = 0;
    }
}

// Four independent voice slots are mixed sample-by-sample. Voice allocation
// is deliberately separate from the I2S ISR so polyphony cannot block the
// audio transport interrupt.
static int32_t synth_next_sample()
{
    if (AUDIO_DIAGNOSTIC_TONE) {
        bool any_note = false;
        for (int i = 0; i < NUM_VOICES; i++) {
            if (voices[i].note >= 0 &&
                voices[i].env_stage != ENV_IDLE &&
                voices[i].env_stage != ENV_RELEASE) {
                any_note = true;
                break;
            }
        }

        if (!any_note)
            return 0;

        int32_t sample = sine_lookup_fast(diag_phase);
        diag_phase += DIAG_PHASE_INC;
        return (int32_t)(((int64_t)sample * volume) / VOLUME_MAX);
    }
    const Adsr *adsr = &PRESETS[preset_idx];
    int64_t mix = 0;

    if (preset_idx == 0) {
        organ_lfo_phase += ORGAN_LFO_INC;
        organ_tremolo_phase += ORGAN_TREMOLO_INC;
    } else if (preset_idx == PRESET_SAX) {
        sax_vibrato_phase += SAX_VIBRATO_INC;
    } else if (preset_idx == PRESET_VLN) {
        vln_vibrato_phase += VLN_VIBRATO_INC;
    }

    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *v = &voices[i];
        int32_t el = v->env_level;

        switch (v->env_stage) {
        case ENV_ATTACK:
            el += env_increment(v->env_rate_fraction, adsr->attack);
            if (el >= ENV_ONE) {
                el = ENV_ONE;
                v->env_stage = ENV_DECAY;
                v->env_rate_fraction = 0;
            }
            break;
        case ENV_DECAY:
            // Hi-hat gets its own short decay so it behaves like a struck
            // cymbal rather than a sustained pitched instrument.
                el -= (preset_idx == PRESET_DRUM && v->note == 2)
                    ? HH_DECAY
                    : env_increment(v->env_rate_fraction, adsr->decay);
            if (el <= adsr->sustain) {
                el = adsr->sustain;
                v->env_rate_fraction = 0;
                if (adsr->sustain > 0)
                    v->env_stage = ENV_SUSTAIN;
                else {
                    v->env_stage = ENV_IDLE;
                    v->phase_inc = 0;
                    // Keep S3 alive while its independent noise tail decays.
                    if (!(preset_idx == PRESET_DRUM && v->note == 2 &&
                          v->hat_noise_level > 0))
                        v->note = -1;
                    el = 0;
                }
            }
            break;
        case ENV_SUSTAIN:
            el = adsr->sustain;
            v->env_rate_fraction = 0;
            break;
        case ENV_RELEASE: {
            el -= env_increment(v->env_rate_fraction, adsr->release);
            if (el <= 0) {
                el = 0;
                v->env_stage = ENV_IDLE;
                v->env_rate_fraction = 0;
                v->phase_inc = 0;
                v->note = -1;
            }
            break;
        }
        default:
            el = 0;
            break;
        }

        v->env_level = el;
        const bool hat_tail = (preset_idx == PRESET_DRUM && v->note == 2 &&
                               v->hat_noise_level > 0);
        if (el == 0 && !hat_tail)
            continue;

        int32_t raw;
        int32_t extra_noise = 0;
        if (preset_idx == PRESET_DRUM && v->note == 0) {
            // Clean kick body. The phase increment is swept downward after
            // mixing; this branch contains no noise or phase warping.
            raw = sine_lookup_fast(v->phase);
        } else if (preset_idx == PRESET_DRUM && v->note == 2) {
            // Closed hi-hat: a short metallic transient plus filtered noise.
            // The noise is deliberately band-limited in the upper mids/highs;
            // this is the main sound, while the inharmonic oscillators add the
            // metallic "chick" rather than acting as pitched notes.
            const int32_t n = next_noise();
            v->hat_noise_hp += (n - v->hat_noise_hp) >> 2;
            const int32_t hp = n - v->hat_noise_hp;
            v->hat_noise_bp += (hp - v->hat_noise_bp) >> 2;
            extra_noise = v->hat_noise_bp;

            // Six inharmonic square components, mixed very quietly.
            // Their job is to give the noise a metallic edge, not distinct
            // spectral pitches.
            const uint32_t m =
                ((v->perc_phase >> 31) ^ (v->drum_phase2 >> 31) ^
                 (v->hat_phase3 >> 31) ^ (v->hat_phase4 >> 31));
            int32_t metal = m ? (SINE_PEAK >> 4) : -(SINE_PEAK >> 4);
            metal += sine_lookup_fast(v->perc_phase) >> 3;
            metal += sine_lookup_fast(v->drum_phase2) >> 3;
            metal += sine_lookup_fast(v->hat_phase3) >> 4;
            metal += sine_lookup_fast(v->hat_phase4) >> 4;
            raw = metal;
        } else if (preset_idx == PRESET_DRUM && v->note == 7) {
            // Crash: several inharmonic components with stronger upper
            // partials for a longer, brighter metallic ring.
            int32_t metal = sine_lookup_fast(v->phase);
            metal += sine_lookup_fast(v->phase * 2u) >> 1;
            metal += sine_lookup_fast(v->phase * 3u) >> 1;
            metal += sine_lookup_fast(v->phase * 4u) >> 2;
            metal += sine_lookup_fast(v->perc_phase) >> 1;
            raw = metal >> 1;
        } else if (v->is_noise) {
            raw = next_noise() >> 2;
        } else {
            uint32_t voice_phase = v->phase;

            if (AUDIO_POLYPHONY_TEST) {
                // Real voice allocator, ADSR, mixer and I2S path; cheap
                // oscillator only. This isolates polyphony from organ DSP.
                raw = sine_lookup_fast(voice_phase);
            } else if (preset_idx == 0) {
                // ~6 Hz Hammond vibrato, about ±0.15% pitch deviation.
                const int32_t lfo = sine_lookup_fast(organ_lfo_phase);
                const uint32_t vibrato_offset =
                    (uint32_t)((lfo >> 10) * 24);
                voice_phase += vibrato_offset;
                raw = organ_lookup(voice_phase) * 2;

                // Very low-level tonewheel leakage: a little extra harmonic
                // energy keeps the sound from being unnaturally sterile.
                raw += sine_lookup_fast(voice_phase * 7u) >> 6;

                // Short broadband key click at note-on. The click is only
                // active for ORG and decays independently of the sustain
                // envelope, like the mechanical contact transient.
                if (v->click_level) {
                    const int32_t click =
                        ((next_noise() >> 2) + (sine_lookup_fast(voice_phase * 13u) >> 2));
                    raw += (int32_t)(((int64_t)click * v->click_level) >> 16);
                    if (v->click_level > 12000)
                        v->click_level -= 12000;
                    else
                        v->click_level = 0;
                }
            } else if (preset_idx == PRESET_SAX) {
                const int32_t vibrato = sine_lookup_fast(
                    sax_vibrato_phase + v->age * 0x9E3779B9u);
                voice_phase += (uint32_t)((vibrato >> 8) * 200);
                raw = instrument_lookup(sax_table, voice_phase);
                raw += next_noise() >> 4;
            } else if (preset_idx == PRESET_VLN) {
                const int32_t vibrato = sine_lookup_fast(
                    vln_vibrato_phase + v->age * 0x9E3779B9u);
                voice_phase += (uint32_t)((vibrato >> 8) * 400);
                raw = instrument_lookup(vln_table, voice_phase);
            } else {
                raw = sine_lookup_fast(voice_phase);
            }
        }

        if (!AUDIO_POLYPHONY_TEST && preset_idx == 0) {
            // Compact mono approximation of Hammond C/V:
            // a slow amplitude cycle plus a much smaller, slightly faster
            // phase modulation. The two rates are intentionally not locked
            // so the sound has continuously changing motion instead of a
            // static vibrato effect.
            const int32_t trem = sine_lookup_fast(organ_tremolo_phase);
            // Keep the hot path 32-bit on Cortex-M0+. trem_gain is scaled
            // to 8 fractional bits before multiplication, avoiding another
            // expensive 64-bit multiply for every voice/sample.
            const int32_t trem_gain_q8 = 256 + (trem >> 23);
            raw = (raw >> 8) * trem_gain_q8;
        }

        /* Preserve the known-good Rev2 fixed-point scaling exactly. The
         * synthesis now runs outside the I2S ISR, so the M0+ 64-bit math
         * no longer consumes the real-time interrupt budget. */
        // Keep the per-voice amplitude scaling entirely in 32-bit math.
        // On Cortex-M0+ this is substantially cheaper than a 64-bit multiply
        // for every voice/sample, while retaining adequate 16-bit envelope
        // resolution for the audio path.
        int32_t scaled = 0;
        if (preset_idx == PRESET_DRUM && v->note == 2) {
            // Metallic body follows the 115 ms envelope. Noise starts low,
            // becomes relatively more prominent as the ring disappears, and
            // continues on its own 220 ms tail.
            if (el > 0)
                scaled = (raw >> 8) * (el >> 8);
            if (v->hat_noise_level > 0) {
                const int32_t level_q8 = v->hat_noise_level >> 8;
                const int32_t rise_q8 = 80 + ((ENV_ONE - v->hat_noise_level) >> 8);
                int32_t noise_scaled = ((extra_noise >> 2) * level_q8) >> 8;
                noise_scaled = (noise_scaled * rise_q8) >> 8;
                scaled += noise_scaled;
            }
        } else {
            scaled = (raw >> 8) * (el >> 8);
        }
        if (v->gain != 256)
            scaled = (scaled * v->gain) >> 8;
        mix += scaled;
        v->phase += v->phase_inc;
        if (preset_idx == PRESET_DRUM) {
            if (v->note == 2) {
                v->perc_phase += HH_METAL_INC1;
                v->drum_phase2 += HH_METAL_INC2;
                v->hat_phase3 += HH_METAL_INC3;
                v->hat_phase4 += HH_METAL_INC4;
                if (v->hat_noise_level > HH_NOISE_DECAY) {
                    v->hat_noise_level -= HH_NOISE_DECAY;
                } else {
                    v->hat_noise_level = 0;
                    if (v->env_stage == ENV_IDLE && v->env_level == 0) {
                        v->note = -1;
                        v->phase_inc = 0;
                    }
                }
            } else if (v->note == 7) {
                v->perc_phase += CRASH_METAL_INC;
                v->drum_phase2 += (uint32_t)(((uint64_t)8800 << 32) / SAMPLE_RATE_HZ);
            }
        }
        if (preset_idx == PRESET_DRUM && v->note == 0 &&
            v->phase_inc > KICK_END_INC) {
            uint32_t next_inc = v->phase_inc - KICK_INC_STEP;
            v->phase_inc = (next_inc < KICK_END_INC) ? KICK_END_INC : next_inc;
        }
    }

    // Keep a fixed headroom budget for the four-voice mixer. The organ
    // waveform contains several drawbars plus click/leakage, so reserve
    // additional headroom rather than relying on the final clip limiter.
    if (preset_idx == PRESET_DRUM)
        mix = (mix * 5) / 4;
    else
        mix /= (NUM_VOICES * 2);

    mix = (mix * volume) / VOLUME_MAX;

    if (mix > 1800000000LL) mix = 1800000000LL;
    if (mix < -1800000000LL) mix = -1800000000LL;

    return (int32_t)mix;
}

static void audio_buffer_fill(uint16_t max_frames = 32)
{
    uint16_t produced = 0;
    while (produced < max_frames) {
        uint16_t write = audio_write_index;
        uint16_t next = (write + 1) & AUDIO_BUFFER_MASK;

        if (next == audio_read_index)
            break;

        audio_buffer[write] = synth_next_sample();
        audio_write_index = next;
        produced++;
    }
}

extern "C" void I2S_Handler(void)
{
    if (!(I2S->INTFLAG.bit.TXRDY1))
        return;

    if (i2s_slot == 0) {
        uint16_t read = audio_read_index;

        if (read == audio_write_index) {
            // Output silence on an underrun rather than stale data.
            i2s_hold = 0;
            audio_underruns++;
        } else {
            i2s_hold = audio_buffer[read];
            audio_read_index = (read + 1) & AUDIO_BUFFER_MASK;
        }

        I2S->DATA[1].reg = (uint32_t)i2s_hold;
        i2s_slot = 1;
    } else {
        I2S->DATA[1].reg = (uint32_t)i2s_hold;
        i2s_slot = 0;
    }
}

static void wait_gclk(void)
{
    for (uint32_t i = 0; i < 100000u && GCLK->STATUS.bit.SYNCBUSY; i++) {}
}

static void wait_i2s(uint32_t mask)
{
    for (uint32_t i = 0; i < 100000u && (I2S->SYNCBUSY.reg & mask); i++) {}
}

static void configure_i2s(void)
{
    PM->APBCMASK.reg |= PM_APBCMASK_I2S;

    GCLK->CLKCTRL.reg =
        GCLK_CLKCTRL_ID(35) |
        GCLK_CLKCTRL_GEN_GCLK0 |
        GCLK_CLKCTRL_CLKEN;
    wait_gclk();

    PORT->Group[0].PINCFG[8].reg  |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[4].bit.PMUXE = 0x6;

    PORT->Group[0].PINCFG[10].reg |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[5].bit.PMUXE = 0x6;

    PORT->Group[0].PINCFG[11].reg |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[5].bit.PMUXO = 0x6;

    I2S->CTRLA.reg = I2S_CTRLA_SWRST;
    wait_i2s(I2S_SYNCBUSY_SWRST);

    I2S->CLKCTRL[0].reg =
        I2S_CLKCTRL_SLOTSIZE(3) |
        I2S_CLKCTRL_NBSLOTS(1) |
        I2S_CLKCTRL_FSWIDTH_HALF |
        I2S_CLKCTRL_BITDELAY |
        I2S_CLKCTRL_FSSEL_SCKDIV |
        I2S_CLKCTRL_SCKSEL_MCKDIV |
        I2S_CLKCTRL_MCKSEL_GCLK |
        I2S_CLKCTRL_MCKEN |
        I2S_CLKCTRL_MCKDIV(I2S_MCK_DIV);

    I2S->SERCTRL[1].reg =
        I2S_SERCTRL_SERMODE_TX |
        I2S_SERCTRL_SLOTADJ_LEFT |
        I2S_SERCTRL_DATASIZE_32 |
        I2S_SERCTRL_CLKSEL_CLK0;

    I2S->INTENSET.reg = I2S_INTENSET_TXRDY1;
    NVIC_SetPriority(I2S_IRQn, 0);
    NVIC_EnableIRQ(I2S_IRQn);

    I2S->CTRLA.reg =
        I2S_CTRLA_ENABLE |
        I2S_CTRLA_CKEN0  |
        I2S_CTRLA_SEREN1;

    wait_i2s(I2S_SYNCBUSY_ENABLE |
             I2S_SYNCBUSY_CKEN0  |
             I2S_SYNCBUSY_SEREN1);

    i2s_slot = 0;
    I2S->DATA[1].reg = 0;
}


void audio_engine_initialize()
{
    sine_table_init();
    instrument_tables_init();
    voices_init();
    audio_buffer_fill(AUDIO_BUFFER_FRAMES - 1);
    configure_i2s();
}

void audio_engine_fill(uint16_t max_frames)
{
    audio_buffer_fill(max_frames);
}

void audio_engine_note_on(int note)
{
    note_on(note);
}

void audio_engine_note_off(int note)
{
    note_off(note);
}

bool audio_engine_adjust_volume(int delta)
{
    int next = volume + delta;
    if (next < 0) next = 0;
    if (next > VOLUME_MAX) next = VOLUME_MAX;
    if (next == volume) return false;
    volume = next;
    return true;
}

bool audio_engine_step_preset(int delta)
{
    int next = (int)preset_idx + delta;
    if (next < 0) next = 0;
    if (next >= NUM_PRESETS) next = NUM_PRESETS - 1;
    if (next == (int)preset_idx) return false;
    preset_idx = (uint8_t)next;
    return true;
}

int audio_engine_volume()
{
    return volume;
}

uint8_t audio_engine_preset_index()
{
    return preset_idx;
}

uint32_t audio_engine_underruns()
{
    return audio_underruns;
}
