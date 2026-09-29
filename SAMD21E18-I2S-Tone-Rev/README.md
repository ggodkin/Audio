# SAMD21E18-I2S-Tone-Rev

Bare ATSAMD21E18A I2S synthesizer for MAX98357A.

## Current audio configuration

- I2S sample rate: 31,250 Hz
- GCLK0: 48 MHz
- MCKDIV: 23
- BCLK/SCK: 2.000 MHz
- 2 × 32-bit I2S slots
- BCLK/LRCLK: 64:1
- Four-voice polyphony test uses the real voice allocator, ADSR, mixer, ring buffer, and I2S path.
- The current polyphony test uses a lightweight sine oscillator in place of the instrument DSP so CPU headroom can be evaluated independently of the more expensive instrument algorithms.
- 31.25 kHz was selected after four voices at 46.875 kHz caused audio-buffer starvation/hiss; four voices now operate cleanly at the lower rate.

## Drum voices

The DRM preset uses eight drum voices:

| Key | Sound | Synthesis |
|---|---|---|
| S1 | Kick | 190→55 Hz downward sine sweep; no noise |
| S2 | Snare | Short sine body |
| S3 | Hi-hat | Inharmonic partials plus a small high-passed noise component |
| S4 | Clap | Short sine body |
| S5 | Low tom | Short sine body |
| S6 | Mid tom | Short sine body |
| S7 | Rim | Short sine body |
| S8 | Crash | Multiple inharmonic partials plus a stronger high-passed noise component |

S1 is intentionally noise-free. S3 and S8 are the only drum voices using the noise source, and their noise contribution is kept below the carrier to avoid excessive broadband hiss.

## Envelope system

ADSR timing is now defined in milliseconds rather than as raw per-sample increments. The firmware converts the requested times to fixed-point envelope rates using the current audio sample rate. This keeps the intended envelope timing stable if the I2S sample rate is changed.

The current presets are:

| Instrument | Attack | Decay | Sustain | Release |
|---|---:|---:|---:|---:|
| ORG | 2 ms | immediate | 100% | 2 s |
| PLK | 1 ms | 180 ms | 0% | 250 ms |
| PAD | 1 s | immediate | 100% | ~2 s |
| BRS | 150 ms | 500 ms → 50% | 50% | 1 s |
| PNO | 5 ms | ~2 s → 0% | 0% | 1 s |
| SAX | 70 ms | 300 ms → 75% | 75% | 700 ms |
| VLN | 120 ms | 500 ms → 75% | 75% | ~2 s |
| DRM | 1 ms | 150 ms → 0% | 0% | not normally used |

Long envelope times are limited by the current 16-bit fixed-point envelope representation: at 31.25 kHz, the smallest non-zero decrement is one fixed-point count per sample, which corresponds to about 2.1 seconds for a full-scale traversal. The long PAD/PNO/VLN timings therefore use approximately 2 seconds where necessary.

Drum voices with zero sustain become idle automatically when their decay reaches zero, so their release stage is normally not reached.

## Why Rev

This revision combines the stable four-voice audio path with sample-rate headroom and instrument-specific envelopes.

### Audio engine

- Producer/consumer audio ring buffer.
- Synthesis remains outside the I2S ISR.
- Producer keeps the buffer ahead of the consumer.
- The Cortex-M0+ synthesis hot path avoids unnecessary 64-bit operations.
- Four independent voices have their own oscillator/envelope state.
- A key release changes only its own voice envelope.
- Audio-buffer underruns are counted internally.

### Key handling

TM1638 keys are debounced independently per key. A key must be observed consistently before its gate changes.

A release affects only the voice mapped to that key. A simultaneous release/press cannot be converted into a different note by the release handler.

**Important:** if a second key still lights/appears pressed on the TM1638 after releasing the first key, that is an electrical/matrix-level ghost reported by the TM1638. Software debounce can reject short transients, but it cannot distinguish a real key from a persistent electrical ghost.

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

Test the four-voice path in this order:

1. One key held.
2. Two keys held.
3. Release the first key while keeping the second held.
4. Release the second key.
5. Repeat with three and four keys.
6. Repeat using the different instrument presets once the real instrument DSP is enabled.

For the current polyphony test, the instrument selector still changes the associated ADSR preset, but the oscillator is intentionally a lightweight sine wave. This makes it a CPU/polyphony test rather than a final timbral test.

If distortion appears again, check the audio-buffer underrun counter and I2S timing before changing ADSR behavior.

If a released key produces another note, observe whether the TM1638 indication for that released key also remains active. That helps distinguish input ghosting from audio voice-management problems.

## Build/upload

```bash
pio run
pio run -t upload
```

ST-LINK V2/OpenOCD configuration is inherited from the project PlatformIO configuration.
