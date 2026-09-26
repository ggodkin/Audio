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
 * With the SAMD21 DFLL at 48 MHz, the nearest integer I2S clock
 * division for 48 kHz / 32-bit stereo is 31:
 *
 *   48 MHz / 31 / 32 = 48,387.096 Hz
 *
 * The MAX98357A accepts this LRCLK frequency (its 30.4-50.4 kHz
 * range includes it). BCLK is 3.096774 MHz, which is 64x LRCLK.
 */
#define AUDIO_ACTUAL_SAMPLE_RATE_HZ_NUM 46875u
#define AUDIO_ACTUAL_SAMPLE_RATE_HZ_DEN 1u

#endif
