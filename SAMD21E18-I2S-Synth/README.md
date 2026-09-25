# SAMD21E18A I2S Synthesizer

First hardware target for the Audio project: an ATSAMD21E18A generating a continuous single-tone PCM waveform over I2S.

## First hardware milestone

- MCU: ATSAMD21E18A
- I2S master/controller
- Nominal sample rate: 48 kHz
- Actual sample rate with the SAMD21 integer divider: 48,387 Hz
- Stereo, 32-bit slots
- Tone: approximately 1 kHz sine wave
- Same tone on left and right channels
- I2S data: PA08 / SD1
- I2S bit clock: PA10 / SCK0
- I2S word select: PA11 / FS0
- MCLK: not connected

The SAMD21 is clocked from its 48 MHz DFLL. The I2S clock divider is 31, giving:

    48,000,000 / 31 / 32 = 48,387.096 Hz sample rate

and:

    48,387.096 * 64 = 3.096774 MHz BCLK

This is intentionally close to 48 kHz and is within the MAX98357A's specified LRCLK range.

## Wiring

| ATSAMD21E18A | MAX98357A |
|---|---|
| PA08 (SD1) | DIN / DATA |
| PA10 (SCK0) | BCLK |
| PA11 (FS0) | LRC / LRCLK / WS |
| GND | GND |

Do not connect PA09/MCLK for this test.

Power the MAX98357A according to the particular breakout board. Connect the speaker only between the amplifier's OUT+ and OUT- terminals; do not connect either speaker terminal to GND.

## Audio test

The acceptance test is deliberately audible; no logic analyzer is required.

1. Build the firmware.
2. Flash the ATSAMD21E18A with ST-LINK/OpenOCD.
3. Power the MAX98357A and speaker.
4. Reset the SAMD21.
5. A continuous approximately 1 kHz tone should be heard.

The waveform uses a phase accumulator rather than assuming an integer number of samples per tone cycle, so the approximately 48.387 kHz hardware sample rate still produces a 1 kHz tone.

## Software structure

The project is intentionally split so that later SAMD51, RP2040/RP2350, and ESP32 implementations can reuse the synthesizer concept without copying the MCU-specific I2S driver.

- src/main.c — SAMD21 bring-up and tone streaming
- include/audio_config.h — audio format and pin configuration
- src/startup.c — minimal Cortex-M0+ startup/vector table
- linker.ld — SAMD21E18A memory map
- Makefile — arm-none-eabi-gcc build and OpenOCD programming

The first implementation intentionally uses polling rather than DMA or interrupts. Once the electrical I2S link is proven, the next step is to move the same waveform generator behind a small portable audio interface and add DMA.

## Building

The project expects a CMSIS device package containing the SAMD21 device headers and system_samd21.c/.h.

Set:

    export SAMD21_CMSIS=/path/to/CMSIS/Device/ATMEL/samd21
    export CMSIS_CORE=/path/to/CMSIS/Core/Include

Then:

    make

The default programmer target is ST-LINK/OpenOCD:

    make flash

The OpenOCD configuration is deliberately not hard-coded to a particular ST-LINK adapter. Set OPENOCD_CFG for the adapter/target configuration being used.

No synthesizer library, RTOS, Arduino framework, or audio codec library is used in this first milestone.
