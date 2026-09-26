#include <Arduino.h>

void setup()
{
    // Minimal reset/startup diagnostic.
    // No I2S object, library, DMA, or peripheral initialization is present.
    // If this LED turns on after both power-on and external RESET, the
    // Arduino/C++ startup path is functioning and we can return to I2S.
    PORT->Group[0].DIRSET.reg = PORT_PA17;
    PORT->Group[0].OUTSET.reg = PORT_PA17;
}

void loop()
{
    PORT->Group[0].OUTTGL.reg = PORT_PA17;
    delay(500);
}
