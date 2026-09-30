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

// I2S clocking: 48 MHz GCLK0 / (MCKDIV + 1) = 2 MHz SCK.
// Two 32-bit slots per frame => 64 BCLKs per audio sample.
// 31.25 kHz gives the SAMD21 more CPU time per rendered sample while
// remaining well within the MAX98357A's supported audio-rate range.
static constexpr uint32_t I2S_GCLK_HZ       = 48000000;
static constexpr uint32_t I2S_MCK_DIV       = 23;
static constexpr uint32_t I2S_SLOTS         = 2;
static constexpr uint32_t I2S_BITS_PER_SLOT = 32;
static constexpr uint32_t SAMPLE_RATE_HZ =
    I2S_GCLK_HZ / (I2S_MCK_DIV + 1) / (I2S_SLOTS * I2S_BITS_PER_SLOT);
static constexpr uint32_t SINE_LEN       = 1024;
static constexpr int      VOLUME_MAX     = 64;
static constexpr int      VOLUME_STEP    = 1;
// Maximum simultaneous notes. The mixer renders every active voice into
// one PCM sample before the I2S ISR sends it to the MAX98357A.
static constexpr int      NUM_VOICES     = 4;
static constexpr int      NUM_PRESETS    = 8;
static constexpr int      PRESET_EDGES   = 4;  // quadrature edges per detent
static constexpr int      PRESET_DRUM    = 7;  // index of drum mode
static constexpr int      PRESET_SAX     = 5;
static constexpr int      PRESET_VLN     = 6;

// Temporary transport diagnostic. When enabled, a held key produces a
// deterministic 523 Hz sine through the same ring buffer and I2S ISR.
// This removes ADSR/voice allocation from the test without changing
// the encoder or TM1638 handling.
static constexpr bool AUDIO_DIAGNOSTIC_TONE = false;
// CPU-isolation test: use the real 4-voice ADSR/mixer/voice allocator, but
// replace the expensive instrument DSP with a plain sine oscillator.
static constexpr bool AUDIO_POLYPHONY_TEST = true;
static constexpr uint32_t DIAG_PHASE_INC =
    (uint32_t)(((uint64_t)523 << 32) / SAMPLE_RATE_HZ);
static uint32_t diag_phase = 0;

static constexpr int32_t SINE_PEAK = 280000000;
static constexpr int32_t ENV_ONE   = 65536;

static int32_t sine_table[SINE_LEN];

static const uint16_t button_hz[8] = {
    523, 587, 659, 698, 784, 880, 988, 1047
};

/* Drum pitch map (Hz) – approximate classic kit tones */
/*
 * How each drum is synthesized:
 *   S1 Kick  – 190→55 Hz sine pitch sweep, gain 1.0x, no noise
 *   S2 Snare – sine 200 Hz, gain 1.0x, pure tone
 *   S3 HH    – controlled fundamental + one independent metallic partial
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

static const uint8_t phys_bit[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

static const uint8_t SEG_DIGIT[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};
static constexpr uint8_t SEG_BLANK = 0x00;
static constexpr uint8_t SEG_A = 0x77;
static constexpr uint8_t SEG_B = 0x7C;
static constexpr uint8_t SEG_D = 0x5E;
static constexpr uint8_t SEG_G = 0x3D;
static constexpr uint8_t SEG_K = 0x75;
static constexpr uint8_t SEG_L = 0x38;
static constexpr uint8_t SEG_M = 0x37;
static constexpr uint8_t SEG_N = 0x54;
static constexpr uint8_t SEG_O = 0x3F;
static constexpr uint8_t SEG_P = 0x73;
static constexpr uint8_t SEG_R = 0x50;
static constexpr uint8_t SEG_S = 0x6D;
static constexpr uint8_t SEG_V = 0x3E;
static constexpr uint8_t SEG_X = 0x76;

enum EnvStage : uint8_t { ENV_IDLE = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

struct Adsr {
    int32_t attack;
    int32_t decay;
    int32_t sustain;
    int32_t release;
    uint8_t label[4];
};

// Envelope rates are specified in milliseconds and converted to fixed-point
// increments for the current SAMPLE_RATE_HZ. This keeps the musical timing
// stable if the audio sample rate changes.
//
// attack_ms / decay_ms / release_ms are approximate times to traverse the
// corresponding envelope range. A zero value means an immediate transition.
static constexpr int32_t env_rate_from_ms(uint32_t ms)
{
    return (ms == 0)
        ? ENV_ONE
        : (int32_t)((((ENV_ONE * 1000ULL) /
                       ((uint64_t)ms * SAMPLE_RATE_HZ)) == 0)
                    ? 1
                    : ((ENV_ONE * 1000ULL) /
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
 * SAX – ~70 ms attack, ~300 ms decay toward 75%, ~700 ms release
 * VLN – ~120 ms attack, ~500 ms decay toward 75%, ~2 s release
 * DRM – instant attack, ~150 ms decay, no audible release
 */
static const Adsr PRESETS[NUM_PRESETS] = {
    { env_rate_from_ms(2),    env_rate_from_ms(0),   ENV_ONE,             env_rate_from_ms(2000), { SEG_O, SEG_R, SEG_G, SEG_BLANK } }, // ORG
    { env_rate_from_ms(1),    env_rate_from_ms(180),  0,                  env_rate_from_ms(250),  { SEG_P, SEG_L, SEG_K, SEG_BLANK } }, // PLK
    { env_rate_from_ms(1000), env_rate_from_ms(0),   ENV_ONE,             env_rate_from_ms(2000), { SEG_P, SEG_A, SEG_D, SEG_BLANK } }, // PAD
    { env_rate_from_ms(150),  env_rate_from_ms(500), ENV_ONE / 2,         env_rate_from_ms(1000), { SEG_B, SEG_R, SEG_S, SEG_BLANK } }, // BRS
    { env_rate_from_ms(5),    env_rate_from_ms(2000), 0,                  env_rate_from_ms(1000), { SEG_P, SEG_N, SEG_O, SEG_BLANK } }, // PNO
    { env_rate_from_ms(70),   env_rate_from_ms(300), ENV_ONE * 3 / 4,     env_rate_from_ms(700),  { SEG_S, SEG_A, SEG_X, SEG_BLANK } }, // SAX
    { env_rate_from_ms(120),  env_rate_from_ms(500), ENV_ONE * 3 / 4,     env_rate_from_ms(2000), { SEG_V, SEG_L, SEG_N, SEG_BLANK } }, // VLN
    { env_rate_from_ms(1),    env_rate_from_ms(150), 0,                  env_rate_from_ms(300),  { SEG_D, SEG_R, SEG_M, SEG_BLANK } }, // DRM
};

enum UiMode : uint8_t { MODE_VOLUME = 0, MODE_PRESET = 1 };

static volatile int     volume     = VOLUME_MAX / 4;
static volatile uint8_t preset_idx = 0;
static volatile uint8_t ui_mode    = MODE_VOLUME;

struct Voice {
    volatile uint32_t phase;
    volatile uint32_t phase_inc;
    volatile int32_t  env_level;
    volatile uint8_t  env_stage;
    volatile int8_t   note;
    volatile uint32_t age;
    volatile uint8_t  is_noise;       // 1 = noise source (hi-hat / crash)
    volatile uint16_t gain;           // 256 = unity
    volatile uint16_t click_level;     // Hammond key-click transient, 0..65535
    volatile uint32_t perc_phase;      // secondary percussion/metal oscillator phase
    volatile uint32_t drum_phase2;     // independent drum partial phase
    volatile uint16_t perc_level;      // percussion decay
    volatile uint16_t hat_noise_level; // hi-hat noise tail
};

static Voice voices[NUM_VOICES];
static volatile uint32_t voice_age_counter = 0;
static volatile uint32_t noise_lfsr = 0xACE1u;  // 16-bit LFSR seed
static volatile int32_t  noise_lpf  = 0;         // one-pole low-pass state

// Audio producer/consumer buffer. The main loop renders audio; the I2S ISR
// only moves already-rendered samples into the I2S DATA register.
// Ring-buffer indexing uses a bit mask, so the frame count MUST be a power of two.\n// 1024 fits the SAMD21E18A SRAM and avoids the invalid wrap behavior of 1408.\n// Ring-buffer indexing uses a bit mask, so the frame count MUST be a power of two.
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

static int32_t sax_table[SINE_LEN];
static int32_t vln_table[SINE_LEN];
static int32_t organ_harmonic_table[SINE_LEN];
static int32_t organ_sub_table[SINE_LEN];      // 16' (0.5x), weighted
static int32_t organ_5th3_table[SINE_LEN];     // 5 1/3' (1.5x), weighted

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

static void instrument_tables_init()
{
    for (uint32_t i = 0; i < SINE_LEN; i++) {
        const double a = 2.0 * M_PI * (double)i / (double)SINE_LEN;

        // SAX and VLN are precomputed. ORG keeps the Hammond drawbar
        // spectrum, but the integer harmonics are also precomputed so the
        // real-time audio path does not perform 64-bit multiplies/divides.
        double sax = sin(a)
                  + 0.55 * sin(3.0 * a)
                  + 0.30 * sin(5.0 * a)
                  + 0.18 * sin(7.0 * a)
                  + 0.10 * sin(2.0 * a);

        double vln = sin(a)
                  + 0.70 * sin(2.0 * a)
                  + 0.50 * sin(3.0 * a)
                  + 0.35 * sin(4.0 * a)
                  + 0.25 * sin(5.0 * a)
                  + 0.15 * sin(6.0 * a);

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

        sax_table[i] = (int32_t)(sax * (double)SINE_PEAK / 2.13);
        vln_table[i] = (int32_t)(vln * (double)SINE_PEAK / 2.95);
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
    return table[(ph >> 22) & (SINE_LEN - 1)];
}

static inline int32_t organ_lookup(uint32_t ph)
{
    // All Hammond drawbars are precomputed. The two fractional drawbars
    // use transformed phase indices so their waveforms are continuous.
    const uint32_t fundamental_idx = (ph >> 22) & (SINE_LEN - 1);
    const uint32_t sub_idx =
        ((ph >> 1) >> 22) & (SINE_LEN - 1);
    const uint32_t fifth3_idx =
        ((ph + (ph >> 1)) >> 22) & (SINE_LEN - 1);

    return organ_harmonic_table[fundamental_idx]
         + organ_sub_table[sub_idx]
         + organ_5th3_table[fifth3_idx];
}


static int32_t sine_lookup(uint32_t ph)
{
    uint32_t idx  = ph >> 22;
    uint32_t frac = (ph >> 12) & 0x3FFu;
    int32_t  s0   = sine_table[idx & (SINE_LEN - 1)];
    int32_t  s1   = sine_table[(idx + 1) & (SINE_LEN - 1)];
    return s0 + (int32_t)(((int64_t)(s1 - s0) * frac) >> 10);
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
        voices[i].note      = -1;
        voices[i].age       = 0;
        voices[i].is_noise  = 0;
        voices[i].gain        = 256;
        voices[i].click_level = 0;
        voices[i].perc_phase  = 0;
        voices[i].drum_phase2 = 0;
        voices[i].perc_level  = 0;
        voices[i].hat_noise_level = 0;
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
    if (v < 0)
        v = alloc_voice();

    voices[v].note      = (int8_t)note;
    voices[v].phase     = 0;
    voices[v].env_level = 0;
    voices[v].env_stage = ENV_ATTACK;
    voices[v].age         = ++voice_age_counter;
    voices[v].click_level = (preset_idx == 0) ? ENV_ONE : 0;
    voices[v].perc_phase  = 0;
    voices[v].drum_phase2 = 0;
    voices[v].perc_level  = (preset_idx == 0) ? ENV_ONE : 0;

    if (preset_idx == PRESET_DRUM) {
        /* Hi-hat (2) and Crash (7) use noise; others use sine body */
        if (note == 2) {
            // S3: proven 523 Hz fundamental plus one independently phased
            // 2.8 kHz metallic partial. No noise or phase multiplication.
            voices[v].is_noise    = 0;
            voices[v].phase_inc   = DIAG_PHASE_INC;
            voices[v].perc_phase  = HH_METAL_INC1;
            voices[v].drum_phase2 = HH_METAL_INC2;
            voices[v].perc_level  = ENV_ONE;
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
    if (voices[v].env_stage != ENV_IDLE)
        voices[v].env_stage = ENV_RELEASE;
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

        int32_t sample = sine_lookup(diag_phase);
        diag_phase += DIAG_PHASE_INC;
        return (int32_t)(((int64_t)sample * volume) / VOLUME_MAX);
    }
    const Adsr *adsr = &PRESETS[preset_idx];
    int64_t mix = 0;

    if (preset_idx == 0)
        organ_lfo_phase += ORGAN_LFO_INC;

    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *v = &voices[i];
        int32_t el = v->env_level;

        switch (v->env_stage) {
        case ENV_ATTACK:
            el += adsr->attack;
            if (el >= ENV_ONE) {
                el = ENV_ONE;
                v->env_stage = ENV_DECAY;
            }
            break;
        case ENV_DECAY:
            // Hi-hat gets its own short decay so it behaves like a struck
            // cymbal rather than a sustained pitched instrument.
            el -= (preset_idx == PRESET_DRUM && v->note == 2)
                    ? HH_DECAY : adsr->decay;
            if (el <= adsr->sustain) {
                el = adsr->sustain;
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
            break;
        case ENV_RELEASE: {
            el -= adsr->release;
            if (el <= 0) {
                el = 0;
                v->env_stage = ENV_IDLE;
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
            // Hi-hat: keep only a small low-frequency body and build the
            // metallic character from three independently phased partials.
            // The partials are intentionally modest to avoid alias-heavy
            // waveforms while making the upper-frequency ring audible.
            int32_t metal = sine_lookup_fast(v->phase) >> 4;
            // A small band-limited-looking noise component adds the soft
            // stick/air component of a real hi-hat without making the sound
            // collapse into broadband hiss.
            // Actual noise is mixed separately so its longer tail is not
            // forced to follow the metallic envelope.
            extra_noise = next_noise();
            metal += sine_lookup_fast(v->perc_phase) >> 1;
            metal += sine_lookup_fast(v->drum_phase2) >> 1;
            metal += sine_lookup_fast(v->drum_phase2 + HH_METAL_INC3) >> 2;
            metal += sine_lookup_fast(v->drum_phase2 + HH_METAL_INC4) >> 3;
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
                raw = organ_lookup(voice_phase);

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
                raw = instrument_lookup(sax_table, voice_phase);
            } else if (preset_idx == PRESET_VLN) {
                raw = instrument_lookup(vln_table, voice_phase);
            } else {
                raw = sine_lookup(voice_phase);
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
            const int32_t trem_gain_q8 = 256 + (trem >> 12);
            raw = (int32_t)(((raw >> 8) * trem_gain_q8) >> 8);
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
                const int32_t rise_q8 = 32 + ((ENV_ONE - v->hat_noise_level) >> 9);
                int32_t noise_scaled = ((extra_noise >> 6) * level_q8) >> 8;
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
                if (v->hat_noise_level > HH_NOISE_DECAY)
                    v->hat_noise_level -= HH_NOISE_DECAY;
                else
                    v->hat_noise_level = 0;
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

static void led_init(void)
{
    PORT->Group[0].DIRSET.reg = PORT_PA17;
    PORT->Group[0].OUTCLR.reg = PORT_PA17;
}
static void led_on(void)  { PORT->Group[0].OUTSET.reg = PORT_PA17; }
static void led_off(void) { PORT->Group[0].OUTCLR.reg = PORT_PA17; }

static void led_blink_n(int n, uint16_t on_ms = 80, uint16_t off_ms = 80)
{
    for (int i = 0; i < n; i++) {
        led_on();  delay(on_ms);
        led_off(); delay(off_ms);
    }
    delay(150);
}

static uint8_t enc_prev = 0;
static bool    sw_prev  = true;
static int8_t  preset_accum = 0;

static void encoder_init(void)
{
    PORT->Group[0].DIRCLR.reg = PORT_PA14 | PORT_PA15 | PORT_PA22;
    PORT->Group[0].PINCFG[14].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].PINCFG[15].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].PINCFG[22].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].OUTSET.reg = PORT_PA14 | PORT_PA15 | PORT_PA22;

    uint32_t in = PORT->Group[0].IN.reg;
    enc_prev = ((in & PORT_PA14) ? 1 : 0) | ((in & PORT_PA15) ? 2 : 0);
    sw_prev  = (in & PORT_PA22) != 0;
}

static bool encoder_poll(void)
{
    static const int8_t table[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0
    };

    uint32_t in = PORT->Group[0].IN.reg;
    uint8_t curr = ((in & PORT_PA14) ? 1 : 0) | ((in & PORT_PA15) ? 2 : 0);
    int8_t delta = table[(enc_prev << 2) | curr];
    enc_prev = curr;

    bool changed = false;

    if (delta) {
        if (ui_mode == MODE_VOLUME) {
            int v = volume + delta * VOLUME_STEP;
            if (v < 0)          v = 0;
            if (v > VOLUME_MAX) v = VOLUME_MAX;
            volume = v;
            changed = true;
        } else {
            // Accumulate edges; one preset step per detent
            preset_accum += delta;
            if (preset_accum >= PRESET_EDGES || preset_accum <= -PRESET_EDGES) {
                int step = (preset_accum > 0) ? 1 : -1;
                preset_accum = 0;
                int p = (int)preset_idx + step;
                if (p < 0)            p = 0;
                if (p >= NUM_PRESETS) p = NUM_PRESETS - 1;
                if (p != (int)preset_idx) {
                    preset_idx = (uint8_t)p;
                    changed = true;
                }
            }
        }
    }

    bool sw = (in & PORT_PA22) != 0;
    if (sw_prev && !sw) {
        ui_mode = (ui_mode == MODE_VOLUME) ? MODE_PRESET : MODE_VOLUME;
        preset_accum = 0;
        changed = true;
    }
    sw_prev = sw;

    return changed;
}

static void tm_delay(void)
{
    for (volatile int i = 0; i < 8; i++) {}
}

static void tm_stb_low(void)  { PORT->Group[0].OUTCLR.reg = PORT_PA16; }
static void tm_stb_high(void) { PORT->Group[0].OUTSET.reg = PORT_PA16; }
static void tm_clk_low(void)  { PORT->Group[0].OUTCLR.reg = PORT_PA18; }
static void tm_clk_high(void) { PORT->Group[0].OUTSET.reg = PORT_PA18; }
static void tm_dio_out(void)  { PORT->Group[0].DIRSET.reg = PORT_PA19; }

static void tm_dio_in(void)
{
    PORT->Group[0].DIRCLR.reg = PORT_PA19;
    PORT->Group[0].PINCFG[19].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].OUTSET.reg = PORT_PA19;
}

static void tm_dio_write(bool v)
{
    if (v) PORT->Group[0].OUTSET.reg = PORT_PA19;
    else   PORT->Group[0].OUTCLR.reg = PORT_PA19;
}

static bool tm_dio_read(void)
{
    return (PORT->Group[0].IN.reg & PORT_PA19) != 0;
}

static void tm_write_byte(uint8_t data)
{
    tm_dio_out();
    for (int i = 0; i < 8; i++) {
        tm_clk_low();
        tm_dio_write(data & 0x01);
        tm_delay();
        tm_clk_high();
        tm_delay();
        data >>= 1;
    }
}

static uint8_t tm_read_byte(void)
{
    uint8_t data = 0;
    tm_dio_in();
    for (int i = 0; i < 8; i++) {
        data >>= 1;
        tm_clk_low();
        tm_delay();
        if (tm_dio_read()) data |= 0x80;
        tm_clk_high();
        tm_delay();
    }
    return data;
}

static void tm_cmd(uint8_t cmd)
{
    tm_stb_low();
    tm_write_byte(cmd);
    tm_stb_high();
}

static void tm_write_digit(uint8_t pos, uint8_t seg)
{
    tm_stb_low();
    tm_write_byte(0xC0 + (pos * 2));
    tm_write_byte(seg);
    tm_stb_high();
}

static void tm_show_ui(void)
{
    if (ui_mode == MODE_VOLUME) {
        tm_write_digit(0, SEG_V);
        tm_write_digit(1, SEG_O);
        tm_write_digit(2, SEG_L);
        tm_write_digit(3, SEG_BLANK);

        int vol = volume;
        tm_write_digit(4, (vol >= 1000) ? SEG_DIGIT[(vol / 1000) % 10] : SEG_BLANK);
        tm_write_digit(5, (vol >= 100)  ? SEG_DIGIT[(vol / 100) % 10]  : SEG_BLANK);
        tm_write_digit(6, (vol >= 10)   ? SEG_DIGIT[(vol / 10) % 10]   : SEG_BLANK);
        tm_write_digit(7, SEG_DIGIT[vol % 10]);
    } else {
        const uint8_t *lab = PRESETS[preset_idx].label;
        tm_write_digit(0, lab[0]);
        tm_write_digit(1, lab[1]);
        tm_write_digit(2, lab[2]);
        tm_write_digit(3, lab[3]);

        int n = preset_idx + 1;
        tm_write_digit(4, SEG_BLANK);
        tm_write_digit(5, SEG_BLANK);
        tm_write_digit(6, SEG_BLANK);
        tm_write_digit(7, SEG_DIGIT[n % 10]);
    }
}

static void tm_init(void)
{
    PORT->Group[0].DIRSET.reg = PORT_PA16 | PORT_PA18 | PORT_PA19;
    PORT->Group[0].PINCFG[16].reg = 0;
    PORT->Group[0].PINCFG[18].reg = 0;
    PORT->Group[0].PINCFG[19].reg = 0;
    tm_stb_high();
    tm_clk_high();
    tm_dio_write(true);

    tm_cmd(0x8F);
    tm_cmd(0x40);

    tm_stb_low();
    tm_write_byte(0xC0);
    for (int i = 0; i < 16; i++)
        tm_write_byte(0x00);
    tm_stb_high();

    tm_show_ui();
}

static void tm_set_leds(uint8_t note_mask)
{
    for (int n = 0; n < 8; n++) {
        uint8_t bit = phys_bit[n];
        tm_stb_low();
        tm_write_byte(0xC1 + (bit * 2));
        tm_write_byte((note_mask & (1u << n)) ? 0x01 : 0x00);
        tm_stb_high();
    }
}

static uint8_t tm_read_keys_raw(void)
{
    uint8_t raw[4];
    tm_stb_low();
    tm_write_byte(0x42);
    for (int i = 0; i < 4; i++)
        raw[i] = tm_read_byte();
    tm_stb_high();
    tm_dio_out();

    uint8_t keys = 0;
    if (raw[0] & 0x01) keys |= (1u << 0);
    if (raw[1] & 0x01) keys |= (1u << 1);
    if (raw[2] & 0x01) keys |= (1u << 2);
    if (raw[3] & 0x01) keys |= (1u << 3);
    if (raw[0] & 0x10) keys |= (1u << 4);
    if (raw[1] & 0x10) keys |= (1u << 5);
    if (raw[2] & 0x10) keys |= (1u << 6);
    if (raw[3] & 0x10) keys |= (1u << 7);
    return keys;
}

static uint8_t keys_physical(uint8_t raw)
{
    uint8_t m = 0;
    for (int n = 0; n < 8; n++) {
        if (raw & (1u << phys_bit[n]))
            m |= (1u << n);
    }
    return m;
}

static void tm_poll(void)
{
    // Debounce each key independently. A key must be observed consistently
    // for five consecutive scans before its gate changes.
    static constexpr uint8_t KEY_THRESH = 5;
    static uint8_t counters[8] = {};
    static uint8_t debounced = 0;

    uint8_t raw = keys_physical(tm_read_keys_raw());

    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(1u << i);

        if (raw & bit) {
            if (counters[i] < KEY_THRESH)
                counters[i]++;

            if (counters[i] == KEY_THRESH && !(debounced & bit)) {
                debounced |= bit;
                note_on(i);
            }
        } else {
            if (counters[i] > 0)
                counters[i]--;

            if (counters[i] == 0 && (debounced & bit)) {
                debounced &= (uint8_t)~bit;
                note_off(i);
            }
        }
    }

    static uint8_t displayed = 0xFF;
    if (debounced != displayed) {
        tm_set_leds(debounced);
        displayed = debounced;
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

void setup()
{
    led_init();
    led_blink_n(3, 50, 50);

    sine_table_init();
    instrument_tables_init();
    voices_init();
    encoder_init();
    tm_init();

    // Pre-render the queue before starting the I2S consumer.
    audio_buffer_fill(AUDIO_BUFFER_FRAMES - 1);

    configure_i2s();

    led_on();
    delay(100);
}

void loop()
{
    // Keep encoder polling at the rate used by the known-good implementation.
    // The buffered audio producer still runs between polls, outside the I2S ISR.
    bool changed = false;

    // Keep the producer running continuously. The previous 200 us delay
    // artificially limited the available CPU time for the 4-voice synth and
    // could allow the audio queue to drain between fills.
    for (int i = 0; i < 64; i++) {
        if (encoder_poll())
            changed = true;

        audio_buffer_fill(64);
    }

    tm_poll();

    if (changed)
        tm_show_ui();
}
