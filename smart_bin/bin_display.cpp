#include "bin_display.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

static Adafruit_SSD1306 s_oled(128, 64, &Wire, -1);
static bool s_ready = false;

bool dispInit() {
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    s_ready = s_oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    if (!s_ready) { Serial.println("[OLED] init failed"); return false; }
    s_oled.setTextColor(SSD1306_WHITE);
    return true;
}

static void twoLines(const char* l1, const char* l2) {
    if (!s_ready) return;
    s_oled.clearDisplay();
    s_oled.setTextSize(1);
    s_oled.setCursor(0, 24);
    s_oled.println(l1);
    if (l2) { s_oled.setCursor(0, 36); s_oled.println(l2); }
    s_oled.display();
}

void dispBoot(const char* line2)  { twoLines("ESP32-CAM Smart Bin", line2); }
void dispReady()                  { twoLines("Ready for", "next item"); }
void dispAnalyzing()              { twoLines("Analyzing...", "hold on"); }
void dispError(const char* l1, const char* l2) { twoLines(l1, l2); }

// Default font is 6x8 px per character at size 1.
static void printCentered(const char* s, int y, int size) {
    int w = (int)strlen(s) * 6 * size;
    s_oled.setTextSize(size);
    s_oled.setCursor(w < 128 ? (128 - w) / 2 : 0, y);
    s_oled.print(s);
}

// "~25 g" below 1 kg, "~1.20 kg" above; "--" when there is no estimate.
static void fmtMass(char* out, size_t len, float kg) {
    if (kg <= 0.0f)     snprintf(out, len, "--");
    else if (kg < 1.0f) snprintf(out, len, "~%d g", (int)lroundf(kg * 1000.0f));
    else                snprintf(out, len, "~%.2f kg", kg);
}

void dispResult(const char* item, float weightKg, float co2Kg, bool recyclable) {
    if (!s_ready) return;
    s_oled.clearDisplay();
    s_oled.setTextColor(SSD1306_WHITE);

    // Item name: large when it fits on one line (10 chars), small otherwise.
    if (strlen(item) <= 10) printCentered(item, 0, 2);
    else                    printCentered(item, 4, 1);
    s_oled.drawFastHLine(0, 18, 128, SSD1306_WHITE);

    char buf[16];
    s_oled.setTextSize(1);
    fmtMass(buf, sizeof(buf), weightKg);
    s_oled.setCursor(0, 23); s_oled.print("Weight: "); s_oled.print(buf);
    fmtMass(buf, sizeof(buf), co2Kg);
    s_oled.setCursor(0, 35); s_oled.print("CO2:    "); s_oled.print(buf);

    // Compartment banner, inverted so it reads at a glance.
    s_oled.fillRect(0, 48, 128, 16, SSD1306_WHITE);
    s_oled.setTextColor(SSD1306_BLACK);
    printCentered(recyclable ? "RECYCLABLE" : "RESIDUAL", 52, 1);
    s_oled.setTextColor(SSD1306_WHITE);
    s_oled.display();
}
