#include <stdint.h>
#include "sam.h"
#include "audio_config.h"

#define I2S_GCLK_ID_0          0x23u
#define I2S_APB_CLOCK_BIT      (1u << 20)

#define I2S_CTRLA_ENABLE       (1u << 1)
#define I2S_CTRLA_CKEN0        (1u << 2)
#define I2S_CTRLA_SEREN1       (1u << 5)

#define I2S_SYNC_ENABLE        (1u << 1)
#define I2S_SYNC_CKEN0         (1u << 2)
#define I2S_SYNC_SEREN1        (1u << 5)
#define I2S_SYNC_DATA1         (1u << 6)

#define I2S_CLKCTRL_SLOTSIZE_32   (3u << 0)
#define I2S_CLKCTRL_NBSLOTS_2     (1u << 2)
#define I2S_CLKCTRL_SCKSEL        (1u << 4)
#define I2S_CLKCTRL_BITDELAY_I2S  (1u << 7)

/*
 * MCKDIV is encoded as divider-1.  A value of 30 therefore gives
 * division by 31.
 */
#define I2S_CLKCTRL_MCKDIV_31     (30u << 16)

#define I2S_SERCTRL_TX            (1u << 0)
#define I2S_SERCTRL_SLOTADJ_LEFT  (1u << 7)
#define I2S_SERCTRL_DATASIZE_32   (0u << 8)
#define I2S_SERCTRL_CLKSEL_CLK0   (0u << 5)

#define I2S_INTFLAG_TXRDY1        (1u << 9)

static const int32_t sine_64[64] = {
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

static void wait_gclk_sync(void)
{
    while (GCLK->STATUS.bit.SYNCBUSY) {}
}

static void wait_i2s_sync(uint32_t mask)
{
    while (I2S->SYNCBUSY.reg & mask) {}
}

static void configure_i2s_pins(void)
{
    PORT->Group[0].PINCFG[AUDIO_PIN_SD1].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[AUDIO_PIN_SD1 / 2].reg =
        (PORT->Group[0].PMUX[AUDIO_PIN_SD1 / 2].reg & 0xF0u) |
        AUDIO_I2S_PIN_MUX;

    PORT->Group[0].PINCFG[AUDIO_PIN_SCK0].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[AUDIO_PIN_SCK0 / 2].reg =
        (PORT->Group[0].PMUX[AUDIO_PIN_SCK0 / 2].reg & 0xF0u) |
        AUDIO_I2S_PIN_MUX;

    PORT->Group[0].PINCFG[AUDIO_PIN_FS0].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[AUDIO_PIN_FS0 / 2].reg =
        (PORT->Group[0].PMUX[AUDIO_PIN_FS0 / 2].reg & 0x0Fu) |
        (AUDIO_I2S_PIN_MUX << 4);
}

static void configure_i2s_clock(void)
{
    /*
     * GCLK3 = DFLL48M / 1 = 48 MHz.
     */
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(3u) | GCLK_GENDIV_DIV(1u);
    wait_gclk_sync();

    GCLK->GENCTRL.reg =
        GCLK_GENCTRL_ID(3u) |
        GCLK_GENCTRL_SRC_DFLL48M |
        GCLK_GENCTRL_IDC |
        GCLK_GENCTRL_GENEN;
    wait_gclk_sync();

    GCLK->CLKCTRL.reg =
        GCLK_CLKCTRL_ID(I2S_GCLK_ID_0) |
        GCLK_CLKCTRL_GEN(3u) |
        GCLK_CLKCTRL_CLKEN;
    wait_gclk_sync();
}

static void configure_i2s(void)
{
    PM->APBCMASK.reg |= I2S_APB_CLOCK_BIT;

    configure_i2s_clock();
    configure_i2s_pins();

    I2S->CTRLA.reg = 0;
    I2S->CTRLA.bit.SWRST = 1;
    wait_i2s_sync(1u);

    /*
     * Internal SCK generation, 32-bit stereo slots, Philips I2S
     * one-bit data delay, divide the 48 MHz I2S clock by 31.
     *
     * Result:
     *   LRCLK = 48,000,000 / 31 / 32 = 48,387.096 Hz
     *   BCLK  = LRCLK * 64 = 3.096774 MHz
     */
    I2S->CLKCTRL[0].reg =
        I2S_CLKCTRL_SLOTSIZE_32 |
        I2S_CLKCTRL_NBSLOTS_2 |
        I2S_CLKCTRL_SCKSEL |
        I2S_CLKCTRL_BITDELAY_I2S |
        I2S_CLKCTRL_MCKDIV_31;

    I2S->SERCTRL[1].reg =
        I2S_SERCTRL_TX |
        I2S_SERCTRL_SLOTADJ_LEFT |
        I2S_SERCTRL_DATASIZE_32 |
        I2S_SERCTRL_CLKSEL_CLK0;

    I2S->CTRLA.reg =
        I2S_CTRLA_ENABLE |
        I2S_CTRLA_CKEN0 |
        I2S_CTRLA_SEREN1;

    wait_i2s_sync(I2S_SYNC_ENABLE | I2S_SYNC_CKEN0 | I2S_SYNC_SEREN1);
}

static void audio_write_sample(int32_t sample)
{
    while ((I2S->INTFLAG.reg & I2S_INTFLAG_TXRDY1) == 0u) {}
    while (I2S->SYNCBUSY.reg & I2S_SYNC_DATA1) {}
    I2S->DATA[1].reg = (uint32_t)sample;

    while ((I2S->INTFLAG.reg & I2S_INTFLAG_TXRDY1) == 0u) {}
    while (I2S->SYNCBUSY.reg & I2S_SYNC_DATA1) {}
    I2S->DATA[1].reg = (uint32_t)sample;
}

void setup(void)
{
    uint32_t phase = 0;

    /*
     * Numerically controlled oscillator.  The hardware sample rate is
     * 48 MHz / (31 * 32), so this produces approximately 1.000 kHz.
     */
    const uint32_t phase_increment =
        (uint32_t)(((uint64_t)AUDIO_TONE_HZ * 4294967296ULL *
                    AUDIO_I2S_DIVISION * AUDIO_SLOT_BITS) / 48000000ULL);

    configure_i2s();
}

void loop(void)
{
        const uint32_t table_index = phase >> 26;
        audio_write_sample(sine_64[table_index]);
        phase += phase_increment;
    }
}
