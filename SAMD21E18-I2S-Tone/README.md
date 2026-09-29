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

### TM1638 LED&KEY module

```
Module     SAMD21
------     ------
STB    →   PA16
CLK    →   PA18
DIO    →   PA19
VCC    →   5 V or 3.3 V (see module)
GND    →   GND
```

Many LED&KEY boards run logic fine at 3.3 V when VCC is 5 V; if unsure, power the module from **3.3 V** first.

## Presets (encoder push → select, turn to change)

| # | Label | Description |
|---|-------|-------------|
| 1 | ORG | Organ – instant attack, full sustain, medium release |
| 2 | PLK | Pluck – instant attack, fast decay to silence |
| 3 | PAD | Pad – slow attack, full sustain, long release |
| 4 | BRS | Brass – medium attack, decay to 50 % sustain |
| 5 | PNO | **Piano** – fast attack, decay to low sustain, long release |
| 6 | DRM | **Drum** – instant attack, very short decay (percussive) |

## Buttons → notes (melody modes 1–5)

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

| Button | Sound | Pitch (approx) |
|--------|-------|----------------|
| S1 | Kick | 60 Hz |
| S2 | Snare | 180 Hz |
| S3 | Hi-hat | noise + 8 kHz |
| S4 | Clap | 220 Hz |
| S5 | Tom low | 120 Hz |
| S6 | Tom mid | 180 Hz |
| S7 | Rim | 400 Hz |
| S8 | Crash | noise + 6 kHz |

- Hold = note/drum on, release = note/drum off  
- 4-voice polyphonic  
- Module LED under a held button lights up  

## Volume

Encoder PA14/PA15: 0…64, starts at **1/4** of max.  
Encoder push (PA22) toggles between volume and preset select.

## Build

```bash
cd SAMD21E18-I2S-Tone
pio run -t upload
```

If button order feels reversed or wrong LEDs light, say so — clone modules sometimes swap key bit order.
