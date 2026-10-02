#include "bin_hw.h"
#include "config.h"
#include <Arduino.h>
#include <ESP32Servo.h>

// ── AI-Thinker ESP32-CAM pin map ─────────────────────────────────────
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM   0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM     5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22

static bool s_cameraReady = false;

bool hwCameraInit() {
    if (s_cameraReady) return true;
    // Clear any half-initialized driver state left by a previous failed
    // attempt (e.g. a brownout mid-init). Safe to call even when the driver
    // was never initialized — it just returns an error, which we ignore.
    esp_camera_deinit();
    camera_config_t cfg = {};
    cfg.pin_pwdn     = PWDN_GPIO_NUM;
    cfg.pin_reset    = RESET_GPIO_NUM;
    cfg.pin_xclk     = XCLK_GPIO_NUM;
    cfg.pin_sscb_sda = SIOD_GPIO_NUM;
    cfg.pin_sscb_scl = SIOC_GPIO_NUM;
    cfg.pin_d7 = Y9_GPIO_NUM; cfg.pin_d6 = Y8_GPIO_NUM;
    cfg.pin_d5 = Y7_GPIO_NUM; cfg.pin_d4 = Y6_GPIO_NUM;
    cfg.pin_d3 = Y5_GPIO_NUM; cfg.pin_d2 = Y4_GPIO_NUM;
    cfg.pin_d1 = Y3_GPIO_NUM; cfg.pin_d0 = Y2_GPIO_NUM;
    cfg.pin_vsync = VSYNC_GPIO_NUM;
    cfg.pin_href  = HREF_GPIO_NUM;
    cfg.pin_pclk  = PCLK_GPIO_NUM;
    cfg.xclk_freq_hz = 20000000;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size   = FRAMESIZE_QVGA;   // 320x240 — Gemini payload stays small
    cfg.jpeg_quality = 12;
    cfg.fb_count     = 1;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("[CAM] Init failed: 0x%x\n", err);
        return false;
    }
    s_cameraReady = true;
    return true;
}

camera_fb_t* hwCaptureJpeg() {
    // If a previous init failed (commonly a brownout), retry here instead of
    // fail-closing every deposit forever — the bin must recover on its own.
    if (!s_cameraReady && !hwCameraInit()) return nullptr;
    // Flush the buffered (possibly minutes-old) frame, then grab fresh.
    camera_fb_t* stale = esp_camera_fb_get();
    if (stale) esp_camera_fb_return(stale);
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { Serial.println("[CAM] Grab failed"); return nullptr; }
    if (fb->format != PIXFORMAT_JPEG) {
        Serial.println("[CAM] Frame not JPEG");
        esp_camera_fb_return(fb);
        return nullptr;
    }
    return fb;
}

void hwUltraInit() {
    pinMode(ULTRA_TRIG_PIN, OUTPUT);
    digitalWrite(ULTRA_TRIG_PIN, LOW);
    pinMode(ULTRA_ECHO_PIN, INPUT);
}

// Diagnostic: fire one trigger, then raw-sample ECHO for 30 ms. Reports the
// pin's idle level before the trigger and how many level transitions occur.
// Distinguishes "pulses arrive but pulseIn misses them" (firmware) from
// "the line never moves at all" (wiring / divider / sensor power).
void hwUltraProbe(int& idleLevel, int& transitions) {
    idleLevel = digitalRead(ULTRA_ECHO_PIN);
    digitalWrite(ULTRA_TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(ULTRA_TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(ULTRA_TRIG_PIN, LOW);
    transitions = 0;
    int prev = digitalRead(ULTRA_ECHO_PIN);
    unsigned long t0 = micros();
    while (micros() - t0 < 30000UL) {
        int v = digitalRead(ULTRA_ECHO_PIN);
        if (v != prev) { transitions++; prev = v; }
    }
}

float hwReadDistanceCm() {
    digitalWrite(ULTRA_TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(ULTRA_TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(ULTRA_TRIG_PIN, LOW);
    unsigned long us = pulseIn(ULTRA_ECHO_PIN, HIGH, 30000UL); // 30 ms ≈ 5 m
    if (us == 0) return 999.0f;
    return us / 58.0f;
}

// ── Servo (non-blocking hold-and-return) ─────────────────────────────
static Servo s_servo;
enum ServoState { SV_IDLE, SV_HOLDING, SV_RETURNING };
static ServoState s_servoState = SV_IDLE;
static unsigned long s_servoT0 = 0;

void hwServoInit() {
    s_servo.attach(SERVO_PIN);
    s_servo.write(SERVO_HOME_ANGLE);
}

void hwServoSort(bool recyclable) {
    s_servo.write(recyclable ? SERVO_RECYCLABLE_ANGLE : SERVO_RESIDUAL_ANGLE);
    s_servoState = SV_HOLDING;
    s_servoT0 = millis();
    Serial.printf("[SERVO] Sorting to %s side\n", recyclable ? "recyclable" : "residual");
}

void hwServoUpdate() {
    if (s_servoState == SV_IDLE) return;
    unsigned long elapsed = millis() - s_servoT0;
    if (s_servoState == SV_HOLDING && elapsed >= SERVO_HOLD_MS) {
        s_servo.write(SERVO_HOME_ANGLE);
        s_servoState = SV_RETURNING;
        s_servoT0 = millis();
    } else if (s_servoState == SV_RETURNING && elapsed >= SERVO_RETURN_MS) {
        s_servoState = SV_IDLE;
    }
}

bool hwServoIdle() { return s_servoState == SV_IDLE; }
