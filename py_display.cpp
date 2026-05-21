#include <Arduino.h>
#include "driver/ledc.h"
#include "py_display.h"
#include "config.h"   

extern HealthStatus health;

// Display-Pins
#define TFT_CS     23
#define TFT_DC     27
#define TFT_RST    26

#define TFT_SCK    14
#define TFT_MOSI   13
#define TFT_MISO   12

// Backlight-Pin (PWM)
#define TFT_BL          33
#define TFT_BL_CHANNEL  LEDC_CHANNEL_0
#define TFT_BL_TIMER    LEDC_TIMER_0

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
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
    timer.speed_mode       = LEDC_HIGH_SPEED_MODE;
    timer.duty_resolution  = LEDC_TIMER_8_BIT;
    timer.timer_num        = TFT_BL_TIMER;
    timer.freq_hz          = 5000;
    timer.clk_cfg          = LEDC_AUTO_CLK;
    ledc_timer_config(&timer);

    // LEDC-Channel konfigurieren
    ledc_channel_config_t ch = {};
    ch.gpio_num       = TFT_BL;
    ch.speed_mode     = LEDC_HIGH_SPEED_MODE;
    ch.channel        = TFT_BL_CHANNEL;
    ch.intr_type      = LEDC_INTR_DISABLE;
    ch.timer_sel      = TFT_BL_TIMER;
    ch.duty           = 255;   // volle Helligkeit
    ch.hpoint         = 0;
    ledc_channel_config(&ch);

    // SPI starten
    SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI);

    // Display starten
    tft.initR(INITR_BLACKTAB);
    tft.setRotation(1);

    tft.fillScreen(ST77XX_BLACK);
    drawStaticUI();
}

void PyDisplay::setBrightness(uint8_t value) {
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, TFT_BL_CHANNEL, value);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, TFT_BL_CHANNEL);
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
    extern HealthStatus health;

    // Strings für Vergleich erzeugen
    String okStr = "";
    for (int m : health.okModules) okStr += String(m) + " ";

    String warnStr = "";
    for (int m : health.warnModules) warnStr += String(m) + " ";

    String errStr = "";
    for (int m : health.errorModules) errStr += String(m) + " ";

    bool changed =
        (lastHealthColor != health.color) ||
        (lastHealthOK != okStr) ||
        (lastHealthWarn != warnStr) ||
        (lastHealthErr != errStr) ||
        (lastHealthMsg != health.strongestMessage);

    if (changed) {
        // Hintergrundfarbe
        uint16_t bg =
            (health.color == "green")  ? ST77XX_BLACK :
            (health.color == "yellow") ? ST77XX_YELLOW :
                                        ST77XX_RED;

        // Block löschen
        tft.fillRect(0, 60, 160, 45, bg);

        // Textfarbe
        uint16_t fg =
            (health.color == "red" || health.color == "yellow")
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
        tft.print(health.strongestMessage);

        // Cache aktualisieren
        lastHealthColor = health.color;
        lastHealthOK = okStr;
        lastHealthWarn = warnStr;
        lastHealthErr = errStr;
        lastHealthMsg = health.strongestMessage;
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
