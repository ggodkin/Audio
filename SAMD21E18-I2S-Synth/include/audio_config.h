#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

#include <stdint.h>

#define AUDIO_SAMPLE_RATE_HZ   48000u
#define AUDIO_TONE_HZ           1000u
#define AUDIO_SLOT_BITS           32u
#define AUDIO_CHANNELS              2u

#define AUDIO_PIN_SD1              8u
#define AUDIO_PIN_SCK0            10u
#define AUDIO_PIN_FS0             11u
#define AUDIO_PIN_MCK0             9u

#define AUDIO_I2S_PIN_MUX           6u

/*
 * The SAMD21 DFLL provides 48 MHz. With two 32-bit I2S slots,
 * an exact 48 kHz frame rate would require a 3.072 MHz SCK,
 * which cannot be obtained from the 48 MHz DFLL using the
 * SAMD21's integer MCKDIV divider.
 *
 * Use the exact integer-divider clock instead:
 *
 *   GCLK_I2S0 = 48 MHz
 *   SCK       = 48 MHz / (15 + 1) = 3 MHz
 *   LRCLK     = 3 MHz / (2 * 32) = 46.875 kHz
 *
 * The MAX98357A specifies LRCLK operation from 30.4 kHz to
 * 50.4 kHz for this audio mode, so 46.875 kHz is valid.
 */
#define AUDIO_ACTUAL_SAMPLE_RATE_HZ_NUM 46875u
#define AUDIO_ACTUAL_SAMPLE_RATE_HZ_DEN     1u

#endif
