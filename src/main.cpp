#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define I2C_SDA             21
#define I2C_SCL             22
#define BTN_RESET           0     /* BOOT = wyczysc liste */
#define SCAN_CHUNK_S        2     /* dlugosc jednej "rundy" skanu (chain) */
#define MAX_SCAN_RESULTS    64
#define MAX_DISPLAY_DEVICES 6
#define ENTRY_TIMEOUT_MS    15000 /* po ilu ms bez nowej reklamy urzadzenie znika z listy */
#define DISPLAY_REFRESH_MS  400   /* co ile odswiezamy OLED */

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

struct ScanEntry {
    char     mac[18];
    int      rssi;
    char     name[24];
    uint32_t lastSeenMs;
};

static BLEScan          *bleScan = nullptr;
static ScanEntry         entries[MAX_SCAN_RESULTS];
static int               entryCount = 0;
static SemaphoreHandle_t entriesMutex;
static uint32_t          lastDisplayUpdate = 0;

/* ============== zarzadzanie lista (callback BLE + main task uzywaja mutexa) ============== */

static void addOrUpdateEntry(BLEAdvertisedDevice &d) {
    char mac[18];
    strncpy(mac, d.getAddress().toString().c_str(), 17);
    mac[17] = '\0';
    int rssi = d.getRSSI();
    bool hasName = d.haveName();

    xSemaphoreTake(entriesMutex, portMAX_DELAY);

    int idx = -1;
    for (int i = 0; i < entryCount; i++) {
        if (strcmp(entries[i].mac, mac) == 0) { idx = i; break; }
    }
    if (idx == -1) {
        if (entryCount >= MAX_SCAN_RESULTS) {
            xSemaphoreGive(entriesMutex);
            return;
        }
        idx = entryCount++;
        strcpy(entries[idx].mac, mac);
        entries[idx].name[0] = '\0';
    }
    entries[idx].rssi = rssi;
    entries[idx].lastSeenMs = millis();
    if (hasName) {
        strncpy(entries[idx].name, d.getName().c_str(), 23);
        entries[idx].name[23] = '\0';
    }

    xSemaphoreGive(entriesMutex);
}

static void ageOutStaleEntries() {
    uint32_t now = millis();
    xSemaphoreTake(entriesMutex, portMAX_DELAY);
    int w = 0;
    for (int r = 0; r < entryCount; r++) {
        if (now - entries[r].lastSeenMs < ENTRY_TIMEOUT_MS) {
            if (w != r) entries[w] = entries[r];
            w++;
        }
    }
    entryCount = w;
    xSemaphoreGive(entriesMutex);
}

static void clearAllEntries() {
    xSemaphoreTake(entriesMutex, portMAX_DELAY);
    entryCount = 0;
    xSemaphoreGive(entriesMutex);
}

/* kopia migawki + sortowanie (sortujemy poza mutexem zeby trzymac go jak najkrocej) */
static int snapshotAndSort(ScanEntry *out) {
    xSemaphoreTake(entriesMutex, portMAX_DELAY);
    int n = entryCount;
    memcpy(out, entries, sizeof(ScanEntry) * n);
    xSemaphoreGive(entriesMutex);

    for (int i = 1; i < n; i++) {
        ScanEntry key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < key.rssi) {
            out[j + 1] = out[j]; j--;
        }
        out[j + 1] = key;
    }
    return n;
}

/* ============== BLE callbacki ============== */

class ScanCallback : public BLEAdvertisedDeviceCallbacks {
public:
    void onResult(BLEAdvertisedDevice ad) override {
        addOrUpdateEntry(ad);
    }
};

/* gdy skok skanu sie konczy — laczymy nastepny, w nieskonczonosc */
static void onScanComplete(BLEScanResults results) {
    bleScan->clearResults();
    bleScan->start(SCAN_CHUNK_S, onScanComplete, false);
}

/* ============== OLED ============== */

static void showBootMsg(const char *msg) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(0, 12, "BLE Finder");
    u8g2.drawHLine(0, 15, 128);
    u8g2.drawStr(0, 36, msg);
    u8g2.sendBuffer();
}

static void renderList() {
    ScanEntry snap[MAX_SCAN_RESULTS];
    int n = snapshotAndSort(snap);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tf);

    char header[28];
    snprintf(header, sizeof(header), "BLE live  N=%d", n);
    u8g2.drawStr(0, 7, header);
    u8g2.drawHLine(0, 9, 128);

    if (n == 0) {
        u8g2.setFont(u8g2_font_6x12_tf);
        u8g2.drawStr(0, 36, "Searching...");
        u8g2.sendBuffer();
        return;
    }

    int rows = (n < MAX_DISPLAY_DEVICES) ? n : MAX_DISPLAY_DEVICES;
    char line[40];
    for (int i = 0; i < rows; i++) {
        const char *label;
        if (snap[i].name[0]) {
            label = snap[i].name;                /* nazwa jest = priorytet */
        } else {
            label = snap[i].mac + 3;             /* skrocony MAC: "BB:CC:DD:EE:FF" */
        }
        /* "1 LabelDoCzternastu %4d" — RSSI dosuniete do prawej */
        snprintf(line, sizeof(line), "%d %-14.14s %4d",
                 i + 1, label, snap[i].rssi);
        u8g2.drawStr(0, 18 + i * 8, line);
    }
    u8g2.sendBuffer();
}

/* ============== przycisk ============== */

static bool checkButtonPressed() {
    if (digitalRead(BTN_RESET) != LOW) return false;
    delay(30);
    if (digitalRead(BTN_RESET) != LOW) return false;
    while (digitalRead(BTN_RESET) == LOW) delay(10);
    delay(30);
    return true;
}

/* ============== Arduino ============== */

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("BLE Finder boot");

    pinMode(BTN_RESET, INPUT_PULLUP);

    Wire.begin(I2C_SDA, I2C_SCL);
    u8g2.setI2CAddress(0x3C * 2);
    u8g2.begin();
    showBootMsg("Init BLE...");

    entriesMutex = xSemaphoreCreateMutex();

    BLEDevice::init("");
    bleScan = BLEDevice::getScan();
    bleScan->setAdvertisedDeviceCallbacks(new ScanCallback(), /*wantDuplicates=*/true);
    bleScan->setActiveScan(true);
    bleScan->setInterval(100);
    bleScan->setWindow(99);
    bleScan->start(SCAN_CHUNK_S, onScanComplete, false);

    Serial.println("BLE scanning. BOOT = clear list.");
    showBootMsg("Searching...");
}

void loop() {
    if (checkButtonPressed()) {
        clearAllEntries();
        Serial.println("> List cleared.");
    }

    uint32_t now = millis();
    if (now - lastDisplayUpdate >= DISPLAY_REFRESH_MS) {
        ageOutStaleEntries();
        renderList();
        lastDisplayUpdate = now;
    }

    delay(20);
}
