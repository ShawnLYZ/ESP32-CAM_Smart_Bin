// ╔══════════════════════════════════════════════════════════════════╗
// ║  ESP32-CAM Smart Bin — standalone ESP32-CAM firmware             ║
// ║  Ultrasonic → camera → Gemini → servo sort → OLED                ║
// ╚══════════════════════════════════════════════════════════════════╝
#include <Arduino.h>
#include "config.h"
#include "bin_hw.h"
#include "bin_display.h"
#include "bin_net.h"
#include "bin_gemini.h"

enum BinState { ST_IDLE, ST_SETTLE, ST_RESULT, ST_ERROR };

static BinState state = ST_IDLE;
static unsigned long stateUntil = 0;      // timed-screen deadline

static bool armed = false;                // chamber must read clear to re-arm
static int aboveCount = 0, belowCount = 0;
static unsigned long lastDistanceRead = 0;
static unsigned long settleAt = 0;

static void enterTimed(BinState timedState, unsigned long durMs) {
    state = timedState;
    stateUntil = millis() + durMs;
}

// Fail-closed: an item the bin could not identify goes to the residual side.
static void failDeposit(const char* msg) {
    Serial.printf("[BIN] deposit failed: %s\n", msg);
    hwServoSort(false);
    dispError(msg, "Item -> residual");
    enterTimed(ST_ERROR, ERROR_SCREEN_MS);
}

// ── Deposit pipeline (synchronous: capture → Gemini → sort → show) ───
static void processDeposit() {
    dispAnalyzing();
    camera_fb_t* fb = hwCaptureJpeg();
    if (!fb) { failDeposit("Camera error"); return; }
    GeminiResult r;
    bool ok = geminiClassify(fb, r);  // consumes fb (returned before upload)
    if (!ok) { failDeposit("AI unavailable"); return; }

    bool recyclable = categoryIsRecyclable(r.category);
    hwServoSort(recyclable);
    dispResult(r.item, r.weightKg, r.co2Kg, recyclable);
    enterTimed(ST_RESULT, RESULT_SCREEN_MS);
}

// ── Setup ────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== ESP32-CAM Smart Bin (standalone) ===");
    hwServoInit();
    hwUltraInit();
    dispInit();
    dispBoot("starting...");
    if (!hwCameraInit()) { dispBoot("CAMERA FAILED"); delay(ERROR_SCREEN_MS); }
    if (!psramFound())
        Serial.println("[MEM] WARNING: PSRAM missing — enable Tools > PSRAM");
    dispBoot("wifi...");
    while (!netWifiConnected()) { netEnsureWifi(); delay(1000); }
    netBootDiag();
    dispReady();
    state = ST_IDLE;
}

// ── Main loop ────────────────────────────────────────────────────────
void loop() {
    hwServoUpdate();
    netEnsureWifi();                  // throttled reconnect
    unsigned long now = millis();

    // Timed screens (result / error) → back to the ready screen.
    if ((state == ST_RESULT || state == ST_ERROR) &&
        (long)(now - stateUntil) >= 0) {
        dispReady();
        state = ST_IDLE;
    }

    // Ultrasonic watch (idle or while a result is showing), servo at rest.
    if ((state == ST_IDLE || state == ST_RESULT) && hwServoIdle() &&
        now - lastDistanceRead >= DISTANCE_POLL_MS) {
        lastDistanceRead = now;
        float d = hwReadDistanceCm();
#if DEBUG_ULTRA_LOG
        // 1 Hz summary of the ~10 reads since the last line. `timeout` counts
        // reads with no echo at all (reported as 999 = "chamber clear").
        static unsigned long ultraLogAt = 0;
        static int ultraOk = 0, ultraTimeout = 0;
        static float ultraLast = -1.0f;
        if (d >= 998.0f) ultraTimeout++; else { ultraOk++; ultraLast = d; }
        if ((long)(now - ultraLogAt) >= 1000) {
            ultraLogAt = now;
            int idle = -1, trans = 0;
            hwUltraProbe(idle, trans);
            Serial.printf("[ULTRA] last=%.1fcm ok=%d timeout=%d idle=%d trans=%d armed=%d above=%d below=%d\n",
                          ultraLast, ultraOk, ultraTimeout, idle, trans,
                          (int)armed, aboveCount, belowCount);
            ultraOk = ultraTimeout = 0;
        }
#endif
        if (!armed) {
            if (d >= CLEAR_DISTANCE_CM) {
                if (++aboveCount >= CLEAR_CONSECUTIVE) { armed = true; aboveCount = 0; }
            } else aboveCount = 0;
        } else if (d > 0 && d <= TRIGGER_DISTANCE_CM) {
            if (++belowCount >= DETECT_CONSECUTIVE) {
                belowCount = 0;
                armed = false;        // re-arm requires the chamber to clear
                settleAt = now + SETTLE_DELAY_MS;
                state = ST_SETTLE;    // last result (if any) stays on screen meanwhile
                Serial.printf("[BIN] deposit detected at %.1f cm\n", d);
            }
        } else belowCount = 0;
    }

    if (state == ST_SETTLE && (long)(now - settleAt) >= 0) processDeposit();
}
