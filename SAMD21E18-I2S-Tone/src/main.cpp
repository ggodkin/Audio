/*
 * ATSAMD21E18A → MAX98357A
 * Continuous 440 Hz sine wave over I2S (stereo, 32-bit slots)
 *
 * Wiring (bare chip):
 *   PA08  →  DIN  (I2S SD1)
 *   PA10  →  BCLK (I2S SCK0)
 *   PA11  →  LRC  (I2S FS0)
 *   GND   →  GND
 *   MCLK left unconnected
 *
 * Sample rate ≈ 46.875 kHz (exact integer divider from 48 MHz DFLL)
 * Future: expand this into a polyphonic synthesizer
 */

#include <Arduino.h>
#include "sam.h"

// ----------------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------------
static constexpr uint32_t TONE_HZ          = 440;
static constexpr uint32_t SAMPLE_RATE_HZ   = 46875;   // 48 MHz / 16 / 64
static constexpr uint32_t SINE_TABLE_SIZE  = 64;

// ----------------------------------------------------------------------------
// 64-entry sine table, full-scale 32-bit signed (peak ≈ 2^30)
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

// Phase accumulator (32-bit fixed-point)
static uint32_t phase = 0;
static uint32_t phase_inc;

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
// Pinmux – PA08=SD1, PA10=SCK0, PA11=FS0  (peripheral function G = 6)
// ----------------------------------------------------------------------------
static void configure_i2s_pins(void)
{
    // PA08 – I2S/SD1  (even pin → PMUXE)
    PORT->Group[0].PINCFG[8].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[8 >> 1].bit.PMUXE = 6;   // G

    // PA10 – I2S/SCK0 (even pin → PMUXE)
    PORT->Group[0].PINCFG[10].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[10 >> 1].bit.PMUXE = 6;

    // PA11 – I2S/FS0  (odd pin → PMUXO)
    PORT->Group[0].PINCFG[11].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[11 >> 1].bit.PMUXO = 6;
}

// ----------------------------------------------------------------------------
// GCLK3 = 48 MHz DFLL → I2S
// ----------------------------------------------------------------------------
static void configure_i2s_clock(void)
{
    // Generator 3: source DFLL48M, no division
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(3) | GCLK_GENDIV_DIV(1);
    wait_gclk_sync();

    GCLK->GENCTRL.reg =
        GCLK_GENCTRL_ID(3) |
        GCLK_GENCTRL_SRC_DFLL48M |
        GCLK_GENCTRL_IDC |
        GCLK_GENCTRL_GENEN;
    wait_gclk_sync();

    // Route GCLK3 to I2S (ID 0x18 on SAMD21)
    GCLK->CLKCTRL.reg =
        GCLK_CLKCTRL_ID(0x18) |          // I2S_GCLK_ID_0
        GCLK_CLKCTRL_GEN_GCLK3 |
        GCLK_CLKCTRL_CLKEN;
    wait_gclk_sync();
}

// ----------------------------------------------------------------------------
// I2S peripheral – master, 32-bit slots, 2 slots (stereo), I2S format
// ----------------------------------------------------------------------------
static void configure_i2s(void)
{
    // Enable APBC clock for I2S
    PM->APBCMASK.reg |= PM_APBCMASK_I2S;

    configure_i2s_clock();
    configure_i2s_pins();

    // Software reset
    I2S->CTRLA.bit.SWRST = 1;
    wait_i2s_sync(I2S_SYNCBUSY_SWRST);

    /*
     * CLKCTRL[0]
     *   SLOTSIZE = 32 bit
     *   NBSLOTS  = 2 (stereo)
     *   BITDELAY = 1 (I2S format – one SCK delay after FS)
     *   FSSEL    = SCKDIV (FS derived from SCK)
     *   SCKSEL   = MCKDIV
     *   MCKSEL   = GCLK
     *   MCKDIV   = 15  →  48 MHz / 16 = 3 MHz SCK
     *   → LRCLK = 3 MHz / 64 = 46.875 kHz
     */
    I2S->CLKCTRL[0].reg =
        I2S_CLKCTRL_SLOTSIZE(3) |          // 32-bit
        I2S_CLKCTRL_NBSLOTS(1) |           // 2 slots (value 1 = 2)
        I2S_CLKCTRL_BITDELAY |
        I2S_CLKCTRL_FSSEL_SCKDIV |
        I2S_CLKCTRL_SCKSEL_MCKDIV |
        I2S_CLKCTRL_MCKSEL_GCLK |
        I2S_CLKCTRL_MCKDIV(15);

    /*
     * SERCTRL[1] – we use serializer 1 because SD1 is on PA08
     *   TX mode, left-justified data in slot, 32-bit, clock from unit 0
     */
    I2S->SERCTRL[1].reg =
        I2S_SERCTRL_SERMODE_TX |
        I2S_SERCTRL_SLOTADJ_LEFT |
        I2S_SERCTRL_DATASIZE_32 |
        I2S_SERCTRL_CLKSEL_CLK0;

    // Enable peripheral, clock unit 0 and serializer 1
    I2S->CTRLA.reg =
        I2S_CTRLA_ENABLE |
        I2S_CTRLA_CKEN0 |
        I2S_CTRLA_SEREN1;

    wait_i2s_sync(I2S_SYNCBUSY_ENABLE |
                  I2S_SYNCBUSY_CKEN0 |
                  I2S_SYNCBUSY_SEREN1);
}

// ----------------------------------------------------------------------------
// Write one stereo sample (left = right)
// ----------------------------------------------------------------------------
static void i2s_write_stereo(int32_t sample)
{
    // Wait for TX ready on serializer 1
    while (!(I2S->INTFLAG.reg & I2S_INTFLAG_TXRDY1)) {}
    while (I2S->SYNCBUSY.reg & I2S_SYNCBUSY_DATA1) {}
    I2S->DATA[1].reg = (uint32_t)sample;          // Left

    while (!(I2S->INTFLAG.reg & I2S_INTFLAG_TXRDY1)) {}
    while (I2S->SYNCBUSY.reg & I2S_SYNCBUSY_DATA1) {}
    I2S->DATA[1].reg = (uint32_t)sample;          // Right
}

// ----------------------------------------------------------------------------
// Arduino entry points
// ----------------------------------------------------------------------------
void setup()
{
    // Phase increment for 440 Hz at actual sample rate
    // phase_inc = tone * 2^32 / sample_rate
    phase_inc = (uint32_t)(((uint64_t)TONE_HZ << 32) / SAMPLE_RATE_HZ);

    configure_i2s();
}

void loop()
{
    // Generate continuous 440 Hz tone
    uint32_t idx = phase >> 26;                 // top 6 bits → 0…63
    int32_t  sample = sine_table[idx];

    i2s_write_stereo(sample);

    phase += phase_inc;
}
