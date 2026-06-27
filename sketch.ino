#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include "esp_bt.h"
#include "esp_wifi.h"
#include "esp_sleep.h"
#include "esp_ota_ops.h"

// ==========================================
// 1. LOVYANGFX HARDWARE-KONFIGURATION
// ==========================================
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7796  _panel_instance;
    lgfx::Bus_SPI       _bus_instance;
public:
    LGFX(void) {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read  = 16000000;
            cfg.pin_sclk = 12;
            cfg.pin_mosi = 11;
            cfg.pin_miso = 13;
            cfg.pin_dc   = 4;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = 10;
            cfg.pin_rst          = 5;
            cfg.panel_width      = 320;
            cfg.panel_height     = 480;
            cfg.offset_rotation  = 1;
            _panel_instance.config(cfg);
        }
        setPanel(&_panel_instance);
    }
};

LGFX tft;

// ==========================================
// 2. GLOBALE DEFINITIONEN & BLE VARIABLEN
// ==========================================
const int SD_CS_PIN = 9;
const int BACKLIGHT_PIN = 3;
const uint8_t CARDKB_ADDR = 0x5F;

// BLE UUIDs (Nordic UART Profil)
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define RX_UUID                "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define TX_UUID                "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLECharacteristic *pTxCharacteristic;
bool deviceConnected = false;
String bleRxBuffer = "";

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) { deviceConnected = true; };
    void onDisconnect(BLEServer* pServer) { deviceConnected = false; }
};

class MyCallbacks: public BLECharacteristicCallbacks {
void onWrite(BLECharacteristic *pCharacteristic) {
        String rxValue = pCharacteristic->getValue();
        if (rxValue.length() > 0) {
            bleRxBuffer += rxValue;
        }
    }
};

struct SystemSettings {
    uint16_t themeColor = TFT_GREEN;
    uint8_t brightness = 70;
    int frequency = 240;
    bool eco = false;
    bool flightMode = false;
    bool bluetoothActive = false;
    String bootphrase = "Welcome to picobook";
} settings;

std::vector<String> appList;
int selectedAppIndex = 0; 
bool redrawRequired = true;

// ==========================================
// 3. HARDWARE TREIBER (KEYBOARD & BLE SCAN)
// ==========================================
String readKeyboardChar() {
    Wire.requestFrom(CARDKB_ADDR, (uint8_t)1);
    if (Wire.available()) {
        uint8_t c = Wire.read();
        if (c == 0) return "";
        
        // Übersetzung von Steuerzeichen für die Python API
        if (c == 0x04) return "LEFT";
        if (c == 0x05) return "RIGHT";
        if (c == 0x12) return "UP";
        if (c == 0x13) return "DOWN";
        if (c == 13)   return "ENTER";
        if (c == 27)   return "ESC";
        if (c == 9)    return "TAB";
        
        return String((char)c);
    }
    return "";
}

void initBluetooth() {
    if (!settings.bluetoothActive) return;
    BLEDevice::init("picobook-OS");
    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService *pService = pServer->createService(SERVICE_UUID);
    pTxCharacteristic = pService->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    pTxCharacteristic->addDescriptor(new BLE2902());
    BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(RX_UUID, BLECharacteristic::PROPERTY_WRITE);
    pRxCharacteristic->setCallbacks(new MyCallbacks());
    pService->start();
    pServer->getAdvertising()->start();
}

// ==========================================
// 4. SPEICHER- & SYSTEM-LOGIK
// ==========================================
void loadSettings() {
    File file = SD.open("/files/config/settings.txt", FILE_READ);
    if (!file) return;
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.startsWith("color:")) settings.themeColor = (uint16_t)strtol(line.substring(6).c_str(), NULL, 16);
        else if (line.startsWith("boot:")) settings.bootphrase = line.substring(5);
        else if (line.startsWith("bluetooth:")) settings.bluetoothActive = (line.substring(10) == "true");
    }
    file.close();
}

void loadAndSortApps() {
    appList.clear();
    File root = SD.open("/apps");
    if (!root) return;
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory() && String(file.name()).endsWith(".py") && String(file.name()) != "picobook_os.py") {
            appList.push_back(String(file.name()));
        }
        file = root.openNextFile();
    }
    root.close();
    std::sort(appList.begin(), appList.end());
}

void drawDesktop() {
    tft.fillScreen(TFT_BLACK);
    tft.fillRect(0, 0, 480, 24, settings.themeColor);
    tft.setTextColor(TFT_BLACK);
    tft.drawString(" picobook OS v1.0", 5, 5);
    
    int startY = 60;
    for (size_t i = 0; i < appList.size(); i++) {
        if ((int)i == selectedAppIndex) {
            tft.fillRect(15, startY + (i * 25) - 2, 450, 22, settings.themeColor);
            tft.setTextColor(TFT_BLACK);
        } else {
            tft.setTextColor(TFT_WHITE);
        }
        tft.drawString(appList[i], 25, startY + (i * 25));
    }
    redrawRequired = false;
}

void launchPythonApp(const String& appName) {
    File check = SD.open("/files/config/next_app.txt", FILE_WRITE);
    if (check) {
        check.print("/apps/" + appName);
        check.close();
    }
    
    const esp_partition_t* py_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "py_core");
    if (py_part != NULL) {
        SD.end();
        SPI.end();
        Wire.end();
        esp_ota_set_boot_partition(py_part);
        esp_restart();
    }
}

void setup() {
    Serial.begin(115200);
    Wire.begin(1, 2, 400000U);
    SPI.begin(12, 13, 11, 10);
    
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    
    if (!SD.begin(SD_CS_PIN, SPI, 40000000)) {
        tft.setTextColor(TFT_RED);
        tft.drawString("CRITICAL ERROR: NO SD CARD!", 50, 100);
        while (true) vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    loadSettings();
    loadAndSortApps();
    initBluetooth();
    
    pinMode(BACKLIGHT_PIN, OUTPUT);
    analogWrite(BACKLIGHT_PIN, map(settings.brightness, 0, 100, 0, 255));
}

void loop() {
    if (redrawRequired) drawDesktop();
    
    String key = readKeyboardChar();
    if (key != "") {
        int totalItems = appList.size();
        if (key == "DOWN" || key == "s") {
            selectedAppIndex = (selectedAppIndex + 1) % totalItems;
            redrawRequired = true;
        } else if (key == "UP" || key == "w") {
            selectedAppIndex = (selectedAppIndex - 1 + totalItems) % totalItems;
            redrawRequired = true;
        } else if (key == "ENTER") {
            launchPythonApp(appList[selectedAppIndex]);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
}
