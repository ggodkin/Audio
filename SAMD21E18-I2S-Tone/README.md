# SAMD21E18-I2S-Tone

Bare ATSAMD21E18A generating a continuous **440 Hz** sine wave over I2S to a **MAX98357A** amplifier.

This is the starting point for a future polyphonic synthesizer.

## Hardware

| ATSAMD21E18A | MAX98357A | Signal |
|--------------|-----------|--------|
| **PA08**     | DIN       | I2S data (SD1) |
| **PA10**     | BCLK      | Bit clock (SCK0) |
| **PA11**     | LRC / WS  | Word select (FS0) |
| GND          | GND       | Common ground |
| —            | MCLK      | **Leave unconnected** |

Power the MAX98357A from 2.5–5.5 V (3.3 V or 5 V both work).  
Connect the speaker **only** between the amp’s OUT+ and OUT– terminals.

## Audio parameters

- Sample rate: **46.875 kHz** (exact integer divider from the 48 MHz DFLL)
- Format: I2S, 32-bit slots, stereo (same tone on L and R)
- Tone: 440 Hz (A4) continuous sine
- Phase accumulator → no audible glitches

## Build & flash (ST-LINK V2)

```bash
cd SAMD21E18-I2S-Tone
pio run
pio run -t upload
```

The `platformio.ini` already contains the working OpenOCD command for ST-LINK V2.

## Project layout

```
SAMD21E18-I2S-Tone/
├── platformio.ini
├── README.md
└── src/
    └── main.cpp          ← I2S bring-up + 440 Hz generator
```

## Next steps (polyphonic synthesizer)

1. Replace the single phase accumulator with a voice array.
2. Add note on/off, envelopes, and simple waveforms.
3. Move sample generation behind a small buffer + DMA (Adafruit ZeroDMA or direct DMAC).
4. Optionally switch to the Adafruit Zero I2S library once the electrical link is proven.

## Notes

- The code uses serializer 1 (`SERCTRL[1]` / `DATA[1]`) because the data pin is **PA08 (SD1)**.
- MCLK is not generated; the MAX98357A derives its clocks from BCLK.
- The actual sample rate is slightly lower than 48 kHz so the SAMD21 integer dividers stay exact.
