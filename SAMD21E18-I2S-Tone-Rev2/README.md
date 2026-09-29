# SAMD21E18-I2S-Tone-Rev2

Bare ATSAMD21E18A → MAX98357A I2S synthesizer, based on the original
`SAMD21E18-I2S-Tone` project.

This revision fixes the audio sample-rate calculation and makes the
sample rate derive directly from the I2S clock configuration.

## What changed

The original project configured:

- GCLK0 = 48 MHz
- MCKDIV = 15
- 2 I2S slots
- 32 bits per slot

That produces:

- SCK/BCLK = 48 MHz / (15 + 1) = **3.000 MHz**
- LRCLK/sample rate = 3.000 MHz / (2 × 32) = **46,875 Hz**

The original synth constant was 31,815 Hz, so oscillator phase increments
and ADSR timing did not match the actual I2S sample rate. This revision
uses the calculated **46,875 Hz** rate everywhere in the synth.

The project intentionally keeps the existing 3 MHz / 46.875 kHz clocking.
A future revision can move to an exact 48 kHz audio clock after the SAMD21
clock tree is configured for an appropriate I2S source.

## Hardware

| ATSAMD21E18A | Device |
|--------------|--------|
| **PA08** | MAX98357A DIN |
| **PA10** | MAX98357A BCLK |
| **PA11** | MAX98357A LRC |
| **PA17** | Heartbeat LED |
| **PA14** | Encoder A |
| **PA15** | Encoder B |
| **PA22** | Encoder push |
| **PA16** | TM1638 STB |
| **PA18** | TM1638 CLK |
| **PA19** | TM1638 DIO |
| GND | Common ground |

## Audio format

- I2S controller/master
- Serializer 1 transmit
- 2 slots/frame
- 32 bits/slot
- Left-adjusted data
- 3.000 MHz BCLK
- 46,875 Hz LRCLK
- Same mono sample sent to both stereo slots
- No MCLK connection is required by the MAX98357A

## Synth

- 4 simultaneous voices
- 1024-entry sine table
- 32-bit phase accumulator
- Linear interpolation between sine-table entries
- ADSR presets
- Drum mode with filtered LFSR noise
- Rotary encoder for volume/preset selection
- TM1638 8-button input/display

## Presets

| # | Label | Description |
|---|-------|-------------|
| 1 | ORG | Organ – instant attack, full sustain |
| 2 | PLK | Pluck – instant attack, fast decay |
| 3 | PAD | Pad – slow attack, long release |
| 4 | BRS | Brass – medium attack, 50% sustain |
| 5 | PNO | Piano – fast attack, decays while held |
| 6 | SAX | Saxophone – medium attack + harmonics |
| 7 | VLN | Violin – slow bow attack + harmonic |
| 8 | DRM | Drum kit – short percussive hits |

## Build and upload

From the project directory:

```bash
pio run
pio run -t upload
```

The upload configuration uses the existing ST-LINK V2 / OpenOCD SWD
setup from the original project and programs the ATSAMD21E18A without a
bootloader.

## Validation

With a logic analyzer or scope, the expected I2S clocks are approximately:

- BCLK: **3.000 MHz**
- LRCLK: **46.875 kHz**
- BCLK/LRCLK ratio: **64:1**

For a 523 Hz C5 test tone, the generated frequency should be approximately
523 Hz rather than the approximately 771 Hz that resulted from using the
old 31,815 Hz software rate against the 46,875 Hz hardware rate.
