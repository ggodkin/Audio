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

#define I2S_CLKCTRL_SLOTSIZE_32   (3u << 0)
#define I2S_CLKCTRL_NBSLOTS_2     (1u << 2)
#define I2S_CLKCTRL_BITDELAY_I2S  (1u << 7)
#define I2S_CLKCTRL_MCKDIV_15     (14u << 16)

#define I2S_SERCTRL_TX            (1u << 0)
#define I2S_SERCTRL_SLOTADJ_LEFT  (1u << 7)
#define I2S_SERCTRL_DATASIZE_32   (0u << 8)
#define I2S_SERCTRL_CLKSEL_CLK0   (0u << 5)

#define I2S_INTFLAG_TXRDY1        (1u << 9)

static const int32_t sine_1khz_50khz[50] = {
         0,  134575535,  267028733,  395270728,  517279068,
   631129608,  735026857,  827332294,  906590206,  971550649,
  1021189158, 1054722903, 1071623039, 1071623039, 1054722903,
  1021189158,  971550649,  906590206,  827332294,  735026857,
   631129608,  517279068,  395270728,  267028733,  134575535,
         0, -134575535, -267028733, -395270728, -517279068,
  -631129608, -735026857, -827332294, -906590206, -971550649,
 -1021189158,-1054722903,-1071623039,-1071623039,-1054722903,
 -1021189158, -971550649, -906590206, -827332294, -735026857,
  -631129608, -517279068, -395270728, -267028733, -134575535
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
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(3u) | GCLK_GENDIV_DIV(0u);
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

    I2S->CLKCTRL[0].reg =
        I2S_CLKCTRL_SLOTSIZE_32 |
        I2S_CLKCTRL_NBSLOTS_2 |
        I2S_CLKCTRL_BITDELAY_I2S |
        I2S_CLKCTRL_MCKDIV_15;

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
    I2S->DATA[1].reg = (uint32_t)sample;

    while ((I2S->INTFLAG.reg & I2S_INTFLAG_TXRDY1) == 0u) {}
    I2S->DATA[1].reg = (uint32_t)sample;
}

int main(void)
{
    uint32_t index = 0;

    configure_i2s();

    for (;;) {
        audio_write_sample(sine_1khz_50khz[index]);
        index++;
        if (index >= 50u) {
            index = 0;
        }
    }
}
