# SAMD21E18-I2S-Tone-RCP

ATSAMD21E18A four-voice I2S synthesizer for the MAX98357A, combining the
buffered audio transport from Rev with the active instrument DSP and safer
note handling from the Tone project.

## Current audio configuration

- I2S sample rate: approximately 23,437.5 Hz
- GCLK0: 48 MHz
- MCKDIV: 31
- BCLK/SCK: 1.500 MHz
- 2 × 32-bit I2S slots
- BCLK/LRCLK: 64:1
- Four voices are rendered into a 1,024-frame producer/consumer buffer; the I2S interrupt only transfers completed samples.
- Full instrument DSP is enabled by default. Set `AUDIO_POLYPHONY_TEST` to `true` only to isolate transport and voice-allocation behavior during diagnosis.
- The lower sample rate provides more CPU time per sample for full-DSP polyphony; underrun behavior still needs verification on the target board.
- Instrument-specific wavetable storage is limited to 512 samples to leave SRAM headroom for the audio buffer and runtime stack.

## Drum voices

The DRM preset uses eight drum voices:

| Key | Sound | Synthesis |
|---|---|---|
| S1 | Kick | 190→55 Hz downward sine sweep over 80 ms |
| S2 | Snare | Short sine body |
| S3 | Hi-hat | Band-limited noise with quiet inharmonic metallic partials |
| S4 | Clap | Short sine body |
| S5 | Low tom | Short sine body |
| S6 | Mid tom | Short sine body |
| S7 | Rim | Short sine body |
| S8 | Crash | Inharmonic metallic partials |

The hi-hat noise has its own 220 ms tail after the 115 ms metallic body decays.
The crash is oscillator-only; the kick is a clean sine pitch sweep.

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

Long envelope times are limited by the fixed-point rate resolution: at this sample rate, the smallest non-zero decrement is one count per sample, or about 2.8 seconds for a full-scale traversal. The long PAD/PNO/VLN timings therefore use approximately 2 seconds where necessary.

Drum voices with zero sustain become idle automatically when their decay reaches zero, so their release stage is normally not reached.

## What RCP Combines

RCP uses a more conservative sample rate and Rev's buffered audio transport while enabling
the full organ, saxophone, violin, and drum synthesis paths by default.

### Audio engine

- Producer/consumer audio ring buffer.
- Synthesis remains outside the I2S ISR.
- Producer keeps the buffer ahead of the consumer.
- The Cortex-M0+ synthesis hot path avoids unnecessary 64-bit operations.
- A 512-sample auxiliary wavetable size keeps full-DSP SRAM use below two-thirds of the SAMD21's available RAM.
- Four independent voices have their own oscillator/envelope state.
- Repeated scans of an already-held key do not retrigger its envelope.
- Releasing a key changes only that voice's envelope.
- PA17 turns off after the first audio-buffer underrun, making starvation visible during polyphony tests.

ADSR rates are specified in milliseconds and converted using the configured
sample rate. The organ uses drawbar-style harmonics, vibrato, tremolo, and a
short key-click transient; SAX and VLN use separate harmonic waveforms.

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
| PA17 | Status LED (turns off on audio-buffer underrun) |
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
6. Repeat using each instrument preset.

If the PA17 LED turns off or distortion appears, the audio producer may be falling behind. Check CPU load and I2S timing before changing ADSR behavior.

If a released key produces another note, observe whether the TM1638 indication for that released key also remains active. That helps distinguish input ghosting from audio voice-management problems.

### Build and upload

```bash
cd SAMD21E18-I2S-Tone-RCP
pio run
pio run -t upload
```

Upload uses the project's ST-LINK V2/OpenOCD configuration. The I2S pins,
TM1638 wiring, encoder, and display layout are the same as the Tone and Rev
projects.
