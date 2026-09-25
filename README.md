# Audio

Embedded audio experiments and reusable digital-audio building blocks for microcontrollers.

## Current work

### SAMD21E18A I2S synthesizer

The first portable synthesizer target is an ATSAMD21E18A producing a 1 kHz tone over I2S for external audio devices.

See SAMD21E18-I2S-Synth/ for the bring-up project.

Planned MCU targets include:

- ATSAMD21
- ATSAMD51
- RP2040
- RP2350
- ESP32 family

The long-term goal is a common synthesizer/audio layer with small MCU-specific I2S backends.
