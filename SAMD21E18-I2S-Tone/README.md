# SAMD21E18-I2S-Tone

Bare ATSAMD21E18A → MAX98357A with I2S sine, rotary volume, and **PS/2 keyboard note on/off**.

## Hardware

| ATSAMD21E18A | Device | Notes |
|--------------|--------|-------|
| **PA08** | MAX98357A DIN | I2S data |
| **PA10** | MAX98357A BCLK | |
| **PA11** | MAX98357A LRC | |
| **PA17** | LED | Heartbeat |
| **PA14** | Encoder A | Volume |
| **PA15** | Encoder B | Volume |
| **PA22** | PS/2 CLK | Level-shift from 5 V |
| **PA23** | PS/2 DATA | Level-shift from 5 V |
| GND | Common | |

### PS/2 voltage

PS/2 is **5 V** open-collector. SAMD21 pins are **3.3 V only**.

- Use a bidirectional level shifter, **or**
- Series ~2.2–10 kΩ + diode/clamp to 3.3 V, with pull-ups to **3.3 V** on the MCU side (firmware enables internal pull-ups).

Keyboard: +5 V and GND from a suitable supply; CLK→PA22, DATA→PA23.

## Key map (monophonic)

| Key | Note | Key | Note |
|-----|------|-----|------|
| A | C4 | W | C#4 |
| S | D4 | E | D#4 |
| D | E4 | T | F#4 |
| F | F4 | Y | G#4 |
| G | G4 | U | A#4 |
| H | A4 | | |
| J | B4 | | |
| K | C5 | | |

- **Key down** → note on (sound)
- **Key up** → note off (silence)
- One note at a time (last mapped key that is still held)

## Volume

Encoder on PA14/PA15: **0…64**, starts at **1/32** of max.

## Build

```bash
cd SAMD21E18-I2S-Tone
pio run -t upload
```
