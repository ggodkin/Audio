/*
 * ATSAMD21E18A → MAX98357A
 * I2S sine + rotary volume + PS/2 keyboard (note on/off)
 *
 * I2S:     PA08=DIN  PA10=BCLK  PA11=LRCLK
 * LED:     PA17
 * Encoder: PA14=A  PA15=B  (volume 0..64, start 1/32)
 * PS/2:    PA22=CLK  PA23=DATA
 *
 * PS/2 is 5 V open-collector — use a level shifter (or series
 * resistors + clamp) into the 3.3 V SAMD21 pins. Pull-ups to 3.3 V
 * on the MCU side are enabled in software.
 *
 * Key map (PS/2 set-2 make codes, white keys + a few blacks):
 *   A S D F G H J K  →  C4 D4 E4 F4 G4 A4 B4 C5
 *   W E T Y U        →  C# D# F# G# A#
 * Hold key = sound; release = silence (monophonic).
 */

#include <Arduino.h>
#include "sam.h"

static constexpr uint32_t SAMPLE_RATE_HZ = 46875;
static constexpr uint32_t SINE_LEN       = 64;
static constexpr int      VOLUME_MAX     = 64;

static const int32_t sine_table[SINE_LEN] = {
           0,  105245103,  209476638,  311690799,
   410903206,  506158392,  596538995,  681174601,
   759250124,  830013653,  892783697,  946955746,
   992008093, 1027506861, 1053110175, 1068571463,
  1073741823, 1068571463, 1053110175, 1027506861,
   992008093,  946955746,  892783697,  830013653,
   759250124,  681174601,  596538995,  506158392,
   410903206,  311690799,  209476638,  105245103,
           0, -105245103, -209476638, -311690799,
  -410903206, -506158392, -596538995, -681174601,
  -759250124, -830013653, -892783697, -946955746,
  -992008093,-1027506861,-1053110175,-1068571463,
 -1073741823,-1068571463,-1053110175,-1027506861,
  -992008093, -946955746, -892783697, -830013653,
  -759250124, -681174601, -596538995, -506158392,
  -410903206, -311690799, -209476638, -105245103
};

static uint32_t phase     = 0;
static uint32_t phase_inc = 0;

static volatile int     volume   = VOLUME_MAX / 32;  // 1/32 max
static volatile bool    gate_on  = false;            // key held
static volatile uint8_t active_sc = 0;               // scan code of held note

// ---------------------------------------------------------------------------
// Note table: scan code → frequency (Hz)
// ---------------------------------------------------------------------------
struct NoteMap {
    uint8_t  sc;
    uint16_t hz;
};

static const NoteMap note_map[] = {
    // Whites
    { 0x1C, 262 },  // A → C4
    { 0x1B, 294 },  // S → D4
    { 0x23, 330 },  // D → E4
    { 0x2B, 349 },  // F → F4
    { 0x34, 392 },  // G → G4
    { 0x33, 440 },  // H → A4
    { 0x3B, 494 },  // J → B4
    { 0x42, 523 },  // K → C5
    // Blacks
    { 0x1D, 277 },  // W → C#4
    { 0x24, 311 },  // E → D#4
    { 0x2C, 370 },  // T → F#4
    { 0x35, 415 },  // Y → G#4
    { 0x3C, 466 },  // U → A#4
};
static constexpr int NOTE_MAP_LEN = sizeof(note_map) / sizeof(note_map[0]);

static uint16_t sc_to_hz(uint8_t sc)
{
    for (int i = 0; i < NOTE_MAP_LEN; i++) {
        if (note_map[i].sc == sc) return note_map[i].hz;
    }
    return 0;
}

static void set_freq_hz(uint16_t hz)
{
    if (hz == 0) {
        phase_inc = 0;
        return;
    }
    phase_inc = (uint32_t)(((uint64_t)hz << 32) / SAMPLE_RATE_HZ);
}

static void note_on(uint8_t sc)
{
    uint16_t hz = sc_to_hz(sc);
    if (!hz) return;
    active_sc = sc;
    gate_on   = true;
    set_freq_hz(hz);
}

static void note_off(uint8_t sc)
{
    // Only silence if this is the key that is currently sounding
    if (sc == active_sc) {
        gate_on   = false;
        active_sc = 0;
        set_freq_hz(0);
    }
}

// ---------------------------------------------------------------------------
// LED (PA17)
// ---------------------------------------------------------------------------
static void led_init(void)
{
    PORT->Group[0].DIRSET.reg = PORT_PA17;
    PORT->Group[0].OUTCLR.reg = PORT_PA17;
}
static void led_on(void)     { PORT->Group[0].OUTSET.reg = PORT_PA17; }
static void led_off(void)    { PORT->Group[0].OUTCLR.reg = PORT_PA17; }
static void led_toggle(void) { PORT->Group[0].OUTTGL.reg = PORT_PA17; }

static void led_blink_n(int n, uint16_t on_ms = 80, uint16_t off_ms = 80)
{
    for (int i = 0; i < n; i++) {
        led_on();  delay(on_ms);
        led_off(); delay(off_ms);
    }
    delay(150);
}

// ---------------------------------------------------------------------------
// Rotary encoder PA14/PA15
// ---------------------------------------------------------------------------
static uint8_t enc_prev = 0;

static void encoder_init(void)
{
    PORT->Group[0].DIRCLR.reg = PORT_PA14 | PORT_PA15;
    PORT->Group[0].PINCFG[14].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].PINCFG[15].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].OUTSET.reg = PORT_PA14 | PORT_PA15;

    uint32_t in = PORT->Group[0].IN.reg;
    enc_prev = ((in & PORT_PA14) ? 1 : 0) | ((in & PORT_PA15) ? 2 : 0);
}

static void encoder_poll(void)
{
    static const int8_t table[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0
    };

    uint32_t in = PORT->Group[0].IN.reg;
    uint8_t curr = ((in & PORT_PA14) ? 1 : 0) | ((in & PORT_PA15) ? 2 : 0);
    int8_t delta = table[(enc_prev << 2) | curr];
    enc_prev = curr;

    if (delta) {
        int v = volume + delta;
        if (v < 0)          v = 0;
        if (v > VOLUME_MAX) v = VOLUME_MAX;
        volume = v;
    }
}

// ---------------------------------------------------------------------------
// PS/2 keyboard – PA22=CLK (EXTINT6), PA23=DATA
// Falling edge of CLK → sample DATA (LSB first)
// Frame: start, 8 data, parity, stop  (parity ignored for simplicity)
// ---------------------------------------------------------------------------
static volatile uint16_t ps2_shift  = 0;
static volatile uint8_t  ps2_bits   = 0;
static volatile uint8_t  ps2_byte   = 0;
static volatile bool     ps2_ready  = false;
static volatile bool     ps2_break  = false;  // saw 0xF0

static void ps2_on_byte(uint8_t b)
{
    if (b == 0xF0) {
        ps2_break = true;
        return;
    }
    // Ignore extended prefix for now (0xE0) – simple keys only
    if (b == 0xE0) return;

    if (ps2_break) {
        note_off(b);
        ps2_break = false;
    } else {
        note_on(b);
    }
}

// EIC interrupt – EXTINT[6] = PA22
extern "C" void EIC_Handler(void)
{
    if (EIC->INTFLAG.reg & (1u << 6)) {
        EIC->INTFLAG.reg = (1u << 6);   // clear

        // Sample DATA on PA23
        uint32_t bit = (PORT->Group[0].IN.reg & PORT_PA23) ? 1u : 0u;

        if (ps2_bits == 0) {
            // Expect start bit = 0
            if (bit == 0) {
                ps2_shift = 0;
                ps2_bits  = 1;
            }
        } else if (ps2_bits >= 1 && ps2_bits <= 8) {
            ps2_shift |= (bit << (ps2_bits - 1));
            ps2_bits++;
        } else if (ps2_bits == 9) {
            // parity – skip
            ps2_bits++;
        } else {
            // stop bit – accept byte
            ps2_byte  = (uint8_t)(ps2_shift & 0xFF);
            ps2_ready = true;
            ps2_bits  = 0;
        }
    }
}

static void ps2_init(void)
{
    // GPIO inputs + pull-ups (MCU side at 3.3 V)
    PORT->Group[0].DIRCLR.reg = PORT_PA22 | PORT_PA23;
    PORT->Group[0].PINCFG[22].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN | PORT_PINCFG_PMUXEN;
    PORT->Group[0].PINCFG[23].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].OUTSET.reg = PORT_PA22 | PORT_PA23;

    // PA22 → EXTINT6 (peripheral function A = 0)
    PORT->Group[0].PMUX[22 >> 1].bit.PMUXE = 0;  // even pin → PMUXE

    // GCLK for EIC (ID 5)
    GCLK->CLKCTRL.reg =
        GCLK_CLKCTRL_ID(5) |
        GCLK_CLKCTRL_GEN_GCLK0 |
        GCLK_CLKCTRL_CLKEN;
    while (GCLK->STATUS.bit.SYNCBUSY) {}

    PM->APBAMASK.reg |= PM_APBAMASK_EIC;

    EIC->CTRL.bit.SWRST = 1;
    while (EIC->STATUS.bit.SYNCBUSY) {}

    // Falling edge on EXTINT6
    EIC->CONFIG[0].reg &= ~(0xFu << 24);          // clear SENSE6
    EIC->CONFIG[0].reg |=  (0x2u << 24);          // 0x2 = FALL
    EIC->INTENSET.reg   =  (1u << 6);

    EIC->CTRL.bit.ENABLE = 1;
    while (EIC->STATUS.bit.SYNCBUSY) {}

    NVIC_EnableIRQ(EIC_IRQn);
}

static void ps2_poll(void)
{
    if (!ps2_ready) return;
    noInterrupts();
    uint8_t b = ps2_byte;
    ps2_ready = false;
    interrupts();
    ps2_on_byte(b);
}

// ---------------------------------------------------------------------------
// I2S
// ---------------------------------------------------------------------------
static void wait_gclk(void)
{
    for (uint32_t i = 0; i < 100000u && GCLK->STATUS.bit.SYNCBUSY; i++) {}
}

static void wait_i2s(uint32_t mask)
{
    for (uint32_t i = 0; i < 100000u && (I2S->SYNCBUSY.reg & mask); i++) {}
}

static void configure_i2s(void)
{
    PM->APBCMASK.reg |= PM_APBCMASK_I2S;

    GCLK->CLKCTRL.reg =
        GCLK_CLKCTRL_ID(35) |
        GCLK_CLKCTRL_GEN_GCLK0 |
        GCLK_CLKCTRL_CLKEN;
    wait_gclk();

    PORT->Group[0].PINCFG[8].reg  |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[4].bit.PMUXE = 0x6;

    PORT->Group[0].PINCFG[10].reg |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[5].bit.PMUXE = 0x6;

    PORT->Group[0].PINCFG[11].reg |= PORT_PINCFG_PMUXEN;
    PORT->Group[0].PMUX[5].bit.PMUXO = 0x6;

    I2S->CTRLA.reg = I2S_CTRLA_SWRST;
    wait_i2s(I2S_SYNCBUSY_SWRST);

    I2S->CLKCTRL[0].reg =
        I2S_CLKCTRL_SLOTSIZE(3) |
        I2S_CLKCTRL_NBSLOTS(1) |
        I2S_CLKCTRL_FSWIDTH_HALF |
        I2S_CLKCTRL_BITDELAY |
        I2S_CLKCTRL_FSSEL_SCKDIV |
        I2S_CLKCTRL_SCKSEL_MCKDIV |
        I2S_CLKCTRL_MCKSEL_GCLK |
        I2S_CLKCTRL_MCKEN |
        I2S_CLKCTRL_MCKDIV(15);

    I2S->SERCTRL[1].reg =
        I2S_SERCTRL_SERMODE_TX |
        I2S_SERCTRL_TXSAME |
        I2S_SERCTRL_SLOTADJ_LEFT |
        I2S_SERCTRL_DATASIZE_32 |
        I2S_SERCTRL_CLKSEL_CLK0;

    I2S->CTRLA.reg =
        I2S_CTRLA_ENABLE |
        I2S_CTRLA_CKEN0  |
        I2S_CTRLA_SEREN1;

    wait_i2s(I2S_SYNCBUSY_ENABLE |
             I2S_SYNCBUSY_CKEN0  |
             I2S_SYNCBUSY_SEREN1);
}

static bool i2s_write_stereo(int32_t sample)
{
    uint32_t t;

    t = 0;
    while (!(I2S->INTFLAG.bit.TXRDY1)) {
        if (++t > 200000u) return false;
    }
    I2S->DATA[1].reg = (uint32_t)sample;

    t = 0;
    while (!(I2S->INTFLAG.bit.TXRDY1)) {
        if (++t > 200000u) return false;
    }
    I2S->DATA[1].reg = (uint32_t)sample;

    return true;
}

// ---------------------------------------------------------------------------
void setup()
{
    led_init();
    led_blink_n(3, 50, 50);

    encoder_init();
    ps2_init();

    phase_inc = 0;   // silent until a key is pressed
    configure_i2s();

    led_on();
    delay(150);
}

void loop()
{
    static uint32_t sample_count = 0;

    encoder_poll();
    ps2_poll();

    uint32_t idx = phase >> 26;
    int32_t raw = sine_table[idx & (SINE_LEN - 1)];

    // Gate: only output when a key is held
    int32_t sample = 0;
    if (gate_on) {
        sample = (int32_t)(((int64_t)raw * volume) / VOLUME_MAX);
    }

    i2s_write_stereo(sample);

    phase += phase_inc;

    if (++sample_count >= (SAMPLE_RATE_HZ / 2)) {
        sample_count = 0;
        led_toggle();
    }
}
