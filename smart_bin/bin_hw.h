#pragma once
#include "esp_camera.h"

// Camera --------------------------------------------------------------
bool hwCameraInit();
// Returns a fresh JPEG frame (flushes one stale buffered frame first).
// Caller MUST call esp_camera_fb_return() on the result. NULL on failure.
camera_fb_t* hwCaptureJpeg();

// Ultrasonic ----------------------------------------------------------
void hwUltraInit();
// One HC-SR04 measurement in cm; 999.0f when no echo (open chamber).
float hwReadDistanceCm();
// Diagnostic probe: ECHO idle level + raw transition count in a 30 ms
// post-trigger window. See bin_hw.cpp for interpretation.
void hwUltraProbe(int& idleLevel, int& transitions);

// Servo ---------------------------------------------------------------
void hwServoInit();
// Starts a non-blocking sort movement toward the recyclable or residual side.
void hwServoSort(bool recyclable);
// Advances the servo state machine; call every loop() pass.
void hwServoUpdate();
bool hwServoIdle();
