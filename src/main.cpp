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
    char     name[24];   /* prawdziwa nazwa BLE jesli urzadzenie ja nadaje */
    char     vendor[16]; /* domyslana etykieta z manufacturer data */
    uint32_t lastSeenMs;
};

/* Mapowanie Bluetooth SIG Company ID -> czytelna etykieta.
   Pelna lista: https://www.bluetooth.com/specifications/assigned-numbers/ */
static const char *guessVendor(uint16_t cid, const uint8_t *data, size_t len) {
    /* Apple ma wlasny "Continuity" protokol — pierwszy bajt po company ID to typ */
    if (cid == 0x004C && len >= 3) {
        uint8_t type = data[2];
        if (type == 0x02 && len >= 4 && data[3] == 0x15) return "iBeacon";
        if (type == 0x07) return "AirPods";
        if (type == 0x09) return "AirPrint";
        if (type == 0x10) return "Apple nearby";
        if (type == 0x12) return "Find My";
        if (type == 0x16) return "Find My";
        return "Apple";
    }
    switch (cid) {
        case 0x0006: return "Microsoft";
        case 0x00E0: return "Google";
        case 0x0075: return "Samsung";
        case 0x0087: return "Garmin";
        case 0x038F: return "Xiaomi";
        case 0x0157: return "Mi Band";
        case 0x0499: return "RuuviTag";
        case 0x05A7: return "Sonos";
        case 0x004F: return "Tile";
        case 0x0059: return "Nordic";
        case 0x000D: return "TI";
        case 0x000F: return "Broadcom";
        case 0x0001: return "Ericsson";
        case 0x0002: return "Intel";
        case 0x0046: return "Sony";
        case 0x0131: return "Cypress";
        case 0x0118: return "Withings";
        case 0x009E: return "Bose";
        case 0x0171: return "Amazon";
        default: return nullptr;
    }
}

static BLEScan          *bleScan = nullptr;
static ScanEntry         entries[MAX_SCAN_RESULTS];
static int               entryCount = 0;
static SemaphoreHandle_t entriesMutex;
static uint32_t          lastDisplayUpdate = 0;

/* ============== zarzadzanie lista (callback BLE + main task uzywaja mutexa) ============== */

static void addOrUpdateEntry(BLEAdvertisedDevice &d) {
    /* wyciagamy dane PRZED zajeciem mutexa, zeby trzymac go jak najkrocej */
    char mac[18];
    strncpy(mac, d.getAddress().toString().c_str(), 17);
    mac[17] = '\0';
    int rssi = d.getRSSI();

    char nameBuf[24];
    nameBuf[0] = '\0';
    if (d.haveName()) {
        strncpy(nameBuf, d.getName().c_str(), 23);
        nameBuf[23] = '\0';
    }

    char vendorBuf[16];
    vendorBuf[0] = '\0';
    if (d.haveManufacturerData()) {
        std::string md = d.getManufacturerData();
        if (md.size() >= 2) {
            uint16_t cid = (uint8_t)md[0] | ((uint8_t)md[1] << 8);
            const char *v = guessVendor(cid, (const uint8_t *)md.data(), md.size());
            if (v) {
                strncpy(vendorBuf, v, 15);
                vendorBuf[15] = '\0';
            }
        }
    }

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
        entries[idx].name[0]   = '\0';
        entries[idx].vendor[0] = '\0';
    }
    entries[idx].rssi = rssi;
    entries[idx].lastSeenMs = millis();
    if (nameBuf[0])   strcpy(entries[idx].name,   nameBuf);
    if (vendorBuf[0]) strcpy(entries[idx].vendor, vendorBuf);

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
            label = snap[i].name;          /* 1. prawdziwa nazwa BLE */
        } else if (snap[i].vendor[0]) {
            label = snap[i].vendor;        /* 2. domyslany producent */
        } else {
            label = snap[i].mac + 3;       /* 3. skrocony MAC "BB:CC:DD:EE:FF" */
        }
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
