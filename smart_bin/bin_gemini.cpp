#include "bin_gemini.h"
#include "bin_net.h"
#include "config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"

#define GEMINI_URL "https://generativelanguage.googleapis.com/v1beta/models/" \
    GEMINI_MODEL ":generateContent?key=" GEMINI_API_KEY

// Request body = HEAD + <base64 jpeg> + TAIL. Both halves are compile-time
// constants. The assembled body prefers internal RAM: TLS uploads sourced
// from PSRAM failed on this board (-3 send payload failed) while the same
// request with an internal-RAM body went through.
static const char kHead[] =
    "{\"contents\":[{\"parts\":[{\"text\":\""
    "You are the vision system of a recycling smart bin. Look at the single "
    "most prominent object deposited in the chamber. Classify it as exactly "
    "one category: glass, milk_carton, metal, plastic, cardboard, can "
    "(aluminum drink can), residual (non-recyclable or heavily contaminated), "
    "or unknown (no clear object visible). Name the object in item: a short "
    "everyday name of at most 20 characters, e.g. Plastic bottle. Estimate "
    "weight_kg, the typical weight of this object in kilograms, and co2_kg, "
    "its carbon footprint: the kilograms of CO2-equivalent emitted to "
    "produce it (use 0 for both numbers when unknown). Respond with JSON only."
    "\"},{\"inlineData\":{\"mimeType\":\"image/jpeg\",\"data\":\"";
static const char kTail[] =
    "\"}}]}],\"generationConfig\":{\"temperature\":0,\"maxOutputTokens\":256,"
    "\"responseMimeType\":\"application/json\",\"responseSchema\":{"
    "\"type\":\"OBJECT\",\"properties\":{"
    "\"category\":{\"type\":\"STRING\",\"enum\":[\"glass\",\"milk_carton\","
    "\"metal\",\"plastic\",\"cardboard\",\"can\",\"residual\",\"unknown\"]},"
    "\"item\":{\"type\":\"STRING\"},"
    "\"weight_kg\":{\"type\":\"NUMBER\"},\"co2_kg\":{\"type\":\"NUMBER\"}},"
    "\"required\":[\"category\",\"item\",\"weight_kg\",\"co2_kg\"]},"
    "\"thinkingConfig\":{\"thinkingBudget\":0}}}";

struct CatRow { const char* cat; bool recyclable; const char* label; };
static const CatRow kCats[] = {
    {"glass",       true,  "Glass"},
    {"milk_carton", true,  "Milk carton"},
    {"cardboard",   true,  "Cardboard"},
    {"metal",       true,  "Metal"},
    {"plastic",     true,  "Plastic"},
    {"can",         true,  "Can"},
    {"residual",    false, "Residual"},
    {"unknown",     false, "Unknown item"},
};

static const CatRow* findCategory(const char* c) {
    for (auto& r : kCats) if (strcmp(r.cat, c) == 0) return &r;
    return nullptr;
}

bool categoryIsRecyclable(const char* c) {
    const CatRow* r = findCategory(c);
    return r && r->recyclable;
}

// Copies Gemini's item name as printable ASCII (the OLED font has no other
// glyphs), trimmed and truncated to fit one display line. Empty → fallback.
static void copyItemName(char* out, size_t outLen, const char* in, const char* fallback) {
    size_t n = 0;
    for (const char* p = in; *p && n < outLen - 1; p++) {
        if (*p < 0x20 || *p > 0x7E) continue;
        if (*p == ' ' && (n == 0 || out[n - 1] == ' ')) continue;
        out[n++] = *p;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
    if (n == 0) strlcpy(out, fallback, outLen);
}

bool geminiClassify(camera_fb_t* fb, GeminiResult& out) {
    if (!fb) return false;
    size_t jpegLen = fb->len;
    if (jpegLen == 0) { esp_camera_fb_return(fb); return false; }

    size_t b64Len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64Len, fb->buf, jpegLen);
    uint8_t* b64 = (uint8_t*)heap_caps_malloc(b64Len + 1, MALLOC_CAP_SPIRAM);
    if (!b64) {
        esp_camera_fb_return(fb);
        Serial.println("[GEM] PSRAM alloc failed (b64)");
        return false;
    }
    size_t written = 0;
    int encErr = mbedtls_base64_encode(b64, b64Len + 1, &written, fb->buf, jpegLen);
    // Frame fully encoded — hand it back to the driver BEFORE the TLS
    // request (the known-good reference sketch frees it here too; holding
    // it across the upload was a delta against the working transport).
    esp_camera_fb_return(fb);
    if (encErr != 0) { free(b64); return false; }

#if DEBUG_VISION_LOG
    // Paste everything between the markers into a base64→jpg decoder to see
    // the exact photo Gemini receives. ~2 s at 115200 baud.
    Serial.printf("[CAM] JPEG %u bytes — base64 dump begin\n", (unsigned)jpegLen);
    Serial.write(b64, written);
    Serial.println("\n[CAM] base64 dump end");
#endif

    size_t bodyLen = sizeof(kHead) - 1 + written + sizeof(kTail) - 1;
    bool bodyInPsram = false;
    uint8_t* body = (uint8_t*)heap_caps_malloc(bodyLen,
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!body) {
        body = (uint8_t*)heap_caps_malloc(bodyLen, MALLOC_CAP_SPIRAM);
        bodyInPsram = true;
    }
    if (!body) { free(b64); Serial.println("[GEM] alloc failed (body)"); return false; }
    memcpy(body, kHead, sizeof(kHead) - 1);
    memcpy(body + sizeof(kHead) - 1, b64, written);
    memcpy(body + sizeof(kHead) - 1 + written, kTail, sizeof(kTail) - 1);
    free(b64);
#if DEBUG_VISION_LOG
    Serial.printf("[GEM] body %u bytes in %s\n", (unsigned)bodyLen,
                  bodyInPsram ? "PSRAM" : "internal RAM");
#endif

    String resp;
    int code = netPostJson(GEMINI_URL, body, bodyLen, resp);
    if (code < 0) {                       // transport hiccup: one paced retry
        delay(500);
        code = netPostJson(GEMINI_URL, body, bodyLen, resp);
    }
    free(body);
    if (code != 200) {
        Serial.printf("[GEM] HTTP %d: %s\n", code, resp.substring(0, 200).c_str());
        return false;
    }

    // Pull only candidates[0].content.parts[0].text out of the envelope.
    JsonDocument filter;
    filter["candidates"][0]["content"]["parts"][0]["text"] = true;
    JsonDocument env;
    if (deserializeJson(env, resp, DeserializationOption::Filter(filter))
            != DeserializationError::Ok) return false;
    const char* text = env["candidates"][0]["content"]["parts"][0]["text"];
    if (!text) { Serial.println("[GEM] no text part"); return false; }
#if DEBUG_VISION_LOG
    Serial.printf("[GEM] raw: %s\n", text);
#endif

    JsonDocument inner;
    if (deserializeJson(inner, text) != DeserializationError::Ok) {
        Serial.printf("[GEM] inner parse failed: %s\n", text);
        return false;
    }
    const char* cat = inner["category"] | "";
    const CatRow* row = findCategory(cat);
    if (!row) { Serial.printf("[GEM] bad category '%s'\n", cat); return false; }

    strlcpy(out.category, cat, sizeof(out.category));
    if (strcmp(cat, "unknown") == 0) {
        // Nothing recognisable in the chamber: no name or estimates to show.
        strlcpy(out.item, row->label, sizeof(out.item));
        out.weightKg = 0.0f;
        out.co2Kg = 0.0f;
    } else {
        copyItemName(out.item, sizeof(out.item), inner["item"] | "", row->label);
        float w = inner["weight_kg"] | 0.0f;
        float c = inner["co2_kg"] | 0.0f;
        // Sanity clamp: a hallucinated estimate cannot break the display.
        out.weightKg = constrain(w, CLAMP_WEIGHT_MIN_KG, CLAMP_WEIGHT_MAX_KG);
        out.co2Kg    = constrain(c, CLAMP_CO2_MIN_KG,   CLAMP_CO2_MAX_KG);
    }
    Serial.printf("[GEM] %s (%s)  %.3f kg  %.3f kgCO2e\n",
                  out.item, out.category, out.weightKg, out.co2Kg);
    return true;
}
