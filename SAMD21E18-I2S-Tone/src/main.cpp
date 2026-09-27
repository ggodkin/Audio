/*
 * ATSAMD21E18A → MAX98357A – 440 Hz I2S tone + rotary volume
 *
 * I2S:
 *   PA08 = DIN (SD1)
 *   PA10 = BCLK (SCK0)
 *   PA11 = LRCLK (FS0)
 *
 * LED:
 *   PA17 = heartbeat
 *
 * Rotary encoder (quadrature):
 *   PA14 = A  (CLK)
 *   PA15 = B  (DT)
 *   Common → GND
 *   (internal pull-ups enabled; no external resistors needed)
 *
 * Turn encoder → volume 0…64 (0 = mute, 64 = full scale)
 * Starts at 1/32 of max (volume = 2)
 */

#include <Arduino.h>
#include "sam.h"

static constexpr uint32_t TONE_HZ        = 440;
static constexpr uint32_t SAMPLE_RATE_HZ = 46875;
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

static uint32_t phase     = 0;
static uint32_t phase_inc = 0;

// Volume: 0 = mute … VOLUME_MAX = full
// Start at 1/32 of max
static volatile int volume = VOLUME_MAX / 32;   // = 2

// ---------------------------------------------------------------------------
// LED (PA17)
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
    delay(200);
}

// ---------------------------------------------------------------------------
// Rotary encoder on PA14 (A) / PA15 (B) – polled quadrature
// ---------------------------------------------------------------------------
static uint8_t enc_prev = 0;

static void encoder_init(void)
{
    // Inputs with pull-ups
    PORT->Group[0].DIRCLR.reg = PORT_PA14 | PORT_PA15;
    PORT->Group[0].PINCFG[14].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].PINCFG[15].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].OUTSET.reg = PORT_PA14 | PORT_PA15;   // pull-up

    // Initial state
    uint32_t in = PORT->Group[0].IN.reg;
    enc_prev = ((in & PORT_PA14) ? 1 : 0) | ((in & PORT_PA15) ? 2 : 0);
}

// Call often from the sample loop. Gray-code table for one detent step.
static void encoder_poll(void)
{
    static const int8_t table[16] = {
    // prev<<2 | curr
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
        if (v < 0)            v = 0;
        if (v > VOLUME_MAX)   v = VOLUME_MAX;
        volume = v;
    }
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

    phase_inc = (uint32_t)(((uint64_t)TONE_HZ << 32) / SAMPLE_RATE_HZ);

    configure_i2s();

    led_on();
    delay(200);
}

void loop()
{
    static uint32_t sample_count = 0;

    // Poll encoder every sample (cheap) so detents are not missed
    encoder_poll();

    uint32_t idx = phase >> 26;
    int32_t raw = sine_table[idx & (SINE_LEN - 1)];

    // Scale by volume (0…VOLUME_MAX)
    int32_t sample = (int32_t)(((int64_t)raw * volume) / VOLUME_MAX);

    i2s_write_stereo(sample);

    phase += phase_inc;

    if (++sample_count >= (SAMPLE_RATE_HZ / 2)) {
        sample_count = 0;
        led_toggle();
    }
}
