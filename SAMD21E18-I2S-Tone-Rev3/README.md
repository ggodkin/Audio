# SAMD21E18-I2S-Tone-Rev3

Bare ATSAMD21E18A I2S synthesizer for MAX98357A.

## Rev3: audio buffering

Rev2 correctly matched the software sample rate to the configured I2S clock at **46,875 Hz**, but the complete four-voice synth/mixer still ran inside the I2S TX interrupt. At 46.875 kHz there are only about 21.3 microseconds per audio frame, so polyphony could make the ISR miss a transmit deadline.

Rev3 separates audio rendering from I2S transport:

- A **512-frame mono ring buffer** is rendered by the main loop.
- The I2S ISR only fetches a pre-rendered sample and writes it to the serializer.
- The same sample is sent in both 32-bit stereo slots.
- The synth, ADSR, harmonics, and noise generation no longer execute inside the I2S ISR.
- The queue is pre-filled before I2S is enabled.
- An underrun counter records any case where the producer fails to keep the queue supplied.
- The existing fixed `/NUM_VOICES` mix scaling is retained so releasing one note does not create a level jump that sounds like a ghost note.

The SAM D21 I2S peripheral provides TX-ready interrupts and also supports DMA requests; DMA is a later optimization. Microchip documents I2S DMA specifically as a way to reduce processor intervention further.

## Audio clock

The existing clock is intentionally retained:

- GCLK0 = 48 MHz
- MCKDIV = 15
- BCLK/SCK = 3.000 MHz
- 2 slots × 32 bits
- LRCLK = **46,875 Hz**
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

## Synth

- 4 simultaneous voices
- 1024-entry interpolated sine table
- 32-bit phase accumulator
- ADSR presets
- Drum mode with filtered LFSR noise
- Rotary encoder for volume/preset selection
- TM1638 8-button input/display

## Testing

Start with one note, then two, three, and four notes held simultaneously. Then release one note at a time.

The expected result is that adding/removing polyphony does not introduce a burst of CPU-generated I2S noise or an audible level jump on the remaining notes.

The internal `audio_underruns` counter should remain at zero during normal operation. It is intentionally not displayed on the TM1638 yet; this can be exposed during the next diagnostic pass if needed.

## Build/upload

```bash
pio run
pio run -t upload
```

The ST-LINK V2/OpenOCD SWD upload configuration is unchanged from Rev2.
