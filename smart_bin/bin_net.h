#pragma once
#include <Arduino.h>

bool netWifiConnected();
// Throttled reconnect (WIFI_RETRY_MS between attempts). True when connected.
bool netEnsureWifi();
// One-shot boot report: IP/gateway/DNS/RSSI/heap + DNS resolution and TLS
// connect probe of the Gemini API host. Falls back to public DNS
// (8.8.8.8/1.1.1.1) when the router's DNS fails to resolve it.
void netBootDiag();
// Opens and immediately closes a TLS connection to host:443, logging the
// outcome and duration. Diagnostic only.
bool netTlsProbe(const char* host);
// TLS with setInsecure() — accepted prototype risk.
// Returns HTTP status code, or negative on transport error. Body → respOut.
int netPostJson(const char* url, const uint8_t* body, size_t bodyLen, String& respOut);
