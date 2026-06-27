#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
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
            cfg.spi_host = SPI2_HOST;     // ESP32-S3 SPI2 Bus
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;    // 40 MHz Schreibgeschwindigkeit
            cfg.freq_read  = 16000000;
            cfg.pin_sclk = 12;            // SuperMini SCLK Pin
            cfg.pin_mosi = 11;            // SuperMini MOSI Pin
            cfg.pin_miso = 13;            // SuperMini MISO Pin
            cfg.pin_dc   = 4;             // Data/Command Pin
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = 10;    // Chip Select Pin
            cfg.pin_rst          = 5;     // Reset Pin
            cfg.panel_width      = 320;   // Physikalische Breite
            cfg.panel_height     = 480;   // Physikalische Höhe
            cfg.offset_rotation  = 1;     // Querformat-Korrektur
            _panel_instance.config(cfg);
        }
        setPanel(&_panel_instance);
    }
};

LGFX tft;

// ==========================================
// 2. GLOBALE SYSTEM-VARIABLEN & STRUKTUREN
// ==========================================
const int SD_CS_PIN = 9;
const int BACKLIGHT_PIN = 3;
const uint8_t CARDKB_ADDR = 0x5F;

struct SystemSettings {
    uint16_t themeColor = TFT_GREEN;
    uint8_t brightness = 70;
    int frequency = 240;
    bool eco = false;
    bool flightMode = false;
    String bootphrase = "Welcome to picobook";
} settings;

struct TemporaryEcoStates {
    uint8_t originalBrightness = 70;
    int originalFreq = 240;
} backupStates;

std::vector<String> appList;
int selectedAppIndex = 0; 
bool redrawRequired = true;

// Prototypen
void setWirelessTransmitters(bool turnOn);
void applyEcoMode(bool enable);
void applyFlightMode(bool enable);
void prepareHardwareForShutdown();
void executeSafeShutdown();
void executeSafeReboot();

// ==========================================
// 3. I2C-TASTATURTREIBER (CardKB)
// ==========================================
char readKeyboard() {
    Wire.requestFrom(CARDKB_ADDR, (uint8_t)1);
    if (Wire.available()) {
        char c = Wire.read();
        if (c != 0) return c;
    }
    return 0;
}

// ==========================================
// 4. SETTINGS & APP-DATEIVERWALTUNG
// ==========================================
void loadSettings() {
    File file = SD.open("/files/config/settings.txt", FILE_READ);
    if (!file) {
        log_w("settings.txt nicht gefunden. Erstelle Defaults...");
        return;
    }
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.startsWith("color:")) {
            settings.themeColor = (uint16_t)strtol(line.substring(6).c_str(), NULL, 16);
        } else if (line.startsWith("boot:")) {
            settings.bootphrase = line.substring(5);
        } else if (line.startsWith("eco:")) {
            settings.eco = (line.substring(4) == "true");
        }
    }
    file.close();
}

void loadAndSortApps() {
    appList.clear();
    File root = SD.open("/apps");
    if (!root || !root.isDirectory()) {
        log_e("Ordner /apps fehlt auf der SD-Karte!");
        return;
    }
    
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory() && String(file.name()).endsWith(".py")) {
            appList.push_back(String(file.name()));
        }
        file = root.openNextFile();
    }
    root.close();
    std::sort(appList.begin(), appList.end());
}

// ==========================================
// 5. GUI-DESKTOP RENDERING
// ==========================================
void drawDesktop() {
    tft.fillScreen(TFT_BLACK);
    
    // Top-Bar (System-Status)
    tft.fillRect(0, 0, 480, 24, settings.themeColor);
    tft.setTextColor(TFT_BLACK);
    tft.drawString(" picobook OS v1.0", 5, 5);
    
    if (settings.eco) tft.drawString("[ECO]", 380, 5);
    if (settings.flightMode) tft.drawString("[FLUG]", 430, 5);
    
    // App-Liste ausgeben
    tft.setTextColor(TFT_WHITE);
    tft.drawString("--- INSTALLIERTE APPS ---", 20, 50);
    
    int startY = 80;
    for (size_t i = 0; i < appList.size(); i++) {
        if ((int)i == selectedAppIndex) {
            tft.fillRect(15, startY + (i * 25) - 2, 450, 22, settings.themeColor);
            tft.setTextColor(TFT_BLACK);
        } else {
            tft.setTextColor(TFT_WHITE);
        }
        tft.drawString(appList[i], 25, startY + (i * 25));
    }
    
    // Power-Buttons am unteren Bildschirmrand positionieren
    int sysY = 280;
    tft.setTextColor(TFT_WHITE);
    tft.drawString("--- SYSTEM-STEUERUNG ---", 20, sysY);
    
    // Button: Ausschalten
    if (selectedAppIndex == -1) {
        tft.fillRect(15, sysY + 30 - 2, 200, 22, TFT_RED);
        tft.setTextColor(TFT_BLACK);
    } else tft.setTextColor(TFT_RED);
    tft.drawString("[ OS AUSSCHALTEN ]", 25, sysY + 30);
    
    // Button: Reboot
    if (selectedAppIndex == -2) {
        tft.fillRect(235, sysY + 30 - 2, 200, 22, TFT_BLUE);
        tft.setTextColor(TFT_BLACK);
    } else tft.setTextColor(TFT_BLUE);
    tft.drawString("[ NEUSTARTEN ]", 245, sysY + 30);
    
    redrawRequired = false;
}

// ==========================================
// 6. EXECUTION-MANAGER (PARTITION JUMP)
// ==========================================
void launchPythonApp(const String& appName) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("Lade Python-Engine...", 20, 100);
    
    // 1. Speichere das Zielskript temporär auf der SD-Karte
    File check = SD.open("/files/config/next_app.txt", FILE_WRITE);
    if (check) {
        check.print("/apps/" + appName);
        check.close();
    }
    
    // 2. Suche die Python-Core Partition im Flash (Adresse 0x200000 aus Schritt 7)
    const esp_partition_t* py_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "py_core");
    
    if (py_part != NULL) {
        // Schließe SD-Schnittstelle sauber vor dem Wechsel
        SD.end();
        // Setze Boot-Partition auf Python und starte das System neu hinein
        esp_ota_set_boot_partition(py_part);
        esp_restart();
    } else {
        tft.drawString("Fehler: Python-Partition nicht gefunden!", 20, 140);
        vTaskDelay(pdMS_TO_TICKS(2000));
        redrawRequired = true;
    }
}

// ==========================================
// 7. POWER-MANAGEMENT LOGIK
// ==========================================
void setWirelessTransmitters(bool turnOn) {
    if (!turnOn) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        esp_wifi_stop();
        btStop();
        esp_bt_controller_disable();
    } else {
        esp_wifi_start();
    }
}

void applyEcoMode(bool enable) {
    if (enable) {
        backupStates.originalBrightness = settings.brightness;
        backupStates.originalFreq = settings.frequency;
        setWirelessTransmitters(false);
        setCpuFrequencyMhz(80); // Drosselung auf 80 MHz
        if (settings.brightness > 40) settings.brightness = 40;
    } else {
        settings.brightness = backupStates.originalBrightness;
        settings.frequency = backupStates.originalFreq;
        setCpuFrequencyMhz(settings.frequency);
        setWirelessTransmitters(true);
    }
    settings.eco = enable;
}

void applyFlightMode(bool enable) {
    settings.flightMode = enable;
    setWirelessTransmitters(!enable);
}

void prepareHardwareForShutdown() {
    SD.end();
    SPI.end();
    tft.writeCommand(0x10); // Display-Panel Schlafmodus (R_SLPIN)
    pinMode(BACKLIGHT_PIN, OUTPUT);
    digitalWrite(BACKLIGHT_PIN, LOW); // Hintergrundbeleuchtung aus
}

void executeSafeShutdown() {
    prepareHardwareForShutdown();
    // Wakeup über CardKB (I2C SDA Pin 1) konfigurieren vor Deep Sleep
    esp_sleep_enable_gpio_wakeup(1ULL << 1, ESP_GPIO_WAKEUP_LOW);
    esp_deep_sleep_start(); // MH-CD42 schaltet nach 45 Sek ab
}

void executeSafeReboot() {
    prepareHardwareForShutdown();
    ESP.restart();
}

// ==========================================
// 8. KERNEL ENTRY POINTS (START & LOOP)
// ==========================================
void setup() {
    Serial.begin(115200);
    Wire.begin(1, 2, 400000U); // SDA=1, SCL=2
    SPI.begin(12, 13, 11, 10); // SCK, MISO, MOSI, CS
    
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    
    // SD-Karten-Zwangsprüfung
    if (!SD.begin(SD_CS_PIN, SPI, 40000000)) {
        tft.setTextColor(TFT_RED);
        tft.drawString("CRITICAL ERROR: NO SD CARD!", 50, 100);
        while (true) { vTaskDelay(pdMS_TO_TICKS(500)); }
    }
    
    loadSettings();
    loadAndSortApps();
    
    // Boot-Animation (Fade In)
    pinMode(BACKLIGHT_PIN, OUTPUT);
    tft.setTextColor(settings.themeColor);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.drawString(settings.bootphrase, tft.width() / 2, tft.height() / 2);
    
    for (int i = 0; i <= settings.brightness; i += 2) {
        analogWrite(BACKLIGHT_PIN, map(i, 0, 100, 0, 255));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    if (settings.eco) applyEcoMode(true);
}

void loop() {
    if (redrawRequired) {
        drawDesktop();
    }
    
    char key = readKeyboard();
    if (key != 0) {
        int totalItems = appList.size();
        
        if (key == 's' || key == 'S') { // Runter navigieren
            if (selectedAppIndex < totalItems - 1) selectedAppIndex++;
            else if (selectedAppIndex == totalItems - 1) selectedAppIndex = -1; // Zu "Ausschalten"
            else if (selectedAppIndex == -1) selectedAppIndex = -2;             // Zu "Reboot"
            else if (selectedAppIndex == -2) selectedAppIndex = 0;              // Zurück zum Anfang
            redrawRequired = true;
        } 
        else if (key == 'w' || key == 'W') { // Hoch navigieren
            if (selectedAppIndex > 0) selectedAppIndex--;
            else if (selectedAppIndex == 0) selectedAppIndex = -2;              // Zu "Reboot"
            else if (selectedAppIndex == -2) selectedAppIndex = -1;             // Zu "Ausschalten"
            else if (selectedAppIndex == -1) selectedAppIndex = totalItems - 1; // Zum Ende der Apps
            redrawRequired = true;
        } 
        else if (key == 13) { // ENTER-Taste gedrückt
            if (selectedAppIndex == -1) {
                executeSafeShutdown();
            } else if (selectedAppIndex == -2) {
                executeSafeReboot();
            } else if (selectedAppIndex >= 0 && totalItems > 0) {
                launchPythonApp(appList[selectedAppIndex]);
            }
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50)); // CPU Entlastung
}
