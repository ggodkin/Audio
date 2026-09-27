# SAMD21E18-I2S-Tone

Bare ATSAMD21E18A → MAX98357A with I2S sine, rotary volume, and **TM1638 LED&KEY** (8 buttons).

## Hardware

| ATSAMD21E18A | Device |
|--------------|--------|
| **PA08** | MAX98357A DIN |
| **PA10** | MAX98357A BCLK |
| **PA11** | MAX98357A LRC |
| **PA17** | Heartbeat LED |
| **PA14** | Encoder A (volume) |
| **PA15** | Encoder B (volume) |
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

## Buttons → notes

| Button | Note | Hz |
|--------|------|-----|
| S1 | C4 | 262 |
| S2 | D4 | 294 |
| S3 | E4 | 330 |
| S4 | F4 | 349 |
| S5 | G4 | 392 |
| S6 | A4 | 440 |
| S7 | B4 | 494 |
| S8 | C5 | 523 |

- Hold = note on, release = note off  
- Monophonic (one note at a time)  
- Module LED under a held button lights up  

## Volume

Encoder PA14/PA15: 0…64, starts at **1/32** of max.

## Build

```bash
cd SAMD21E18-I2S-Tone
pio run -t upload
```

If button order feels reversed or wrong LEDs light, say so — clone modules sometimes swap key bit order.
