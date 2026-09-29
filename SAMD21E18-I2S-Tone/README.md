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
| 6 | SAX | Saxophone – medium attack + harmonics |
| 7 | VLN | Violin – slow bow attack + 2nd harmonic |
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

## Buttons → drums (DRM mode)

| Button | Sound |
|--------|--------|
| S1 | Kick |
| S2 | Snare |
| S3 | Hi-hat (filtered noise) |
| S4 | Clap |
| S5 | Tom low |
| S6 | Tom mid |
| S7 | Rim |
| S8 | Crash (filtered noise) |

- Hold = on, release = off  
- 4-voice polyphonic  
- LED under held button lights up  

## Volume

Encoder: 0…64. Push toggles volume ↔ preset select.

## Build

```bash
cd SAMD21E18-I2S-Tone
pio run -t upload
```
