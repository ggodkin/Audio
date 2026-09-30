#pragma once

#include <stdint.h>

void audio_engine_initialize();
void audio_engine_fill(uint16_t max_frames);
void audio_engine_note_on(int note);
void audio_engine_note_off(int note);
bool audio_engine_adjust_volume(int delta);
bool audio_engine_step_preset(int delta);
int audio_engine_volume();
uint8_t audio_engine_preset_index();
uint32_t audio_engine_underruns();
