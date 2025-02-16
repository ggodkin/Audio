#include "AudioTools.h"
#include "BluetoothA2DPSink.h"

I2SStream i2s;
BluetoothA2DPSink a2dp_sink(i2s);

// bck_io_num = 14  //BCK pin
// ws_io_num = 15  //WS (LRC) pin
// data_out_num = 22  //SD (Data Out) pin NOOOO DIN!!!!

void setup() {
    a2dp_sink.start("ESP32BTSpeaker");
}

void loop() {
}