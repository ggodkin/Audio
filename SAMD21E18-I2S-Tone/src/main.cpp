/*
 * ATSAMD21E18A → MAX98357A
 * Continuous 440 Hz sine over I2S
 *
 * PA08 = DIN (SD1), PA10 = BCLK (SCK0), PA11 = LRCLK (FS0), PA17 = LED
 *
 * Bug fixed: SEREN1 is bit 5 of CTRLA (was wrongly written as bit 6).
 * That left the serializer disabled → TXRDY1 never asserted → no clocks.
 */

#include <Arduino.h>
#include "sam.h"

static constexpr uint32_t TONE_HZ        = 440;
static constexpr uint32_t SAMPLE_RATE_HZ = 46875;
static constexpr uint32_t SINE_LEN       = 64;

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

static uint32_t phase = 0;
static uint32_t phase_inc = 0;

// ---------------------------------------------------------------------------
// LED (PA17, active high)
// ---------------------------------------------------------------------------
static void led_init(void)
{
    PORT->Group[0].DIRSET.reg = PORT_PA17;
    PORT->Group[0].OUTCLR.reg = PORT_PA17;
}

static void led_on(void)     { PORT->Group[0].OUTSET.reg = PORT_PA17; }
static void led_off(void)    { PORT->Group[0].OUTCLR.reg = PORT_PA17; }
static void led_toggle(void) { PORT->Group[0].OUTTGL.reg = PORT_PA17; }

static void led_blink_n(int n, uint16_t on_ms = 100, uint16_t off_ms = 100)
{
    for (int i = 0; i < n; i++) {
        led_on();
        delay(on_ms);
        led_off();
        delay(off_ms);
    }
    delay(250);
}

static void wait_gclk(void)
{
    for (uint32_t i = 0; i < 100000u && GCLK->STATUS.bit.SYNCBUSY; i++) {}
}

static void wait_i2s(uint32_t mask)
{
    for (uint32_t i = 0; i < 100000u && (I2S->SYNCBUSY.reg & mask); i++) {}
}

// ---------------------------------------------------------------------------
static void configure_i2s(void)
{
    // 1. APB clock
    PM->APBCMASK.reg |= PM_APBCMASK_I2S;
    led_blink_n(1);

    // 2. GCLK0 (48 MHz) → I2S (GCLK ID 0x18)
    GCLK->CLKCTRL.reg =
        GCLK_CLKCTRL_ID(0x18) |
        GCLK_CLKCTRL_GEN_GCLK0 |
        GCLK_CLKCTRL_CLKEN;
    wait_gclk();
    led_blink_n(2);

    // 3. Pinmux function G (6)
    // PA08 = SD1
    PORT->Group[0].PINCFG[8].reg  |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[4].bit.PMUXE = 0x6;

    // PA10 = SCK0
    PORT->Group[0].PINCFG[10].reg |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[5].bit.PMUXE = 0x6;

    // PA11 = FS0
    PORT->Group[0].PINCFG[11].reg |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[5].bit.PMUXO = 0x6;
    led_blink_n(3);

    // 4. Software reset
    I2S->CTRLA.reg = I2S_CTRLA_SWRST;
    wait_i2s(I2S_SYNCBUSY_SWRST);
    led_blink_n(4);

    // 5. Clock unit 0 + Serializer 1
    // 32-bit slots, 2 slots, I2S format (BITDELAY),
    // FS from SCK, SCK from MCK, MCK = GCLK, MCKDIV=15 → 3 MHz BCLK
    I2S->CLKCTRL[0].reg =
        I2S_CLKCTRL_SLOTSIZE(3) |      // 32-bit
        I2S_CLKCTRL_NBSLOTS(1) |       // 2 slots
        I2S_CLKCTRL_BITDELAY |
        I2S_CLKCTRL_FSSEL_SCKDIV |
        I2S_CLKCTRL_SCKSEL_MCKDIV |
        I2S_CLKCTRL_MCKSEL_GCLK |
        I2S_CLKCTRL_MCKDIV(15);

    // Serializer 1 = TX, left-aligned, 32-bit, clock unit 0
    I2S->SERCTRL[1].reg =
        I2S_SERCTRL_SERMODE_TX |
        I2S_SERCTRL_TXSAME |
        I2S_SERCTRL_SLOTADJ_LEFT |
        I2S_SERCTRL_DATASIZE_32 |
        I2S_SERCTRL_CLKSEL_CLK0;

    led_blink_n(5);

    // 6. Enable – SEREN1 is bit 5 (NOT bit 6)
    I2S->CTRLA.reg =
        I2S_CTRLA_ENABLE |
        I2S_CTRLA_CKEN0  |
        I2S_CTRLA_SEREN1;              // bit 5

    wait_i2s(I2S_SYNCBUSY_ENABLE |
             I2S_SYNCBUSY_CKEN0  |
             I2S_SYNCBUSY_SEREN1);

    led_blink_n(6);
}

static bool i2s_write_stereo(int32_t sample)
{
    uint32_t t;

    t = 0;
    while (!(I2S->INTFLAG.bit.TXRDY1)) {
        if (++t > 100000u) return false;
    }
    I2S->DATA[1].reg = (uint32_t)sample;

    t = 0;
    while (!(I2S->INTFLAG.bit.TXRDY1)) {
        if (++t > 100000u) return false;
    }
    I2S->DATA[1].reg = (uint32_t)sample;

    return true;
}

// ---------------------------------------------------------------------------
void setup()
{
    led_init();

    // 3 fast blinks = reached setup()
    led_blink_n(3, 50, 50);

    phase_inc = (uint32_t)(((uint64_t)TONE_HZ << 32) / SAMPLE_RATE_HZ);

    configure_i2s();

    led_on();
    delay(400);
}

void loop()
{
    static uint32_t sample_count = 0;
    static bool i2s_ok = true;

    uint32_t idx = phase >> 26;
    int32_t sample = sine_table[idx & (SINE_LEN - 1)];

    if (!i2s_write_stereo(sample)) {
        i2s_ok = false;
        led_blink_n(10, 40, 40);   // SOS – TX never ready
    }

    phase += phase_inc;

    if (i2s_ok && ++sample_count >= (SAMPLE_RATE_HZ / 2)) {
        sample_count = 0;
        led_toggle();              // ~1 Hz heartbeat when audio is flowing
    }
}
