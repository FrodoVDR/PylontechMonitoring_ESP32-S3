#include <Arduino.h>
#include "driver/ledc.h"
#include "py_display.h"
#include "config.h"   

extern HealthStatus health;

// Display-Pins
// GPIO 26-32 sind auf dem ESP32-S3 intern für Octal-SPI-Flash reserviert.
// GPIO 4-7 sind auf dem Waveshare ESP32-S3-ETH intern mit dem TF-Kartenslot
// verdrahtet und stehen am Pin-Header NICHT zur Verfügung (SD_CS=4, SD_MISO=5,
// SD_MOSI=6, SD_CLK=7) – daher hier nicht verwenden!
//
// SPI-Bus:  Display nutzt HSPI (SPI3), damit ETH weiterhin den Default-SPI
//           (SPI2/FSPI, GPIO 11-14) exklusiv verwenden kann.
//
// Anschluss-Kurzreferenz (Waveshare ESP32-S3 ETH → Display-Modul):
//   GPIO 18 → MOSI (SDA/DIN)
//   GPIO 15 → SCK  (CLK)
//   GPIO 38 → CS
//   GPIO 39 → DC   (RS/A0)
//   GPIO 40 → RST
//   GPIO 41 → BL   (Backlight PWM, 3.3V wenn kein PWM gewünscht)
//   3.3V    → VCC,  GND → GND
// MISO wird nicht angeschlossen (ST7735 ist write-only).
#define TFT_CS     38
#define TFT_DC     39
#define TFT_RST    40

#define TFT_SCK     15  // SPI3 – frei am Header, kein Konflikt mit SD-Slot (4-7) oder ETH (SPI2: SCLK=13)
#define TFT_MOSI    18  // SPI3 – frei am Header, kein Konflikt mit SD-Slot (4-7) oder ETH (SPI2: MOSI=11)
// MISO wird nicht benötigt (ST7735 ist write-only)

// Backlight-Pin (PWM)
#define TFT_BL          41
#define TFT_BL_CHANNEL  LEDC_CHANNEL_0
#define TFT_BL_TIMER    LEDC_TIMER_0

// Eigener SPI-Bus (SPI3/HSPI) für das Display.
// ETH belegt den Default-SPI (SPI2/FSPI) über SPI.begin() in py_eth.cpp.
// HSPI (=2) ist auf ESP32-S3 der zweite verfügbare SPI-Host (SPI3_HOST).
static SPIClass displaySPI(HSPI);

Adafruit_ST7735 tft = Adafruit_ST7735(&displaySPI, TFT_CS, TFT_DC, TFT_RST);
PyDisplay display;

bool wifiAPMode = false;

String formatCurrent(float amps) {
    char buf[10];

    float a = fabs(amps);

    if (a < 10.0f) {
        // 1.23 oder -1.23
        sprintf(buf, "%s%1.2f",
            (amps < 0 ? "-" : " "),
            a
        );
    }
    else if (a < 100.0f) {
        // 12.3 oder -12.3
        sprintf(buf, "%s%2.1f",
            (amps < 0 ? "-" : " "),
            a
        );
    }
    else {
        // 123 oder -123
        sprintf(buf, "%s%3.0f",
            (amps < 0 ? "-" : " "),
            a
        );
    }

    return String(buf);
}


void PyDisplay::begin() {
    // LEDC-Timer konfigurieren
    ledc_timer_config_t timer = {};
    timer.speed_mode       = LEDC_LOW_SPEED_MODE;   // ESP32-S3: nur Low-Speed-Mode verfügbar
    timer.duty_resolution  = LEDC_TIMER_8_BIT;
    timer.timer_num        = TFT_BL_TIMER;
    timer.freq_hz          = 5000;
    timer.clk_cfg          = LEDC_AUTO_CLK;
    ledc_timer_config(&timer);

    // LEDC-Channel konfigurieren
    ledc_channel_config_t ch = {};
    ch.gpio_num       = TFT_BL;
    ch.speed_mode     = LEDC_LOW_SPEED_MODE;
    ch.channel        = TFT_BL_CHANNEL;
    ch.intr_type      = LEDC_INTR_DISABLE;
    ch.timer_sel      = TFT_BL_TIMER;
    ch.duty           = 255;   // volle Helligkeit
    ch.hpoint         = 0;
    ledc_channel_config(&ch);

    // SPI3 starten (MISO=-1, ST7735 ist write-only)
    displaySPI.begin(TFT_SCK, /*MISO=*/-1, TFT_MOSI, /*SS=*/-1);

    // Display starten
    tft.initR(INITR_BLACKTAB);
    tft.setRotation(1);

    tft.fillScreen(ST77XX_BLACK);
    drawStaticUI();
}

void PyDisplay::setBrightness(uint8_t value) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, TFT_BL_CHANNEL, value);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, TFT_BL_CHANNEL);
}

void PyDisplay::drawStaticUI() {
    tft.fillScreen(ST77XX_BLACK);

    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);

    // 2×2 Raster Labels
    tft.setCursor(1, 10);
    tft.print("U");

    tft.setCursor(1, 35);
    tft.print("I");

    tft.setCursor(90, 10);
    tft.print("T");

    tft.setCursor(90, 35);
    tft.print("SOC");

    // Status unten
    tft.setCursor(5, 110);
    tft.print("WiFi:");

    tft.setCursor(5, 120);
    tft.print("MQTT:");
}



void PyDisplay::updatePwr(int volt_mV, int curr_mA, int temp_mC, int socVal) {
    if (volt == volt_mV &&
        curr == curr_mA &&
        temp == temp_mC &&
        soc  == socVal) {
        return;
    }

    volt = volt_mV;
    curr = curr_mA;
    temp = temp_mC;
    soc  = socVal;

    needsRedraw = true;
}

void PyDisplay::updateWifi(bool connected, const String &ip, int rssi, bool apMode) {
    wifiConnected = connected;
    wifiIP = ip;
    wifiRSSI = rssi;
    wifiAPMode = apMode;
    needsRedraw = true;
}

void PyDisplay::updateMqtt(bool connected, const String &server) {
    mqttConnected = connected;
    mqttServer = server;
    needsRedraw = true;
}

void PyDisplay::drawValues() {
    tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
    tft.setTextSize(2);

    // --- 2×2 Raster ---

    // U (links oben)
    tft.setCursor(15, 10);
    tft.printf("%4.2fV", volt / 1000.0f);

    // I (rechts oben)
    tft.setCursor(100, 10);
    //tft.printf("%5.2fA", curr / 1000.0f);
    tft.printf("%4.1fC", temp / 1000.0f);


    float amps = curr / 1000.0f;
    String iStr = formatCurrent(amps);

    tft.setCursor(15, 35);
    tft.print(iStr);
    tft.print("A");

    // SOC (rechts unten)
    tft.setCursor(110, 35);
    tft.printf("%3d%%", soc);

    // ---------------------------------------------------------
    // HEALTH BLOCK (Mitte des Displays)
    // ---------------------------------------------------------

    // ---------------------------------------------------------
    // HEALTH BLOCK (nur neu zeichnen, wenn sich etwas geändert hat)
    // ---------------------------------------------------------
    // `health` (Vectors + Strings) wird von py_parser_pwr.cpp (Core 1,
    // realtimeTask) unter g_healthMutex geschrieben. Ungeschütztes Lesen hier
    // (Core 0) war eine Data Race auf std::vector/String internals -> konnte
    // während eines laufenden Health-Updates zu Heap-Korruption/Crash führen.
    // Fix: benötigte Felder kurz unter Lock kopieren, Lock sofort freigeben,
    // dann mit den lokalen Kopien zeichnen.
    std::vector<int> okModules, warnModules, errorModules;
    String healthColor = lastHealthColor;
    String strongestMessage = lastHealthMsg;
    if (g_healthMutex && xSemaphoreTake(g_healthMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        okModules = health.okModules;
        warnModules = health.warnModules;
        errorModules = health.errorModules;
        healthColor = health.color;
        strongestMessage = health.strongestMessage;
        xSemaphoreGive(g_healthMutex);
    }

    // Strings für Vergleich erzeugen
    String okStr = "";
    for (int m : okModules) okStr += String(m) + " ";

    String warnStr = "";
    for (int m : warnModules) warnStr += String(m) + " ";

    String errStr = "";
    for (int m : errorModules) errStr += String(m) + " ";

    bool changed =
        (lastHealthColor != healthColor) ||
        (lastHealthOK != okStr) ||
        (lastHealthWarn != warnStr) ||
        (lastHealthErr != errStr) ||
        (lastHealthMsg != strongestMessage);

    if (changed) {
        // Hintergrundfarbe
        uint16_t bg =
            (healthColor == "green")  ? ST77XX_BLACK :
            (healthColor == "yellow") ? ST77XX_YELLOW :
                                        ST77XX_RED;

        // Block löschen
        tft.fillRect(0, 60, 160, 45, bg);

        // Textfarbe
        uint16_t fg =
            (healthColor == "red" || healthColor == "yellow")
                ? ST77XX_BLACK
                : ST77XX_GREEN;

        tft.setTextColor(fg);
        tft.setTextSize(1);

        // Zeile 1
        tft.setCursor(2, 62);
        tft.print("OK: ");
        tft.print(okStr);

        // Zeile 2
        tft.setCursor(2, 74);
        tft.print("Warn: ");
        tft.print(warnStr);

        // Zeile 3
        tft.setCursor(2, 86);
        tft.print("Err: ");
        tft.print(errStr);

        // Zeile 4
        tft.setCursor(2, 98);
        tft.print(strongestMessage);

        // Cache aktualisieren
        lastHealthColor = healthColor;
        lastHealthOK = okStr;
        lastHealthWarn = warnStr;
        lastHealthErr = errStr;
        lastHealthMsg = strongestMessage;
    }


    // --- Status unten ---

    tft.setTextSize(1);

    // WiFi
    tft.setCursor(50, 110);
    tft.setTextColor(wifiConnected ? ST77XX_GREEN : ST77XX_RED, ST77XX_BLACK);

    if (wifiAPMode) {
        tft.printf("AP 192.168.4.1");
    } else {
        tft.printf("%s (%ddBm)", wifiIP.c_str(), wifiRSSI);
    }

    // MQTT
    tft.setCursor(50, 120);
    tft.setTextColor(mqttConnected ? ST77XX_GREEN : ST77XX_RED, ST77XX_BLACK);
    tft.printf("%s", mqttServer.c_str());
}



void PyDisplay::loop() {
    if (!needsRedraw) return;
    drawValues();
    needsRedraw = false;
}
