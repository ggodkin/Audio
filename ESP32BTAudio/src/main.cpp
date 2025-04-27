#include <Arduino.h>
#include "BluetoothA2DPSink.h"

I2SStream i2s;
BluetoothA2DPSink a2dp_sink(i2s);

void setup() {
    // Start A2DP sink
    a2dp_sink.start("MyMusicWOpin");
}

void loop() {
}

// #include "AudioTools.h"
// #include "BluetoothA2DPSink.h"

// const int BUTTON = 13;          // Sensitive Touch
// const int BUTTON_PRESSED = 40;  // touch limit

// I2SStream i2s;
// BluetoothA2DPSink a2dp_sink(i2s);

// void setup() {
//   Serial.begin(115200);
//   a2dp_sink.activate_pin_code(true);
//   a2dp_sink.start("ReceiverWithPin", false);
// }

// void confirm() { a2dp_sink.confirm_pin_code(); }

// void loop() {
//   if (a2dp_sink.pin_code() != 0 && touchRead(BUTTON) < BUTTON_PRESSED) {
//     a2dp_sink.debounce(confirm, 5000);
//   }
// }