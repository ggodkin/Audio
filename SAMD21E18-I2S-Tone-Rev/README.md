# SAMD21E18-I2S-Tone-Rev

Bare ATSAMD21E18A I2S synthesizer for MAX98357A.

## Why Rev

Rev3 showed substantial distortion with polyphony and ghost notes when releasing one of two keys. Rev addresses two separate problems:

### Audio engine

- 512-frame mono producer/consumer ring buffer.
- Synthesis remains outside the I2S ISR.
- Producer keeps the buffer substantially ahead of the consumer instead of rendering only a small fixed batch.
- The Cortex-M0+ synthesis hot path avoids 64-bit multiplication.
- The artificial "fast release when another key is held" behavior has been removed. A key release now changes only its own voice envelope.
- I2S remains 46,875 Hz with 3.000 MHz BCLK and two 32-bit slots.
- Underruns are counted internally.

### Key handling

TM1638 keys are now debounced independently per key. A key must be observed consistently for five scans before its gate changes.

A release affects only the voice mapped to that key. A simultaneous release/press cannot be converted into a different note by the release handler.

**Important:** if a second key still lights/appears pressed on the TM1638 after releasing the first key, that is an electrical/matrix-level ghost reported by the TM1638. Software debounce can reject short transients, but it cannot distinguish a real key from a persistent electrical ghost. If that occurs, the next diagnostic step should be to expose the raw TM1638 key bytes before changing the synth again.

## Audio clock

- GCLK0 = 48 MHz
- MCKDIV = 15
- BCLK/SCK = 3.000 MHz
- 2 × 32-bit slots
- LRCLK = 46,875 Hz
- BCLK/LRCLK = 64:1

## Hardware

| ATSAMD21E18A | Device |
|---|---|
| PA08 | MAX98357A DIN |
| PA10 | MAX98357A BCLK |
| PA11 | MAX98357A LRC |
| PA17 | Heartbeat LED |
| PA14 | Encoder A |
| PA15 | Encoder B |
| PA22 | Encoder push |
| PA16 | TM1638 STB |
| PA18 | TM1638 CLK |
| PA19 | TM1638 DIO |
| GND | Common ground |

## Testing

Test in this order:

1. One key held.
2. Two keys held.
3. Release the first key while keeping the second held.
4. Release the second key.
5. Repeat with three and four keys.

For the first test, use the ORG preset at a moderate volume.

If distortion remains with two notes, the next revision should instrument the audio-buffer underrun counter and verify the I2S TX timing rather than changing ADSR behavior again.

If the released key produces another note, observe whether the TM1638 LED for that released key also remains lit. That distinguishes input ghosting from an audio voice-management problem.

## Build/upload

```bash
pio run
pio run -t upload
```

ST-LINK V2/OpenOCD configuration is inherited unchanged from Rev3.
