/*
 * ATSAMD21E18A → MAX98357A  – diagnostic build
 *
 * PA08 = DIN (SD1), PA10 = BCLK (SCK0), PA11 = LRCLK (FS0), PA17 = LED
 *
 * LED codes:
 *   3 fast blinks at start of setup()
 *   then 1 blink = APB clock enabled
 *   then 2 blinks = GCLK routed
 *   then 3 blinks = pins muxed
 *   then 4 blinks = I2S reset done
 *   then 5 blinks = CLKCTRL/SERCTRL written
 *   then 6 blinks = I2S enabled
 *   then slow 1 Hz toggle = main loop running (samples being written)
 *
 * If it stops at a particular count, that is where it was hanging before.
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
// LED helpers (PA17, active high)
// ---------------------------------------------------------------------------
static void led_init(void)
{
    PORT->Group[0].DIRSET.reg = PORT_PA17;
    PORT->Group[0].OUTCLR.reg = PORT_PA17;
}

static void led_on(void)  { PORT->Group[0].OUTSET.reg = PORT_PA17; }
static void led_off(void) { PORT->Group[0].OUTCLR.reg = PORT_PA17; }
static void led_toggle(void) { PORT->Group[0].OUTTGL.reg = PORT_PA17; }

static void led_blink_n(int n, uint16_t on_ms = 120, uint16_t off_ms = 120)
{
    for (int i = 0; i < n; i++) {
        led_on();
        delay(on_ms);
        led_off();
        delay(off_ms);
    }
    delay(300);
}

// Timed waits – never hang forever
static void wait_gclk_sync_timeout(void)
{
    for (uint32_t i = 0; i < 100000u; i++) {
        if (!GCLK->STATUS.bit.SYNCBUSY) return;
    }
}

static void wait_i2s_sync_timeout(uint32_t mask)
{
    for (uint32_t i = 0; i < 100000u; i++) {
        if ((I2S->SYNCBUSY.reg & mask) == 0) return;
    }
}

// ---------------------------------------------------------------------------
// Step-by-step I2S bring-up with LED progress codes
// ---------------------------------------------------------------------------
static void configure_i2s(void)
{
    // 1. Enable APB clock for I2S
    PM->APBCMASK.reg |= PM_APBCMASK_I2S;
    led_blink_n(1);

    // 2. Route GCLK0 (48 MHz) to I2S
    //    GCLK ID for I2S clock unit 0 on SAMD21 = 0x18
    GCLK->CLKCTRL.reg = (uint16_t)(
        (0x18u << 0) |          // ID
        (0u << 8) |             // GEN = GCLK0
        (1u << 14)              // CLKEN
    );
    wait_gclk_sync_timeout();
    led_blink_n(2);

    // 3. Pinmux function G (6)
    // PA08 = SD1 (even → PMUXE of PMUX[4])
    PORT->Group[0].PINCFG[8].reg  |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[4].reg    = (PORT->Group[0].PMUX[4].reg & 0xF0) | 0x06;

    // PA10 = SCK0 (even → PMUXE of PMUX[5])
    PORT->Group[0].PINCFG[10].reg |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[5].reg    = (PORT->Group[0].PMUX[5].reg & 0xF0) | 0x06;

    // PA11 = FS0  (odd  → PMUXO of PMUX[5])
    PORT->Group[0].PINCFG[11].reg |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[5].reg    = (PORT->Group[0].PMUX[5].reg & 0x0F) | 0x60;
    led_blink_n(3);

    // 4. Software reset of I2S
    I2S->CTRLA.reg = (1u << 0);                 // SWRST
    wait_i2s_sync_timeout(1u << 0);
    led_blink_n(4);

    // 5. Clock unit 0 + Serializer 1 configuration (raw values)
    // CLKCTRL[0]:
    //   SLOTSIZE=3 (32bit), NBSLOTS=1 (2 slots), BITDELAY=1,
    //   FSSEL=0 (SCKDIV), SCKSEL=1 (MCKDIV), MCKSEL=0 (GCLK),
    //   MCKDIV=15 → 48MHz/16 = 3 MHz BCLK → 46.875 kHz LRCLK
    I2S->CLKCTRL[0].reg =
        (3u << 0)  |   // SLOTSIZE
        (1u << 2)  |   // NBSLOTS
        (1u << 7)  |   // BITDELAY
        (0u << 8)  |   // FSSEL = SCKDIV
        (1u << 12) |   // SCKSEL = MCKDIV
        (0u << 16) |   // MCKSEL = GCLK
        (15u << 19);   // MCKDIV

    // SERCTRL[1] – TX on serializer 1 (PA08 = SD1)
    // SERMODE=1 (TX), SLOTADJ=1 (left), DATASIZE=0 (32bit), CLKSEL=0 (CLK0)
    I2S->SERCTRL[1].reg =
        (1u << 0)  |   // SERMODE = TX
        (1u << 7)  |   // SLOTADJ = LEFT
        (0u << 8)  |   // DATASIZE = 32
        (0u << 5);     // CLKSEL = CLK0

    led_blink_n(5);

    // 6. Enable peripheral + clock unit 0 + serializer 1
    I2S->CTRLA.reg =
        (1u << 1) |    // ENABLE
        (1u << 2) |    // CKEN0
        (1u << 6);     // SEREN1

    wait_i2s_sync_timeout(
        (1u << 1) |    // ENABLE
        (1u << 2) |    // CKEN0
        (1u << 6));    // SEREN1

    led_blink_n(6);
}

// Non-blocking-ish write (with short timeout so LED still lives if I2S is dead)
static bool i2s_write_stereo(int32_t sample)
{
    uint32_t t;

    t = 0;
    while (!(I2S->INTFLAG.reg & (1u << 9))) {   // TXRDY1
        if (++t > 50000u) return false;
    }
    I2S->DATA[1].reg = (uint32_t)sample;

    t = 0;
    while (!(I2S->INTFLAG.reg & (1u << 9))) {
        if (++t > 50000u) return false;
    }
    I2S->DATA[1].reg = (uint32_t)sample;

    return true;
}

// ---------------------------------------------------------------------------
void setup()
{
    led_init();

    // 3 rapid blinks = we reached setup()
    led_blink_n(3, 60, 60);

    phase_inc = (uint32_t)(((uint64_t)TONE_HZ << 32) / SAMPLE_RATE_HZ);

    configure_i2s();

    // If we get here, init finished – solid on for a moment
    led_on();
    delay(500);
}

void loop()
{
    static uint32_t sample_count = 0;
    static bool i2s_ok = true;

    uint32_t idx = phase >> 26;
    int32_t sample = sine_table[idx & (SINE_LEN - 1)];

    if (!i2s_write_stereo(sample)) {
        // I2S never became ready – flash SOS-style and keep trying
        i2s_ok = false;
        led_blink_n(10, 40, 40);
    }

    phase += phase_inc;

    // Slow heartbeat only when I2S writes succeed
    if (i2s_ok && ++sample_count >= (SAMPLE_RATE_HZ / 2)) {
        sample_count = 0;
        led_toggle();
    }
}
