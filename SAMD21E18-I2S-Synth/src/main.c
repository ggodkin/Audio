#include <stdint.h>
#include "sam.h"
#include "audio_config.h"

#define AUDIO_APB_CLOCK_BIT         (1u << 20)
#define AUDIO_CTRLA_ENABLE          (1u << 1)
#define AUDIO_CTRLA_CKEN0           (1u << 2)
#define AUDIO_CTRLA_SEREN0          (1u << 5)
#define AUDIO_SYNC_ENABLE           (1u << 1)
#define AUDIO_SYNC_CKEN0            (1u << 2)
#define AUDIO_SYNC_SEREN0           (1u << 5)
#define AUDIO_SYNC_DATA0            (1u << 6)
#define AUDIO_CLKCTRL_SLOTSIZE_32   (3u << 0)
#define AUDIO_CLKCTRL_NBSLOTS_2     (1u << 2)
#define AUDIO_CLKCTRL_BITDELAY_I2S  (1u << 7)
#define AUDIO_CLKCTRL_FSSEL_SCKDIV  (0u << 8)
#define AUDIO_CLKCTRL_SCKSEL_MCKDIV (1u << 12)
#define AUDIO_CLKCTRL_MCKSEL_GCLK   (0u << 16)
#define AUDIO_CLKCTRL_MCKDIV_16     (15u << 19)
#define AUDIO_SERCTRL_TX             (1u << 0)
#define AUDIO_SERCTRL_SLOTADJ_LEFT  (1u << 7)
#define AUDIO_SERCTRL_DATASIZE_32   (0u << 8)
#define AUDIO_SERCTRL_CLKSEL_CLK0   (0u << 5)
#define AUDIO_INTFLAG_TXRDY0        (1u << 9)

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

static uint32_t phase;
static uint32_t phase_increment;

static void configure_i2s_pins(void)
{
    PORT->Group[0].PINCFG[AUDIO_PIN_SD0].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[AUDIO_PIN_SD0 / 2].reg =
        (PORT->Group[0].PMUX[AUDIO_PIN_SD0 / 2].reg & 0x0Fu) |
        (AUDIO_I2S_PIN_MUX << 4);

    PORT->Group[0].PINCFG[AUDIO_PIN_SCK0].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[AUDIO_PIN_SCK0 / 2].reg =
        (PORT->Group[0].PMUX[AUDIO_PIN_SCK0 / 2].reg & 0x0Fu) |
        (AUDIO_I2S_PIN_MUX << 4);

    PORT->Group[0].PINCFG[AUDIO_PIN_FS0].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[AUDIO_PIN_FS0 / 2].reg =
        (PORT->Group[0].PMUX[AUDIO_PIN_FS0 / 2].reg & 0xF0u) |
        AUDIO_I2S_PIN_MUX;
}

static void configure_i2s_clock(void)
{
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
    PM->APBCMASK.reg |= AUDIO_APB_CLOCK_BIT;

    configure_i2s_clock();
    configure_i2s_pins();

    I2S->CTRLA.reg = 0;
    I2S->CTRLA.bit.SWRST = 1;
    wait_i2s_sync(1u);

    I2S->CLKCTRL[0].reg =
        AUDIO_CLKCTRL_SLOTSIZE_32 |
        AUDIO_CLKCTRL_NBSLOTS_2 |
        AUDIO_CLKCTRL_BITDELAY_I2S |
        AUDIO_CLKCTRL_FSSEL_SCKDIV |
        AUDIO_CLKCTRL_SCKSEL_MCKDIV |
        AUDIO_CLKCTRL_MCKSEL_GCLK |
        AUDIO_CLKCTRL_MCKDIV_16;

    I2S->SERCTRL[0].reg =
        AUDIO_SERCTRL_TX |
        AUDIO_SERCTRL_SLOTADJ_LEFT |
        AUDIO_SERCTRL_DATASIZE_32 |
        AUDIO_SERCTRL_CLKSEL_CLK0;

    I2S->CTRLA.reg =
        AUDIO_CTRLA_ENABLE |
        AUDIO_CTRLA_CKEN0 |
        AUDIO_CTRLA_SEREN0;

    wait_i2s_sync(AUDIO_SYNC_ENABLE | AUDIO_SYNC_CKEN0 | AUDIO_SYNC_SEREN0);
}

static void audio_write_sample(int32_t sample)
{
    while ((I2S->INTFLAG.reg & AUDIO_INTFLAG_TXRDY0) == 0u) {}
    while (I2S->SYNCBUSY.reg & AUDIO_SYNC_DATA0) {}
    I2S->DATA[0].reg = (uint32_t)sample;

    while ((I2S->INTFLAG.reg & AUDIO_INTFLAG_TXRDY0) == 0u) {}
    while (I2S->SYNCBUSY.reg & AUDIO_SYNC_DATA0) {}
    I2S->DATA[0].reg = (uint32_t)sample;
}

/*
 * Temporary hardware diagnostic:
 *
 * Drive PA10 (D13 / I2S SCK0) as an ordinary GPIO. This deliberately
 * bypasses the I2S peripheral so the physical pin and scope connection
 * can be verified independently of the I2S clock/serializer setup.
 *
 * Expected result: a square wave on PA10. The exact frequency is not
 * important; it is intentionally slow enough to see with the scope.
 */

void setup(void)
{
    PORT->Group[0].DIRSET.reg = PORT_PA10;
    PORT->Group[0].OUTCLR.reg = PORT_PA10;
}

void loop(void)
{
    PORT->Group[0].OUTTGL.reg = PORT_PA10;

    for (volatile uint32_t i = 0; i < 1000u; ++i) {
        __asm__ volatile ("nop");
    }
}
