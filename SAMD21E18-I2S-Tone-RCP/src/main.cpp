#include <Arduino.h>
#include "audio_engine.h"
#include "controls.h"

void setup()
{
    controls_initialize();
    audio_engine_initialize();
    controls_audio_started();
}

void loop()
{
    bool changed = false;
    for (int i = 0; i < 64; i++) {
        if (controls_poll_encoder())
            changed = true;
        audio_engine_fill(64);
    }

    controls_poll_keys();
    controls_update_audio_status();
    if (changed)
        controls_show_ui();
}
