#include <Arduino.h>
#include "sam.h"
#include "audio_engine.h"
#include "controls.h"

namespace {
static constexpr int PRESET_EDGES = 4;
static const uint8_t phys_bit[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static const uint8_t SEG_DIGIT[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};
static constexpr uint8_t SEG_BLANK = 0x00;
static constexpr uint8_t SEG_A = 0x77;
static constexpr uint8_t SEG_B = 0x7C;
static constexpr uint8_t SEG_D = 0x5E;
static constexpr uint8_t SEG_G = 0x3D;
static constexpr uint8_t SEG_K = 0x75;
static constexpr uint8_t SEG_L = 0x38;
static constexpr uint8_t SEG_M = 0x37;
static constexpr uint8_t SEG_N = 0x54;
static constexpr uint8_t SEG_O = 0x3F;
static constexpr uint8_t SEG_P = 0x73;
static constexpr uint8_t SEG_R = 0x50;
static constexpr uint8_t SEG_S = 0x6D;
static constexpr uint8_t SEG_V = 0x3E;
static constexpr uint8_t SEG_X = 0x76;
static const uint8_t PRESET_LABELS[8][4] = {
    { SEG_O, SEG_R, SEG_G, SEG_BLANK },
    { SEG_P, SEG_L, SEG_K, SEG_BLANK },
    { SEG_P, SEG_A, SEG_D, SEG_BLANK },
    { SEG_B, SEG_R, SEG_S, SEG_BLANK },
    { SEG_P, SEG_N, SEG_O, SEG_BLANK },
    { SEG_S, SEG_A, SEG_X, SEG_BLANK },
    { SEG_V, SEG_L, SEG_N, SEG_BLANK },
    { SEG_D, SEG_R, SEG_M, SEG_BLANK }
};
enum UiMode : uint8_t { MODE_VOLUME = 0, MODE_PRESET = 1 };
static volatile uint8_t ui_mode = MODE_VOLUME;

static void led_init(void)
{
    PORT->Group[0].DIRSET.reg = PORT_PA17;
    PORT->Group[0].OUTCLR.reg = PORT_PA17;
}
static void led_on(void)  { PORT->Group[0].OUTSET.reg = PORT_PA17; }
static void led_off(void) { PORT->Group[0].OUTCLR.reg = PORT_PA17; }

static void led_blink_n(int n, uint16_t on_ms = 80, uint16_t off_ms = 80)
{
    for (int i = 0; i < n; i++) {
        led_on();  delay(on_ms);
        led_off(); delay(off_ms);
    }
    delay(150);
}

static uint8_t enc_prev = 0;
static bool    sw_prev  = true;
static int8_t  preset_accum = 0;

static void encoder_init(void)
{
    PORT->Group[0].DIRCLR.reg = PORT_PA14 | PORT_PA15 | PORT_PA22;
    PORT->Group[0].PINCFG[14].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].PINCFG[15].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].PINCFG[22].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].OUTSET.reg = PORT_PA14 | PORT_PA15 | PORT_PA22;

    uint32_t in = PORT->Group[0].IN.reg;
    enc_prev = ((in & PORT_PA14) ? 1 : 0) | ((in & PORT_PA15) ? 2 : 0);
    sw_prev  = (in & PORT_PA22) != 0;
}

static bool encoder_poll(void)
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

    bool changed = false;

    if (delta) {
        if (ui_mode == MODE_VOLUME) {
            changed = audio_engine_adjust_volume(delta);
        } else {
            // Accumulate edges; one preset step per detent
            preset_accum += delta;
            if (preset_accum >= PRESET_EDGES || preset_accum <= -PRESET_EDGES) {
                int step = (preset_accum > 0) ? 1 : -1;
                preset_accum = 0;
                changed = audio_engine_step_preset(step) || changed;
            }
        }
    }

    bool sw = (in & PORT_PA22) != 0;
    if (sw_prev && !sw) {
        ui_mode = (ui_mode == MODE_VOLUME) ? MODE_PRESET : MODE_VOLUME;
        preset_accum = 0;
        changed = true;
    }
    sw_prev = sw;

    return changed;
}

static void tm_delay(void)
{
    for (volatile int i = 0; i < 8; i++) {}
}

static void tm_stb_low(void)  { PORT->Group[0].OUTCLR.reg = PORT_PA16; }
static void tm_stb_high(void) { PORT->Group[0].OUTSET.reg = PORT_PA16; }
static void tm_clk_low(void)  { PORT->Group[0].OUTCLR.reg = PORT_PA18; }
static void tm_clk_high(void) { PORT->Group[0].OUTSET.reg = PORT_PA18; }
static void tm_dio_out(void)  { PORT->Group[0].DIRSET.reg = PORT_PA19; }

static void tm_dio_in(void)
{
    PORT->Group[0].DIRCLR.reg = PORT_PA19;
    PORT->Group[0].PINCFG[19].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
    PORT->Group[0].OUTSET.reg = PORT_PA19;
}

static void tm_dio_write(bool v)
{
    if (v) PORT->Group[0].OUTSET.reg = PORT_PA19;
    else   PORT->Group[0].OUTCLR.reg = PORT_PA19;
}

static bool tm_dio_read(void)
{
    return (PORT->Group[0].IN.reg & PORT_PA19) != 0;
}

static void tm_write_byte(uint8_t data)
{
    tm_dio_out();
    for (int i = 0; i < 8; i++) {
        tm_clk_low();
        tm_dio_write(data & 0x01);
        tm_delay();
        tm_clk_high();
        tm_delay();
        data >>= 1;
    }
}

static uint8_t tm_read_byte(void)
{
    uint8_t data = 0;
    tm_dio_in();
    for (int i = 0; i < 8; i++) {
        data >>= 1;
        tm_clk_low();
        tm_delay();
        if (tm_dio_read()) data |= 0x80;
        tm_clk_high();
        tm_delay();
    }
    return data;
}

static void tm_cmd(uint8_t cmd)
{
    tm_stb_low();
    tm_write_byte(cmd);
    tm_stb_high();
}

static void tm_write_digit(uint8_t pos, uint8_t seg)
{
    tm_stb_low();
    tm_write_byte(0xC0 + (pos * 2));
    tm_write_byte(seg);
    tm_stb_high();
}

static void tm_show_ui(void)
{
    if (ui_mode == MODE_VOLUME) {
        tm_write_digit(0, SEG_V);
        tm_write_digit(1, SEG_O);
        tm_write_digit(2, SEG_L);
        tm_write_digit(3, SEG_BLANK);

        int vol = audio_engine_volume();
        tm_write_digit(4, (vol >= 1000) ? SEG_DIGIT[(vol / 1000) % 10] : SEG_BLANK);
        tm_write_digit(5, (vol >= 100)  ? SEG_DIGIT[(vol / 100) % 10]  : SEG_BLANK);
        tm_write_digit(6, (vol >= 10)   ? SEG_DIGIT[(vol / 10) % 10]   : SEG_BLANK);
        tm_write_digit(7, SEG_DIGIT[vol % 10]);
    } else {
        const uint8_t *lab = PRESET_LABELS[audio_engine_preset_index()];
        tm_write_digit(0, lab[0]);
        tm_write_digit(1, lab[1]);
        tm_write_digit(2, lab[2]);
        tm_write_digit(3, lab[3]);

        int n = audio_engine_preset_index() + 1;
        tm_write_digit(4, SEG_BLANK);
        tm_write_digit(5, SEG_BLANK);
        tm_write_digit(6, SEG_BLANK);
        tm_write_digit(7, SEG_DIGIT[n % 10]);
    }
}

static void tm_init(void)
{
    PORT->Group[0].DIRSET.reg = PORT_PA16 | PORT_PA18 | PORT_PA19;
    PORT->Group[0].PINCFG[16].reg = 0;
    PORT->Group[0].PINCFG[18].reg = 0;
    PORT->Group[0].PINCFG[19].reg = 0;
    tm_stb_high();
    tm_clk_high();
    tm_dio_write(true);

    tm_cmd(0x8F);
    tm_cmd(0x40);

    tm_stb_low();
    tm_write_byte(0xC0);
    for (int i = 0; i < 16; i++)
        tm_write_byte(0x00);
    tm_stb_high();

    tm_show_ui();
}

static void tm_set_leds(uint8_t note_mask)
{
    for (int n = 0; n < 8; n++) {
        uint8_t bit = phys_bit[n];
        tm_stb_low();
        tm_write_byte(0xC1 + (bit * 2));
        tm_write_byte((note_mask & (1u << n)) ? 0x01 : 0x00);
        tm_stb_high();
    }
}

static uint8_t tm_read_keys_raw(void)
{
    uint8_t raw[4];
    tm_stb_low();
    tm_write_byte(0x42);
    for (int i = 0; i < 4; i++)
        raw[i] = tm_read_byte();
    tm_stb_high();
    tm_dio_out();

    uint8_t keys = 0;
    if (raw[0] & 0x01) keys |= (1u << 0);
    if (raw[1] & 0x01) keys |= (1u << 1);
    if (raw[2] & 0x01) keys |= (1u << 2);
    if (raw[3] & 0x01) keys |= (1u << 3);
    if (raw[0] & 0x10) keys |= (1u << 4);
    if (raw[1] & 0x10) keys |= (1u << 5);
    if (raw[2] & 0x10) keys |= (1u << 6);
    if (raw[3] & 0x10) keys |= (1u << 7);
    return keys;
}

static uint8_t keys_physical(uint8_t raw)
{
    uint8_t m = 0;
    for (int n = 0; n < 8; n++) {
        if (raw & (1u << phys_bit[n]))
            m |= (1u << n);
    }
    return m;
}

static void tm_poll(void)
{
    // Debounce each key independently. A key must be observed consistently
    // for five consecutive scans before its gate changes.
    static constexpr uint8_t KEY_THRESH = 5;
    static uint8_t counters[8] = {};
    static uint8_t debounced = 0;

    uint8_t raw = keys_physical(tm_read_keys_raw());

    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(1u << i);

        if (raw & bit) {
            if (counters[i] < KEY_THRESH)
                counters[i]++;

            if (counters[i] == KEY_THRESH && !(debounced & bit)) {
                debounced |= bit;
                audio_engine_note_on(i);
            }
        } else {
            if (counters[i] > 0)
                counters[i]--;

            if (counters[i] == 0 && (debounced & bit)) {
                debounced &= (uint8_t)~bit;
                audio_engine_note_off(i);
            }
        }
    }

    static uint8_t displayed = 0xFF;
    if (debounced != displayed) {
        tm_set_leds(debounced);
        displayed = debounced;
    }
}


}

void controls_initialize()
{
    led_init();
    led_blink_n(3, 50, 50);
    encoder_init();
    tm_init();
}

void controls_audio_started()
{
    led_on();
    delay(100);
}

bool controls_poll_encoder()
{
    return encoder_poll();
}

void controls_poll_keys()
{
    tm_poll();
}

void controls_show_ui()
{
    tm_show_ui();
}

void controls_update_audio_status()
{
    static uint32_t last_underruns = 0;
    const uint32_t underruns = audio_engine_underruns();
    if (underruns != last_underruns) {
        led_off();
        last_underruns = underruns;
    }
}
