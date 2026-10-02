#pragma once
#include "esp_camera.h"

struct GeminiResult {
    char category[16];   // one of the fixed schema enum values
    char item[22];       // short object name, printable ASCII, fits one OLED line
    float weightKg;      // clamped to CLAMP_WEIGHT_MIN/MAX_KG (0 when unknown)
    float co2Kg;         // clamped to CLAMP_CO2_MIN/MAX_KG (0 when unknown)
};

// Sends the JPEG to Gemini (strict-JSON schema, thinking budget 0).
// CONSUMES fb: it is returned to the camera driver before the upload starts
// (on every path) — the caller must NOT call esp_camera_fb_return() on it.
// True on success with a valid category; false on ANY failure (fail-closed).
bool geminiClassify(camera_fb_t* fb, GeminiResult& out);

// True for categories that go to the recyclable compartment.
bool categoryIsRecyclable(const char* category);
