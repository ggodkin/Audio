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

// I2S clocking: 48 MHz GCLK0 / (MCKDIV + 1) = 3 MHz SCK.
// Two 32-bit slots per frame => 64 BCLKs per audio sample.
static constexpr uint32_t I2S_GCLK_HZ       = 48000000;
static constexpr uint32_t I2S_MCK_DIV       = 15;
static constexpr uint32_t I2S_SLOTS         = 2;
static constexpr uint32_t I2S_BITS_PER_SLOT = 32;
static constexpr uint32_t SAMPLE_RATE_HZ =
    I2S_GCLK_HZ / (I2S_MCK_DIV + 1) / (I2S_SLOTS * I2S_BITS_PER_SLOT);
static constexpr uint32_t SINE_LEN       = 1024;
static constexpr int      VOLUME_MAX     = 64;
static constexpr int      VOLUME_STEP    = 1;
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
 *   S1 Kick  – sine 150 Hz, gain 2.0x, pure tone body
 *   S2 Snare – sine 200 Hz, gain 1.0x, pure tone
 *   S3 HH    – sine 7 kHz + light filtered noise, gain 1.1x (metallic)
 *   S4 Clap  – sine 280 Hz, gain 1.0x
 *   S5 TomL  – sine 160 Hz, gain 1.0x
 *   S6 TomM  – sine 220 Hz, gain 1.0x
 *   S7 Rim   – sine 500 Hz, gain 1.0x
 *   S8 Crash – sine 4.5 kHz + light filtered noise, gain 1.0x (metallic)
 * Envelope for all: global DRM ADSR (~40 ms decay, no sustain).
 */
static const uint16_t drum_hz[8] = {
    150,  // Kick
    200,  // Snare
    7000, // Hi-hat carrier
    280,  // Clap
    160,  // Tom low
    220,  // Tom mid
    500,  // Rim
    4500  // Crash carrier
};

/* Relative gain 0–256 (256 = unity). */
static const uint16_t drum_gain[8] = {
    512,  // Kick
    256,  // Snare
    280,  // Hi-hat (was inaudible at 64)
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

/*
 * Rates per sample at 46.875 kHz. Time ≈ ENV_ONE / rate / SAMPLE_RATE seconds.
 *
 * ORG – immediate attack, full sustain, ~2.2 s release; Hammond-style drawbars
 * PLK – instant on, ~15 ms decay to silence
 * PAD – ~1.0 s attack, full sustain, ~2 s release
 * BRS – ~0.2 s attack, decay to 50%, medium release
 * PNO – fast attack, slow decay while held, faster cut on release
 * SAX – medium attack, full sustain, medium release (+ 2nd harmonic)
 * VLN – slow bow attack, full sustain, longer release (+ 2nd harmonic)
 * DRM – instant on, ~40 ms decay (percussive)
 */
static const Adsr PRESETS[NUM_PRESETS] = {
    // Rates are fixed-point envelope increments per rendered sample.
    // The envelope state machine reaches SUSTAIN explicitly; RELEASE always
    // continues to zero after note_off().
    //
    // attack  decay  sustain           release
    { 1024,    256,   ENV_ONE,          8,   { SEG_O, SEG_R, SEG_G, SEG_BLANK } }, // ORG
    { 8192,    150,   0,                200, { SEG_P, SEG_L, SEG_K, SEG_BLANK } }, // PLK
    {    2,     16,   ENV_ONE,          1,   { SEG_P, SEG_A, SEG_D, SEG_BLANK } }, // PAD
    {   12,     40,   ENV_ONE / 2,      20,  { SEG_B, SEG_R, SEG_S, SEG_BLANK } }, // BRS
    { 8000,      3,   0,                 40, { SEG_P, SEG_N, SEG_O, SEG_BLANK } }, // PNO
    {   80,    120,   ENV_ONE * 3 / 4,   18, { SEG_S, SEG_A, SEG_X, SEG_BLANK } }, // SAX
    {   40,     80,   ENV_ONE * 3 / 4,   12, { SEG_V, SEG_L, SEG_N, SEG_BLANK } }, // VLN
    { 8192,     50,   0,                200, { SEG_D, SEG_R, SEG_M, SEG_BLANK } }, // DRM
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
    volatile uint8_t  is_noise;   // 1 = noise source (hi-hat / crash)
    volatile uint16_t gain;       // 256 = unity
};

static Voice voices[NUM_VOICES];
static volatile uint32_t voice_age_counter = 0;
static volatile uint32_t noise_lfsr = 0xACE1u;  // 16-bit LFSR seed
static volatile int32_t  noise_lpf  = 0;         // one-pole low-pass state

// Audio producer/consumer buffer. The main loop renders audio; the I2S ISR
// only moves already-rendered samples into the I2S DATA register.
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
    /* ~±SINE_PEAK/4 */
    int32_t n = (int32_t)(((int32_t)l - 32768) * (SINE_PEAK / 131072));
    /* simple one-pole: y += (x - y) / 4  — softens harsh digital noise */
    noise_lpf += (n - noise_lpf) >> 2;
    return noise_lpf;
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
static int32_t organ_sub_table[SINE_LEN];
static int32_t organ_5th3_table[SINE_LEN];

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

        // Store the two non-integer drawbars already scaled to the same
        // output range. This removes all large integer multiplies/divides
        // from the real-time organ path.
        organ_sub_table[i] = (int32_t)(sin(0.5 * a) * (96.0 / 744.0) * (double)SINE_PEAK);
        organ_5th3_table[i] = (int32_t)(sin(1.5 * a) * (150.0 / 744.0) * (double)SINE_PEAK);
    }
}

static inline int32_t instrument_lookup(const int32_t *table, uint32_t ph)
{
    return table[(ph >> 22) & (SINE_LEN - 1)];
}

static inline int32_t organ_lookup(uint32_t ph)
{
    // All Hammond drawbars are precomputed. The runtime path is only
    // three table reads and two additions, which is comfortably inside
    // the SAMD21 audio producer budget.
    const uint32_t idx = (ph >> 22) & (SINE_LEN - 1);
    return organ_harmonic_table[idx]
         + organ_sub_table[idx]
         + organ_5th3_table[idx];
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
        voices[i].gain      = 256;
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
    voices[v].age       = ++voice_age_counter;

    if (preset_idx == PRESET_DRUM) {
        /* Hi-hat (2) and Crash (7) use noise; others use sine body */
        voices[v].is_noise  = (note == 2 || note == 7) ? 1 : 0;
        voices[v].phase_inc = hz_to_inc(drum_hz[note]);
        voices[v].gain      = drum_gain[note];
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
            el -= adsr->decay;
            if (el <= adsr->sustain) {
                el = adsr->sustain;
                if (adsr->sustain > 0)
                    v->env_stage = ENV_SUSTAIN;
                else {
                    v->env_stage = ENV_IDLE;
                    v->phase_inc = 0;
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
        if (el == 0)
            continue;

        int32_t raw;
        if (v->is_noise) {
            int32_t sine = sine_lookup(v->phase);
            int32_t n = next_noise() >> 4;
            raw = (sine >> 1) + n;
        } else {
            if (preset_idx == 0)
                raw = organ_lookup(v->phase);
            else if (preset_idx == PRESET_SAX)
                raw = instrument_lookup(sax_table, v->phase);
            else if (preset_idx == PRESET_VLN)
                raw = instrument_lookup(vln_table, v->phase);
            else
                raw = sine_lookup(v->phase);
        }

        /* Preserve the known-good Rev2 fixed-point scaling exactly. The
         * synthesis now runs outside the I2S ISR, so the M0+ 64-bit math
         * no longer consumes the real-time interrupt budget. */
        int64_t scaled = ((int64_t)raw * el) >> 16;
        scaled = (scaled * v->gain) >> 8;
        mix += scaled;
        v->phase += v->phase_inc;
    }

    // Keep output gain independent of current polyphony. Dividing by the
    // number of active voices makes a remaining note jump in level when
    // another note is released, which sounds like a ghost note.
    if (preset_idx == PRESET_DRUM)
        mix = (mix * 5) / 4;
    else
        mix /= NUM_VOICES;

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
    for (int i = 0; i < 40; i++) {
        if (encoder_poll())
            changed = true;

        audio_buffer_fill(32);
    }

    tm_poll();

    if (changed)
        tm_show_ui();
}
