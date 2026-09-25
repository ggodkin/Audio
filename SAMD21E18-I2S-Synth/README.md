# SAMD21E18A I2S Synthesizer

First hardware target for the Audio project: an ATSAMD21E18A generating a continuous single-tone PCM waveform over I2S.

## First milestone

- MCU: ATSAMD21E18A
- I2S master/controller
- Sample rate: 50 kHz
- Slot format: stereo, 32-bit slots
- Tone: 1 kHz sine wave
- Output: same tone on left and right channels
- I2S data: PA08 / SD1
- I2S bit clock: PA10 / SCK0
- I2S word select: PA11 / FS0
- MCLK: not connected
- DAC/amplifier targets:
  - MAX98357A
  - PCM5102A

The SAM D21 I2S peripheral supports two clock units and two serializers. This first implementation deliberately uses Serializer 1 with Clock Unit 0, allowing the compact PA08/PA10/PA11 pin group while leaving PA09/MCK unused.

The 50 kHz rate is intentional for the first bring-up: with a 48 MHz I2S generic clock, a divide-by-15 SCK gives 3.2 MHz BCLK, which is exactly 2 x 32 x 50 kHz.

## Wiring

| ATSAMD21E18A | I2S device |
|---|---|
| PA08 (SD1) | DIN / DATA |
| PA10 (SCK0) | BCLK |
| PA11 (FS0) | LRC / LRCLK / WS |
| GND | GND |
| 3.3 V | logic supply as appropriate |

Do not connect PA09/MCLK for this first test.

For a MAX98357A module, connect its speaker/output and power according to the module manufacturer's requirements.

For a PCM5102A module, connect its analog output to the following amplifier or line-level input; the PCM5102A does not itself drive a speaker.

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

## Initial acceptance test

1. Build and flash the firmware.
2. Connect PA08, PA10, and PA11 to an I2S DAC/amplifier.
3. Verify BCLK is approximately 3.2 MHz.
4. Verify LRCLK is approximately 50 kHz.
5. Verify the DAC/amplifier produces a continuous 1 kHz tone.
6. Capture the I2S data with a logic analyzer before moving to DMA.

No synthesizer library, RTOS, Arduino framework, or audio codec library is used in this first milestone.
