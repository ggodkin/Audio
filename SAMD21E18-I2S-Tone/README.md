# SAMD21E18-I2S-Tone

Bare ATSAMD21E18A generating a continuous **440 Hz** sine wave over I2S to a **MAX98357A**, with rotary-encoder volume control.

## Hardware

| ATSAMD21E18A | Device | Signal |
|--------------|--------|--------|
| **PA08** | MAX98357A DIN | I2S data (SD1) |
| **PA10** | MAX98357A BCLK | Bit clock (SCK0) |
| **PA11** | MAX98357A LRC | Word select (FS0) |
| **PA17** | LED (+ resistor to GND) | Heartbeat |
| **PA14** | Encoder A (CLK) | Volume up/down |
| **PA15** | Encoder B (DT) | Volume up/down |
| GND | Encoder common | |
| GND | MAX98357A GND | |
| — | MAX98357A MCLK | Leave open |

Encoder: mechanical quadrature, common to GND. **Internal pull-ups** are enabled on PA14/PA15 — no external resistors required.

Volume range: **0 (mute) … 64 (full)**. Starts at 50 %. One detent ≈ one step.

## Build & flash (ST-LINK V2)

```bash
cd SAMD21E18-I2S-Tone
pio run -t upload
```

## Notes

- Sample rate ≈ 46.875 kHz (exact divider from 48 MHz DFLL)
- Serializer 1 / DATA[1] because data is on PA08 (SD1)
- GCLK channel for I2S_0 is **ID 35**
- Reset button needs 10 kΩ pull-up to 3.3 V (no extra capacitor if it interferes with power-on)
