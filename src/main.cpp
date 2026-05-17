#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

#define I2C_SDA 21
#define I2C_SCL 22

/* SH1106 128x64 I2C, full buffer mode, sprzetowy I2C
   (modul z etykieta SSD1306 ale faktycznie ma SH1106 w srodku) */
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /*reset=*/ U8X8_PIN_NONE);

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("BLE Finder boot");

    Wire.begin(I2C_SDA, I2C_SCL);

    u8g2.setI2CAddress(0x3C * 2);   /* U8g2 chce adres przesuniety o 1 bit w lewo */
    u8g2.begin();

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(0, 12, "Hello World");
    u8g2.drawStr(0, 28, "ESP32 + SSD1306");
    u8g2.drawStr(0, 44, "BLE Finder");
    u8g2.drawStr(0, 60, "setup OK");
    u8g2.sendBuffer();

    Serial.println("Display ready");
}

void loop() {
    delay(1000);
}
