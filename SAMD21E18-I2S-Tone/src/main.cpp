/*
 * ATSAMD21E18A → MAX98357A
 * I2S sine + rotary volume + TM1638 LED&KEY (8 buttons)
 *
 * I2S:     PA08=DIN  PA10=BCLK  PA11=LRCLK
 * LED:     PA17 heartbeat
 * Encoder: PA14=A  PA15=B  (volume 0..64, start 1/32)
 *
 * TM1638 LED&KEY module (3-wire):
 *   PA16 = STB
 *   PA18 = CLK
 *   PA19 = DIO
 *
 * Buttons S1…S8 → C4 D4 E4 F4 G4 A4 B4 C5
 */

#include <Arduino.h>
#include "sam.h"

/*
 * Clocking (GCLK0 = 48 MHz):
 *   MCKDIV = 3  → BCLK = 48 MHz / 4 = 12 MHz
 *   2 × 32-bit slots → LRCLK = 12 MHz / 64 = 187500 Hz  … too high for MAX98357A
 *
 * Prefer audio-range LRCLK:
 *   MCKDIV = 15 → BCLK = 3 MHz → LRCLK = 46875 Hz
 *
 * Pitch was 2 octaves low with phase_inc using 46875 while the effective
 * rate heard was ~1/4 of that. Using the nominal 46875 rate with a ×4
 * correction in set_freq_hz restores concert pitch. If your scope shows
 * a different LRCLK, change PITCH_CORR or SAMPLE_RATE_HZ to match.
 */
static constexpr uint32_t SAMPLE_RATE_HZ = 46875;
static constexpr uint32_t PITCH_CORR     = 4;      // was 2 octaves low
static constexpr uint32_t SINE_LEN       = 64;
static constexpr int      VOLUME_MAX     = 64;

static const int32_t sine_table[SINE_LEN] = {
           0,  105245103,  209476638,  311690799,
   410903206,  506158392,  596538995,  681174601,
   759250124,  830013653,  892783697,  946955746,
   992008093, 1027506861, 1053110175, 1068571463,
  1073741823, 1068571463, 1053110175, 1027506861,
   992008093,  946955746,  892783697,  830013653,
   759250124,  681174601,  596538995,  506158392,
   410903206,  311690799,  209476638,  105245103,
           0, -105245103, -209476638, -311690799,
  -410903206, -506158392, -596538995, -681174601,
  -759250124, -830013653, -892783697, -946955746,
  -992008093,-1027506861,-1053110175,-1068571463,
 -1073741823,-1068571463,-1053110175,-1027506861,
  -992008093, -946955746, -892783697, -830013653,
  -759250124, -681174601, -596538995, -506158392,
  -410903206, -311690799, -209476638, -105245103
};

// C4 … C5 for buttons 0…7
static const uint16_t button_hz[8] = {
    262, 294, 330, 349, 392, 440, 494, 523
};

static uint32_t phase     = 0;
static uint32_t phase_inc = 0;

static volatile int  volume  = VOLUME_MAX / 32;
static volatile bool gate_on = false;
static int           active_btn = -1;

static void set_freq_hz(uint16_t hz)
{
    if (hz == 0) {
        phase_inc = 0;
        return;
    }
    // phase_inc = freq / sample_rate * 2^32, with empirical pitch correction
    phase_inc = (uint32_t)(((uint64_t)hz * PITCH_CORR << 32) / SAMPLE_RATE_HZ);
}

// ---------------------------------------------------------------------------
// Heartbeat LED PA17
// ---------------------------------------------------------------------------
static void led_init(void)
{
    PORT->Group[0].DIRSET.reg = PORT_PA17;
    PORT->Group[0].OUTCLR.reg = PORT_PA17;
}
static void led_on(void)     { PORT->Group[0].OUTSET.reg = PORT_PA17; }
static void led_off(void)    { PORT->Group[0].OUTCLR.reg = PORT_PA17; }
static void led_toggle(void) { PORT->Group[0].OUTTGL.reg = PORT_PA17; }

static void led_blink_n(int n, uint16_t on_ms = 80, uint16_t off_ms = 80)
{
    for (int i = 0; i < n; i++) {
        led_on();  delay(on_ms);
        led_off(); delay(off_ms);
    }
    delay(150);
}

// ---------------------------------------------------------------------------
// Rotary encoder PA14 / PA15
// ---------------------------------------------------------------------------
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

static void encoder_poll(void)
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
        int v = volume + delta;
        if (v < 0)          v = 0;
        if (v > VOLUME_MAX) v = VOLUME_MAX;
        volume = v;
    }
}

// ---------------------------------------------------------------------------
// TM1638 LED&KEY – PA16=STB, PA18=CLK, PA19=DIO
// ---------------------------------------------------------------------------
static void tm_delay(void)
{
    for (volatile int i = 0; i < 20; i++) {}
}

static void tm_stb_low(void)  { PORT->Group[0].OUTCLR.reg = PORT_PA16; }
static void tm_stb_high(void) { PORT->Group[0].OUTSET.reg = PORT_PA16; }
static void tm_clk_low(void)  { PORT->Group[0].OUTCLR.reg = PORT_PA18; }
static void tm_clk_high(void) { PORT->Group[0].OUTSET.reg = PORT_PA18; }

static void tm_dio_out(void)
{
    PORT->Group[0].DIRSET.reg = PORT_PA19;
}

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
}

static void tm_set_leds(uint8_t mask)
{
    for (int i = 0; i < 8; i++) {
        tm_stb_low();
        tm_write_byte(0xC1 + (i * 2));
        tm_write_byte((mask & (1u << i)) ? 0x01 : 0x00);
        tm_stb_high();
    }
}

static uint8_t tm_read_keys(void)
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

static void tm_poll(void)
{
    static uint8_t prev = 0;
    uint8_t keys = tm_read_keys();

    uint8_t pressed  = keys & ~prev;
    uint8_t released = prev & ~keys;

    if (pressed) {
        for (int i = 0; i < 8; i++) {
            if (pressed & (1u << i)) {
                active_btn = i;
                gate_on = true;
                set_freq_hz(button_hz[i]);
                break;
            }
        }
    }

    if (released && active_btn >= 0 && (released & (1u << active_btn))) {
        if (keys) {
            for (int i = 0; i < 8; i++) {
                if (keys & (1u << i)) {
                    active_btn = i;
                    set_freq_hz(button_hz[i]);
                    break;
                }
            }
        } else {
            active_btn = -1;
            gate_on = false;
            set_freq_hz(0);
        }
    }

    tm_set_leds(keys);
    prev = keys;
}

// ---------------------------------------------------------------------------
// I2S
// ---------------------------------------------------------------------------
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

    // MCKDIV=15 → 48 MHz/16 = 3 MHz BCLK → 46.875 kHz LRCLK
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
        I2S_SERCTRL_TXSAME |
        I2S_SERCTRL_SLOTADJ_LEFT |
        I2S_SERCTRL_DATASIZE_32 |
        I2S_SERCTRL_CLKSEL_CLK0;

    I2S->CTRLA.reg =
        I2S_CTRLA_ENABLE |
        I2S_CTRLA_CKEN0  |
        I2S_CTRLA_SEREN1;

    wait_i2s(I2S_SYNCBUSY_ENABLE |
             I2S_SYNCBUSY_CKEN0  |
             I2S_SYNCBUSY_SEREN1);
}

static bool i2s_write_stereo(int32_t sample)
{
    uint32_t t;

    t = 0;
    while (!(I2S->INTFLAG.bit.TXRDY1)) {
        if (++t > 200000u) return false;
    }
    I2S->DATA[1].reg = (uint32_t)sample;

    t = 0;
    while (!(I2S->INTFLAG.bit.TXRDY1)) {
        if (++t > 200000u) return false;
    }
    I2S->DATA[1].reg = (uint32_t)sample;

    return true;
}

// ---------------------------------------------------------------------------
void setup()
{
    led_init();
    led_blink_n(3, 50, 50);

    encoder_init();
    tm_init();

    phase_inc = 0;
    configure_i2s();

    led_on();
    delay(100);
}

void loop()
{
    static uint32_t sample_count = 0;
    static uint32_t poll_div = 0;

    encoder_poll();

    if (++poll_div >= 48) {
        poll_div = 0;
        tm_poll();
    }

    uint32_t idx = phase >> 26;
    int32_t raw = sine_table[idx & (SINE_LEN - 1)];

    int32_t sample = 0;
    if (gate_on)
        sample = (int32_t)(((int64_t)raw * volume) / VOLUME_MAX);

    i2s_write_stereo(sample);
    phase += phase_inc;

    if (++sample_count >= (SAMPLE_RATE_HZ / 2)) {
        sample_count = 0;
        led_toggle();
    }
}
