#include "AudioTools.h"
#include "BluetoothA2DPSink.h"

I2SStream i2s;
BluetoothA2DPSink a2dp_sink(i2s);

void setup() {
    auto cfg = i2s.defaultConfig();
    cfg.pin_bck = 14;   // MAX98357 BCLK pin
    cfg.pin_ws = 15;    // MAX98357 LRC pin
    cfg.pin_data = 22;  // MAX98357 DIN pin 
    i2s.begin(cfg);

    a2dp_sink.start("ESP32BT-I2S");
}

void loop() {
}