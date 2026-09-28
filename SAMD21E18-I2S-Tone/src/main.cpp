/*
 * ATSAMD21E18A → MAX98357A – 4-voice polyphonic sine
 *
 * I2S:     PA08=DIN  PA10=BCLK  PA11=LRCLK
 * LED:     PA17
 * Encoder: PA14/PA15 volume
 * TM1638:  PA16=STB  PA18=CLK  PA19=DIO
 * Display: "VOL " + level 0–64
 *
 * S1…S8 → C5 D5 E5 F5 G5 A5 B5 C6
 * Up to 4 keys at once; extra notes steal the oldest voice.
 */

#include <Arduino.h>
#include "sam.h"
#include <math.h>

static constexpr uint32_t SAMPLE_RATE_HZ = 46875;
static constexpr uint32_t SINE_LEN       = 1024;
static constexpr int      VOLUME_MAX     = 64;
static constexpr int      VOLUME_STEP    = 1;
static constexpr int      NUM_VOICES     = 4;

static constexpr int32_t SINE_PEAK = 280000000;  // headroom for 4-voice mix

static constexpr int32_t ENV_ATTACK  = 256;
static constexpr int32_t ENV_RELEASE = 128;
static constexpr int32_t ENV_ONE     = 65536;

static int32_t sine_table[SINE_LEN];

static const uint16_t button_hz[8] = {
    523, 587, 659, 698, 784, 880, 988, 1047
};

static const uint8_t phys_bit[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

static const uint8_t SEG_DIGIT[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};
static constexpr uint8_t SEG_V     = 0x3E;
static constexpr uint8_t SEG_O     = 0x3F;
static constexpr uint8_t SEG_L     = 0x38;
static constexpr uint8_t SEG_BLANK = 0x00;

struct Voice {
    volatile uint32_t phase;
    volatile uint32_t phase_inc;
    volatile int32_t  env_level;
    volatile int32_t  env_target;
    volatile int8_t   note;      // 0…7 button index, -1 = free
    volatile uint32_t age;       // for voice stealing
};

static Voice voices[NUM_VOICES];
static volatile uint32_t voice_age_counter = 0;

static volatile int volume = VOLUME_MAX / 4;

static volatile uint8_t i2s_slot = 0;
static volatile int32_t i2s_hold = 0;

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
        voices[i].phase      = 0;
        voices[i].phase_inc  = 0;
        voices[i].env_level  = 0;
        voices[i].env_target = 0;
        voices[i].note       = -1;
        voices[i].age        = 0;
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
    // Prefer free (fully released) voice
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].note < 0 && voices[i].env_level == 0)
            return i;
    }
    // Prefer any free note slot (still releasing)
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].note < 0)
            return i;
    }
    // Steal oldest
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

    // Already playing this note – re-attack
    int v = find_voice_for_note(note);
    if (v < 0)
        v = alloc_voice();

    voices[v].note       = (int8_t)note;
    voices[v].phase_inc  = hz_to_inc(button_hz[note]);
    voices[v].env_target = ENV_ONE;
    voices[v].age        = ++voice_age_counter;
    // keep phase continuous on re-attack for less click
}

static void note_off(int note)
{
    int v = find_voice_for_note(note);
    if (v < 0) return;
    voices[v].env_target = 0;
    voices[v].note       = -1;  // free for reuse after release finishes
}

// ---------------------------------------------------------------------------
// I2S ISR – mix up to 4 voices
// ---------------------------------------------------------------------------
extern "C" void I2S_Handler(void)
{
    if (!(I2S->INTFLAG.bit.TXRDY1))
        return;

    if (i2s_slot == 0) {
        int64_t mix = 0;

        for (int i = 0; i < NUM_VOICES; i++) {
            Voice *v = &voices[i];

            int32_t el = v->env_level;
            int32_t et = v->env_target;
            if (el < et) {
                el += ENV_ATTACK;
                if (el > et) el = et;
            } else if (el > et) {
                el -= ENV_RELEASE;
                if (el < et) el = et;
            }
            v->env_level = el;

            if (el == 0) {
                v->phase_inc = 0;
                continue;
            }

            int32_t raw = sine_lookup(v->phase);
            mix += ((int64_t)raw * el) >> 16;
            v->phase += v->phase_inc;
        }

        // Average voices so 4 keys ≈ same peak as 1
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

// ---------------------------------------------------------------------------
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

static void encoder_init(void)
{
    PORT->Group[0].DIRCLR.reg = PORT_PA14 | PORT_PA15;
    PORT->Group[0].PINCFG[14].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].PINCFG[15].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].OUTSET.reg = PORT_PA14 | PORT_PA15;

    uint32_t in = PORT->Group[0].IN.reg;
    enc_prev = ((in & PORT_PA14) ? 1 : 0) | ((in & PORT_PA15) ? 2 : 0);
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

    if (delta) {
        int v = volume + delta * VOLUME_STEP;
        if (v < 0)          v = 0;
        if (v > VOLUME_MAX) v = VOLUME_MAX;
        volume = v;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
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

static void tm_show_volume(int vol)
{
    tm_write_digit(0, SEG_V);
    tm_write_digit(1, SEG_O);
    tm_write_digit(2, SEG_L);
    tm_write_digit(3, SEG_BLANK);

    if (vol < 0) vol = 0;
    if (vol > 9999) vol = 9999;

    tm_write_digit(4, (vol >= 1000) ? SEG_DIGIT[(vol / 1000) % 10] : SEG_BLANK);
    tm_write_digit(5, (vol >= 100)  ? SEG_DIGIT[(vol / 100) % 10]  : SEG_BLANK);
    tm_write_digit(6, (vol >= 10)   ? SEG_DIGIT[(vol / 10) % 10]   : SEG_BLANK);
    tm_write_digit(7, SEG_DIGIT[vol % 10]);
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

    tm_show_volume(volume);
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
    static int last_vol = -1;
    bool changed = false;

    for (int i = 0; i < 40; i++) {
        if (encoder_poll())
            changed = true;
        delayMicroseconds(200);
    }

    tm_poll();

    int v = volume;
    if (changed || v != last_vol) {
        tm_show_volume(v);
        last_vol = v;
    }
}
