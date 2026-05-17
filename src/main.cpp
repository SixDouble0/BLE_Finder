#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define I2C_SDA         21
#define I2C_SCL         22
#define SCAN_DURATION_S 5

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

static BLEScan *bleScan = nullptr;
static uint32_t scanCounter = 0;

static void showStatus(const char *line1, const char *line2 = nullptr) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(0, 12, "BLE Finder");
    u8g2.drawHLine(0, 15, 128);
    u8g2.drawStr(0, 32, line1);
    if (line2) u8g2.drawStr(0, 48, line2);
    u8g2.sendBuffer();
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("BLE Finder boot");

    /* OLED */
    Wire.begin(I2C_SDA, I2C_SCL);
    u8g2.setI2CAddress(0x3C * 2);
    u8g2.begin();
    showStatus("Init BLE...");

    /* BLE init */
    BLEDevice::init("");
    bleScan = BLEDevice::getScan();
    bleScan->setActiveScan(true);          /* active = pyta o nazwe urzadzenia */
    bleScan->setInterval(100);
    bleScan->setWindow(99);

    Serial.println("BLE ready");
    showStatus("Ready", "scanning soon");
    delay(500);
}

void loop() {
    scanCounter++;
    Serial.printf("\n--- Scan #%lu (duration %ds) ---\n",
                  (unsigned long)scanCounter, SCAN_DURATION_S);

    char l1[24], l2[24];
    snprintf(l1, sizeof(l1), "Scan #%lu ...", (unsigned long)scanCounter);
    showStatus(l1, "please wait");

    BLEScanResults results = bleScan->start(SCAN_DURATION_S, false);
    int count = results.getCount();

    for (int i = 0; i < count; i++) {
        BLEAdvertisedDevice d = results.getDevice(i);
        const char *name = d.haveName() ? d.getName().c_str() : "(no name)";
        Serial.printf("  [%2d] %s  RSSI=%d dBm  %s\n",
                      i,
                      d.getAddress().toString().c_str(),
                      d.getRSSI(),
                      name);
    }
    Serial.printf("Total: %d device(s)\n", count);

    snprintf(l1, sizeof(l1), "Scan #%lu done", (unsigned long)scanCounter);
    snprintf(l2, sizeof(l2), "Found: %d", count);
    showStatus(l1, l2);

    bleScan->clearResults();   /* zwalnia pamiec */
    delay(2000);
}
