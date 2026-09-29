# SAMD21E18-I2S-Tone

Bare ATSAMD21E18A → MAX98357A with I2S 4-voice synth, rotary volume/preset, and **TM1638 LED&KEY** (8 buttons).

## Hardware

| ATSAMD21E18A | Device |
|--------------|--------|
| **PA08** | MAX98357A DIN |
| **PA10** | MAX98357A BCLK |
| **PA11** | MAX98357A LRC |
| **PA17** | Heartbeat LED |
| **PA14** | Encoder A (volume / preset) |
| **PA15** | Encoder B (volume / preset) |
| **PA22** | Encoder push (toggle volume ↔ preset) |
| **PA16** | TM1638 **STB** |
| **PA18** | TM1638 **CLK** |
| **PA19** | TM1638 **DIO** |
| GND | Common ground |

## Presets (encoder push → select, turn to change)

| # | Label | Description |
|---|-------|-------------|
| 1 | ORG | Organ – instant attack, full sustain |
| 2 | PLK | Pluck – instant attack, fast decay |
| 3 | PAD | Pad – slow attack, long release |
| 4 | BRS | Brass – medium attack, 50% sustain |
| 5 | PNO | Piano – fast attack, decays while held |
| 6 | SAX | Saxophone – medium attack + quiet harmonics |
| 7 | VLN | Violin – slow bow attack + quiet 2nd harmonic |
| 8 | DRM | Drum kit – short percussive hits |

## Buttons → notes (melody modes)

| Button | Note | Hz |
|--------|------|-----|
| S1 | C5 | 523 |
| S2 | D5 | 587 |
| S3 | E5 | 659 |
| S4 | F5 | 698 |
| S5 | G5 | 784 |
| S6 | A5 | 880 |
| S7 | B5 | 988 |
| S8 | C6 | 1047 |

## Drum synthesis (DRM mode)

All pads share the global DRM envelope: instant attack, ~40 ms decay, no sustain.

| Button | Sound | Waveform | Pitch | Gain | Notes |
|--------|--------|----------|-------|------|-------|
| S1 | Kick | Pure sine | 150 Hz | 2.0× | Raised for small speakers |
| S2 | Snare | Pure sine | 200 Hz | 1.0× | Body tone only |
| S3 | Hi-hat | Sine + light filtered noise | 7 kHz | 1.1× | Metallic; sine-dominant |
| S4 | Clap | Pure sine | 280 Hz | 1.0× | |
| S5 | Tom low | Pure sine | 160 Hz | 1.0× | |
| S6 | Tom mid | Pure sine | 220 Hz | 1.0× | |
| S7 | Rim | Pure sine | 500 Hz | 1.0× | |
| S8 | Crash | Sine + light filtered noise | 4.5 kHz | 1.0× | Metallic; sine-dominant |

Noise uses a 16-bit LFSR through a one-pole low-pass so it is less harsh than raw white noise.

## Volume

Encoder: 0…64. Push toggles volume ↔ preset select.

Keys are 2-sample debounced to reduce ghost triggers when releasing one of several held buttons.

## Build

```bash
cd SAMD21E18-I2S-Tone
pio run -t upload
```
