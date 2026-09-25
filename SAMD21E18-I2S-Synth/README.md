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

1. Build the firmware with PlatformIO.
2. Flash the ATSAMD21E18A with the configured SWD programmer.
3. Power the MAX98357A and speaker.
4. Reset the SAMD21.
5. A continuous approximately 1 kHz tone should be heard.

The waveform uses a phase accumulator rather than assuming an integer number of samples per tone cycle, so the approximately 48.387 kHz hardware sample rate still produces a 1 kHz tone.

## PlatformIO

This project now uses PlatformIO rather than a hand-maintained Makefile. The firmware remains bare-metal register-level I2S code; PlatformIO is only providing the build environment, SAMD21 device support, linker integration, and programming infrastructure.

Build:

    pio run

Upload:

    pio run -t upload

The initial PlatformIO configuration uses the SAMD21E18A board definition and SWD upload infrastructure. We will verify the exact ST-LINK upload configuration against the programmer hardware before changing any firmware.

## Software structure

- src/main.c — SAMD21 bring-up and tone streaming
- src/startup.c — minimal Cortex-M0+ startup/vector table
- include/audio_config.h — audio format and pin configuration
- linker.ld — memory map
- platformio.ini — PlatformIO build/programming configuration

The first implementation intentionally uses polling rather than DMA or interrupts. Once the electrical I2S link is proven, the next step is to move the same waveform generator behind a small portable audio interface and add DMA.

No synthesizer library, RTOS, or audio codec library is used in this first milestone.
