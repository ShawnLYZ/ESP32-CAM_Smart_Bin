#pragma once
#include <Arduino.h>

bool dispInit();
void dispBoot(const char* line2);          // "ESP32-CAM Smart Bin" + status line
void dispReady();                          // idle screen
void dispAnalyzing();                      // during capture + Gemini
// Sorted item: name, estimated weight, estimated CO2 footprint, and the
// compartment it went to. weightKg/co2Kg <= 0 are shown as "--".
void dispResult(const char* item, float weightKg, float co2Kg, bool recyclable);
void dispError(const char* line1, const char* line2);
