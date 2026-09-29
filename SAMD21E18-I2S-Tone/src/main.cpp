/*
 * ATSAMD21E18A → MAX98357A – 4-voice + ADSR presets
 *
 * I2S:     PA08=DIN  PA10=BCLK  PA11=LRCLK
 * LED:     PA17
 * Encoder: PA14/PA15 = A/B   PA22 = push (mode toggle)
 * TM1638:  PA16=STB  PA18=CLK  PA19=DIO
 *
 * Display left 4: VOL / ORG / PLK / PAD / BRS / PNO / DRM
 * Display right 4: volume 0–64 or preset index 1–6
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

static constexpr uint32_t SAMPLE_RATE_HZ = 31815;
static constexpr uint32_t SINE_LEN       = 1024;
static constexpr int      VOLUME_MAX     = 64;
static constexpr int      VOLUME_STEP    = 1;
static constexpr int      NUM_VOICES     = 4;
static constexpr int      NUM_PRESETS    = 6;
static constexpr int      PRESET_EDGES   = 4;  // quadrature edges per detent
static constexpr int      PRESET_DRUM    = 5;  // index of drum mode

static constexpr int32_t SINE_PEAK = 280000000;
static constexpr int32_t ENV_ONE   = 65536;

static int32_t sine_table[SINE_LEN];

static const uint16_t button_hz[8] = {
    523, 587, 659, 698, 784, 880, 988, 1047
};

/* Drum pitch map (Hz) – approximate classic kit tones */
static const uint16_t drum_hz[8] = {
    60,   // Kick
    180,  // Snare body
    8000, // Hi-hat (will be noise-dominated)
    220,  // Clap
    120,  // Tom low
    180,  // Tom mid
    400,  // Rim
    6000  // Crash (noise + tone)
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

enum EnvStage : uint8_t { ENV_IDLE = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

struct Adsr {
    int32_t attack;
    int32_t decay;
    int32_t sustain;
    int32_t release;
    uint8_t label[4];
};

/*
 * Rates per sample at ~31.8 kHz. Time ≈ ENV_ONE / rate / SAMPLE_RATE seconds.
 *
 * ORG – instant on, full sustain, ~0.3 s release  (continuous organ)
 * PLK – instant on, ~15 ms decay to silence       (short pluck; no sustain)
 * PAD – ~1.0 s attack, full sustain, ~2 s release (slow swell)
 * BRS – ~0.2 s attack, decay to 50%, medium release
 * PNO – fast attack, medium decay to low sustain, long release (piano-like)
 * DRM – instant on, very fast decay, no sustain   (percussive drum hits)
 */
static const Adsr PRESETS[NUM_PRESETS] = {
    // attack  decay  sustain           release
    { 4096,    256,   ENV_ONE,          8,   { SEG_O, SEG_R, SEG_G, SEG_BLANK } }, // ORG
    { 8192,    150,   0,                200, { SEG_P, SEG_L, SEG_K, SEG_BLANK } }, // PLK
    {    2,     16,   ENV_ONE,          1,   { SEG_P, SEG_A, SEG_D, SEG_BLANK } }, // PAD
    {   12,     40,   ENV_ONE / 2,      20,  { SEG_B, SEG_R, SEG_S, SEG_BLANK } }, // BRS
    { 6000,     25,   ENV_ONE / 8,       6,  { SEG_P, SEG_N, SEG_O, SEG_BLANK } }, // PNO
    { 8192,    400,   0,                800, { SEG_D, SEG_R, SEG_M, SEG_BLANK } }, // DRM
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
};

static Voice voices[NUM_VOICES];
static volatile uint32_t voice_age_counter = 0;
static volatile uint32_t noise_lfsr = 0xACE1u;  // 16-bit LFSR seed

static volatile uint8_t i2s_slot = 0;
static volatile int32_t i2s_hold = 0;

/* Simple 16-bit LFSR white noise */
static inline int32_t next_noise(void)
{
    uint32_t l = noise_lfsr;
    uint32_t bit = ((l >> 0) ^ (l >> 2) ^ (l >> 3) ^ (l >> 5)) & 1u;
    l = (l >> 1) | (bit << 15);
    noise_lfsr = l;
    /* Map 0..65535 → approx ±SINE_PEAK/2 */
    return (int32_t)(((int32_t)l - 32768) * (SINE_PEAK / 32768));
}

static void sine_table_init(void)
{
    for (uint32_t i = 0; i < SINE_LEN; i++) {
        float a = (2.0f * 3.14159265f * (float)i) / (float)SINE_LEN;
        sine_table[i] = (int32_t)(sinf(a) * (float)SINE_PEAK);
    }
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
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].note < 0 && voices[i].env_stage == ENV_IDLE)
            return i;
    }
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].note < 0)
            return i;
    }
    int oldest = 0;
    uint32_t best = voices[0].age;
    for (int i = 1; i < NUM_VOICES; i++) {
        if (voices[i].age < best) {
            best = voices[i].age;
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
    voices[v].env_level = 0;
    voices[v].env_stage = ENV_ATTACK;
    voices[v].age       = ++voice_age_counter;

    if (preset_idx == PRESET_DRUM) {
        /* Hi-hat (2) and Crash (7) use noise; others use sine body */
        voices[v].is_noise  = (note == 2 || note == 7) ? 1 : 0;
        voices[v].phase_inc = hz_to_inc(drum_hz[note]);
        /* Slight pitch drop for kick/toms feels more natural – handled by short env */
    } else {
        voices[v].is_noise  = 0;
        voices[v].phase_inc = hz_to_inc(button_hz[note]);
    }
}

static void note_off(int note)
{
    int v = find_voice_for_note(note);
    if (v < 0) return;
    voices[v].env_stage = ENV_RELEASE;
    voices[v].note      = -1;
}

extern "C" void I2S_Handler(void)
{
    if (!(I2S->INTFLAG.bit.TXRDY1))
        return;

    if (i2s_slot == 0) {
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
                        el = 0;
                    }
                }
                break;
            case ENV_SUSTAIN:
                el = adsr->sustain;
                break;
            case ENV_RELEASE:
                el -= adsr->release;
                if (el <= 0) {
                    el = 0;
                    v->env_stage = ENV_IDLE;
                    v->phase_inc = 0;
                }
                break;
            default:
                el = 0;
                break;
            }
            v->env_level = el;

            if (el == 0)
                continue;

            int32_t raw;
            if (v->is_noise) {
                /* Mix noise with a little sine body for metallic character */
                int32_t n = next_noise();
                int32_t s = sine_lookup(v->phase);
                raw = (n >> 1) + (s >> 2);   // mostly noise
            } else {
                raw = sine_lookup(v->phase);
            }
            mix += ((int64_t)raw * el) >> 16;
            v->phase += v->phase_inc;
        }

        mix /= NUM_VOICES;
        mix = (mix * volume) / VOLUME_MAX;

        if (mix >  2147483647LL) mix =  2147483647LL;
        if (mix < -2147483648LL) mix = -2147483648LL;

        i2s_hold = (int32_t)mix;
        I2S->DATA[1].reg = (uint32_t)(int32_t)mix;
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
    static uint8_t prev = 0;
    uint8_t keys = keys_physical(tm_read_keys_raw());
    uint8_t pressed  = keys & ~prev;
    uint8_t released = prev & ~keys;

    for (int i = 0; i < 8; i++) {
        if (pressed & (1u << i))
            note_on(i);
        if (released & (1u << i))
            note_off(i);
    }

    tm_set_leds(keys);
    prev = keys;
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
        I2S_CLKCTRL_MCKDIV(15);

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
    voices_init();
    encoder_init();
    tm_init();

    configure_i2s();

    led_on();
    delay(100);
}

void loop()
{
    bool changed = false;

    for (int i = 0; i < 40; i++) {
        if (encoder_poll())
            changed = true;
        delayMicroseconds(200);
    }

    tm_poll();

    if (changed)
        tm_show_ui();
}
