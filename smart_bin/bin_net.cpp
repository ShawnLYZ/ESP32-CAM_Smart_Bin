#include "bin_net.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "lwip/dns.h"
#include "esp_heap_caps.h"

// Once the router's DNS fails us we pin public resolvers for the rest of
// the uptime (DHCP re-installs the router's DNS on every reconnect).
static bool s_publicDns = false;

static void applyPublicDns() {
    ip_addr_t d1, d2;
    IP_ADDR4(&d1, 8, 8, 8, 8);
    IP_ADDR4(&d2, 1, 1, 1, 1);
    dns_setserver(0, &d1);
    dns_setserver(1, &d2);
}

// "https://host/path?query" -> "host"
static void hostFromUrl(const char* url, char* out, size_t outLen) {
    const char* p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != '?' && i < outLen - 1) { out[i] = p[i]; i++; }
    out[i] = '\0';
}

bool netTlsProbe(const char* host) {
    WiFiClientSecure c;
    c.setInsecure();
    unsigned long t0 = millis();
    bool ok = c.connect(host, 443, 10000);   // explicit ms timeout
    Serial.printf("[NET] TLS probe %s -> %s (%lums)\n",
                  host, ok ? "OK" : "FAIL", millis() - t0);
    c.stop();
    return ok;
}

static bool dnsProbe(const char* host) {
    IPAddress ip;
    unsigned long t0 = millis();
    bool ok = WiFi.hostByName(host, ip) == 1;
    Serial.printf("[NET] DNS %s -> %s (%lums)\n",
                  host, ok ? ip.toString().c_str() : "FAIL", millis() - t0);
    if (!ok && !s_publicDns) {
        s_publicDns = true;
        applyPublicDns();
        t0 = millis();
        ok = WiFi.hostByName(host, ip) == 1;
        Serial.printf("[NET] DNS retry via 8.8.8.8 -> %s (%lums)\n",
                      ok ? ip.toString().c_str() : "FAIL", millis() - t0);
    }
    return ok;
}

bool netWifiConnected() { return WiFi.status() == WL_CONNECTED; }

bool netEnsureWifi() {
    if (netWifiConnected()) return true;
    static unsigned long lastAttempt = 0;
    if (lastAttempt != 0 && millis() - lastAttempt < WIFI_RETRY_MS) return false;
    lastAttempt = millis();
    Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    // Modem power-save trades throughput for latency spikes that make some
    // routers drop TLS handshakes entirely; the bin is mains-powered.
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long t0 = millis();
    while (!netWifiConnected() && millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) delay(250);
    if (netWifiConnected()) {
        Serial.println("[WiFi] Connected: " + WiFi.localIP().toString());
        if (s_publicDns) applyPublicDns();   // DHCP just overwrote our resolvers
    } else {
        Serial.println("[WiFi] Connect timed out");
    }
    return netWifiConnected();
}

void netBootDiag() {
    Serial.printf("[NET] ip=%s gw=%s dns=%s rssi=%ddBm heap=%u largest=%u\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.dnsIP().toString().c_str(),
                  WiFi.RSSI(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    dnsProbe("generativelanguage.googleapis.com");
    // One handshake before the camera has ever captured: tells us whether
    // TLS works at all in the pre-capture state.
    netTlsProbe("generativelanguage.googleapis.com");
}

int netPostJson(const char* url, const uint8_t* body, size_t len, String& respOut) {
    respOut = "";
    if (!netWifiConnected()) {
        Serial.println("[NET] request skipped: WiFi down");
        return -1;
    }
    unsigned long t0 = millis();
    WiFiClientSecure client;
    client.setInsecure();               // prototype posture (accepted risk)
    HTTPClient http;
    if (!http.begin(client, url)) return -2;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(10000);      // default is 5 s — too tight here
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(const_cast<uint8_t*>(body), len);
    if (code > 0) respOut = http.getString();
    if (code <= 0) {
        // Transport-level failure: name the layer so the serial log alone
        // can separate DNS vs TCP/TLS vs RF/heap problems. (URL is not
        // printed — it carries API keys in the query string.)
        char host[64];
        hostFromUrl(url, host, sizeof(host));
        Serial.printf("[NET] POST %s failed: %d (%s) after %lums rssi=%ddBm heap=%u largest=%u\n",
                      host, code, http.errorToString(code).c_str(),
                      millis() - t0, WiFi.RSSI(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        // -1 (connection refused/failed) is where DNS trouble hides; the
        // probe also switches to public DNS so the NEXT attempt can succeed.
        if (code == HTTPC_ERROR_CONNECTION_REFUSED) {
            dnsProbe(host);
            // Cross-check another Google host: FAIL here means the whole
            // radio/TLS stack is degraded right now, OK means only the
            // original destination is refusing us.
            netTlsProbe("www.google.com");
        }
    }
    http.end();
    return code;
}
