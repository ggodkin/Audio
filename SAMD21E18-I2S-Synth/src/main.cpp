#include <Arduino.h>
#include <I2S.h>

#include "audio_config.h"

/*
 * The SparkFun Qwiic Micro variant predates the I2S pin macros expected by
 * Arduino's SAMD I2S library. The build flags define the library instance
 * using the board's Arduino pin numbers:
 *
 *   PA08 = D0   -> I2S SD1
 *   PA10 = D13  -> I2S SCK0
 *   PA11 = D33  -> I2S FS0
 */

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

static uint32_t phase;
static uint32_t phase_increment;

void setup()
{
    // Diagnostic status output: onboard LED is connected to PA17.
    // Drive the port directly so this test is independent of Arduino pin mapping.
    // HIGH = I2S.begin() succeeded; blinking = I2S.begin() failed.
    PORT->Group[0].DIRSET.reg = PORT_PA17;
    PORT->Group[0].OUTCLR.reg = PORT_PA17;

    /*
     * Arduino's SAMD I2S library requests:
     *   BCLK = sampleRate * 2 * bitsPerSample
     *
     * With the 48 MHz DFLL and 32-bit stereo frames, the integer
     * divider gives 3 MHz BCLK and 46.875 kHz LRCLK.
     */
    if (!I2S.begin(I2S_PHILIPS_MODE, AUDIO_SAMPLE_RATE_HZ, AUDIO_SLOT_BITS)) {
        // I2S.begin() allocates DMA before configuring the I2S clock.
        // If DMA allocation fails, no BCLK/LRCLK will be generated.
        while (true) {
            PORT->Group[0].OUTSET.reg = PORT_PA17;
            delay(150);
            PORT->Group[0].OUTCLR.reg = PORT_PA17;
            delay(150);
        }
    }

    // Keep the diagnostic pin HIGH so we know setup reached this point.
    PORT->Group[0].OUTSET.reg = PORT_PA17;

    phase_increment =
        (uint32_t)(((uint64_t)AUDIO_TONE_HZ * 4294967296ULL) /
                   AUDIO_ACTUAL_SAMPLE_RATE_HZ_NUM);
}

void loop()
{
    const uint32_t table_index = phase >> 26;
    I2S.write(sine_64[table_index]);
    phase += phase_increment;
}
