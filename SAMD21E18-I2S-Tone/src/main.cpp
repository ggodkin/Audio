/*
 * ATSAMD21E18A → MAX98357A
 * Continuous 440 Hz sine wave over I2S (stereo, 32-bit slots)
 *
 * Wiring (bare chip):
 *   PA08  →  DIN  (I2S SD1)
 *   PA10  →  BCLK (I2S SCK0)
 *   PA11  →  LRC  (I2S FS0)
 *   PA17  →  LED  (heartbeat, active high)
 *   GND   →  GND
 *   MCLK left unconnected
 *
 * Sample rate ≈ 46.875 kHz (48 MHz / 16 / 64)
 */

#include <Arduino.h>
#include "sam.h"

// ----------------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------------
static constexpr uint32_t TONE_HZ         = 440;
static constexpr uint32_t SAMPLE_RATE_HZ  = 46875;   // 48 MHz / 16 / 64
static constexpr uint32_t SINE_TABLE_SIZE = 64;

// ----------------------------------------------------------------------------
// 64-entry sine table, full-scale ~2^30
// ----------------------------------------------------------------------------
static const int32_t sine_table[SINE_TABLE_SIZE] = {
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

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
static void wait_gclk_sync(void)
{
    while (GCLK->STATUS.bit.SYNCBUSY) {}
}

static void wait_i2s_sync(uint32_t mask)
{
    while (I2S->SYNCBUSY.reg & mask) {}
}

// ----------------------------------------------------------------------------
// Heartbeat LED on PA17 (active high)
// ----------------------------------------------------------------------------
static void led_init(void)
{
    PORT->Group[0].DIRSET.reg = PORT_PA17;
    PORT->Group[0].OUTCLR.reg = PORT_PA17;
}

static void led_toggle(void)
{
    PORT->Group[0].OUTTGL.reg = PORT_PA17;
}

// ----------------------------------------------------------------------------
// Pinmux – PA08=SD1, PA10=SCK0, PA11=FS0  (function G = 6)
// ----------------------------------------------------------------------------
static void configure_i2s_pins(void)
{
    // PA08 – I2S/SD1 (even → PMUXE)
    PORT->Group[0].PINCFG[8].reg |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[4].bit.PMUXE = 0x6;          // PMUX[8/2]

    // PA10 – I2S/SCK0 (even → PMUXE)
    PORT->Group[0].PINCFG[10].reg |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[5].bit.PMUXE = 0x6;         // PMUX[10/2]

    // PA11 – I2S/FS0 (odd → PMUXO)
    PORT->Group[0].PINCFG[11].reg |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[5].bit.PMUXO = 0x6;         // same PMUX register
}

// ----------------------------------------------------------------------------
// Clock: use GCLK0 (48 MHz DFLL already running under Arduino) → I2S
// ----------------------------------------------------------------------------
static void configure_i2s_clock(void)
{
    // I2S GCLK ID on SAMD21 is 0x18 (I2S_GCLK_ID_0)
    GCLK->CLKCTRL.reg =
        GCLK_CLKCTRL_ID(0x18) |
        GCLK_CLKCTRL_GEN_GCLK0 |
        GCLK_CLKCTRL_CLKEN;
    wait_gclk_sync();
}

// ----------------------------------------------------------------------------
// I2S peripheral – master, 32-bit, stereo, I2S format, serializer 1 (PA08)
// ----------------------------------------------------------------------------
static void configure_i2s(void)
{
    // Enable APB clock for I2S
    PM->APBCMASK.reg |= PM_APBCMASK_I2S;

    configure_i2s_clock();
    configure_i2s_pins();

    // Software reset
    I2S->CTRLA.reg = I2S_CTRLA_SWRST;
    wait_i2s_sync(I2S_SYNCBUSY_SWRST);

    /*
     * Clock unit 0:
     *   32-bit slots, 2 slots (stereo)
     *   BITDELAY = 1 → classic I2S (1-bit delay after FS edge)
     *   FS derived from SCK, SCK derived from MCK, MCK = GCLK
     *   MCKDIV = 15 → 48 MHz / 16 = 3 MHz BCLK
     *   → LRCLK = 3 MHz / 64 = 46.875 kHz
     */
    I2S->CLKCTRL[0].reg =
        (3u << I2S_CLKCTRL_SLOTSIZE_Pos) |     // 32-bit
        (1u << I2S_CLKCTRL_NBSLOTS_Pos)  |     // 2 slots
        I2S_CLKCTRL_BITDELAY             |
        I2S_CLKCTRL_FSSEL_SCKDIV         |
        I2S_CLKCTRL_SCKSEL_MCKDIV        |
        I2S_CLKCTRL_MCKSEL_GCLK          |
        (15u << I2S_CLKCTRL_MCKDIV_Pos);

    /*
     * Serializer 1 (connected to PA08 / SD1)
     * TX, left-aligned, 32-bit data, clocked by unit 0
     */
    I2S->SERCTRL[1].reg =
        I2S_SERCTRL_SERMODE_TX   |
        I2S_SERCTRL_TXSAME       |             // repeat last sample if underrun
        I2S_SERCTRL_SLOTADJ_LEFT |
        I2S_SERCTRL_DATASIZE_32  |
        I2S_SERCTRL_CLKSEL_CLK0;

    // Enable I2S + clock unit 0 + serializer 1
    I2S->CTRLA.reg =
        I2S_CTRLA_ENABLE |
        I2S_CTRLA_CKEN0  |
        I2S_CTRLA_SEREN1;

    wait_i2s_sync(I2S_SYNCBUSY_ENABLE |
                  I2S_SYNCBUSY_CKEN0  |
                  I2S_SYNCBUSY_SEREN1);
}

// ----------------------------------------------------------------------------
// Blocking stereo write (left = right)
// ----------------------------------------------------------------------------
static void i2s_write_stereo(int32_t sample)
{
    // Left
    while (!(I2S->INTFLAG.bit.TXRDY1)) {}
    while (I2S->SYNCBUSY.bit.DATA1) {}
    I2S->DATA[1].reg = (uint32_t)sample;

    // Right
    while (!(I2S->INTFLAG.bit.TXRDY1)) {}
    while (I2S->SYNCBUSY.bit.DATA1) {}
    I2S->DATA[1].reg = (uint32_t)sample;
}

// ----------------------------------------------------------------------------
// Arduino entry points
// ----------------------------------------------------------------------------
void setup()
{
    led_init();

    // Quick visual confirmation that we reached setup()
    for (int i = 0; i < 6; i++) {
        led_toggle();
        delay(80);
    }

    phase_inc = (uint32_t)(((uint64_t)TONE_HZ << 32) / SAMPLE_RATE_HZ);

    configure_i2s();

    // Steady-on after I2S init succeeds
    PORT->Group[0].OUTSET.reg = PORT_PA17;
}

void loop()
{
    // Heartbeat: toggle LED every ~250 ms of audio samples
    static uint32_t sample_count = 0;
    if (++sample_count >= (SAMPLE_RATE_HZ / 4)) {
        sample_count = 0;
        led_toggle();
    }

    uint32_t idx   = phase >> 26;          // top 6 bits → table index
    int32_t  sample = sine_table[idx];

    i2s_write_stereo(sample);
    phase += phase_inc;
}
