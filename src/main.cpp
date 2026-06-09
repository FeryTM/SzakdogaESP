#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Ticker.h>

#define SCAN_INTERVAL_S   2.0
#define CONFIG_INTERVAL_S 10.0

#define WIFI_SSID "Bosa 3G"

#define SUPABASE_URL      "https://fuabyfsracjmrykbnndg.supabase.co"
#define SUPABASE_ANON_KEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZ1YWJ5ZnNyYWNqbXJ5a2JubmRnIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODA2MDQwMjUsImV4cCI6MjA5NjE4MDAyNX0.Cp2naUYvwjv_KtiJCswUkc2ZWnHut-Ce9jbU0Ep66dQ"

// Ismert ESP node-ok MAC cime (BSSID) alapjan, sajat magunk nelkul.
struct KnownNode {
    const char *node_id;
    const char *mac;
};

// A NODE_ID, AP_SSID es knownNodes[] a build flag alapjan kivalasztott
// node-specifikus configbol szarmazik (lasd platformio.ini env-ek es include/).
#if defined(NODE1)
#include "node1_config.h"
#elif defined(NODE2)
#include "node2_config.h"
#elif defined(NODE3)
#include "node3_config.h"
#else
#error "Hianyzo build flag: definialj NODE1, NODE2 vagy NODE3 erteket (lasd platformio.ini)"
#endif

const size_t knownNodeCount = sizeof(knownNodes) / sizeof(knownNodes[0]);

// Ujrahasznositott TLS kliens: egyszer foglaljuk le, minden HTTP hivásnal
// ugyanazt hasznaljuk, igy nem fragmentalodik a heap.
WiFiClientSecure secureClient;

// --- Ticker-ek es flagek ---
// Mindket Ticker csak egy-egy volatile bool flaget allit be a megszakitas-
// kontextusban; a tényleges munkat a loop() vegzi el, ahol blokkolo/hosszu
// muvelet (HTTP, WiFi scan) biztonsagosan futtatható.

Ticker scanTicker;
Ticker configTicker;

volatile bool scanRequested   = false;
volatile bool configRequested = false;

// measurement_active: a Supabase system_config tablabol olvasott ertek.
// Ha false, a node nem vegez scan-t es nem kuld adatot.
bool measurementActive = false;

void IRAM_ATTR onScanTick() {
    scanRequested = true;
}

void IRAM_ATTR onConfigTick() {
    configRequested = true;
}

// ------------------------------------------------------------------

bool parseMac(const char *macStr, uint8_t out[6]) {
    int values[6];
    if (sscanf(macStr, "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)values[i];
    }
    return true;
}

void connectToWifi() {
    WiFi.begin(WIFI_SSID);
    Serial.println("WiFi csatlakozas...");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("");
    Serial.println("WiFi csatlakozva!");
    Serial.print("IP cim: ");
    Serial.println(WiFi.localIP());
}

// system_config tabla lekerdezes: visszaadja a measurement_active erteket.
// A tabla elso sorat olvassa (LIMIT=1). Valasz pl.: [{"measurement_active":true}]
// Egyszeru keresés a JSON-ben, konyvtar nelkul.
bool fetchMeasurementActive() {
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    HTTPClient http;
    String url = String(SUPABASE_URL) +
                 "/rest/v1/system_config?select=measurement_active&limit=1";

    if (!http.begin(secureClient, url)) {
        Serial.println("Config GET: kapcsolat sikertelen");
        return false;
    }

    http.addHeader("apikey", SUPABASE_ANON_KEY);
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);

    int httpCode = http.GET();
    bool active = false;

    if (httpCode == 200) {
        String body = http.getString();
        Serial.print("system_config valasz: ");
        Serial.println(body);
        // Egyszeru keresés: tartalmazza-e a "true" szot?
        active = body.indexOf("true") != -1;
    } else {
        Serial.print("Config GET hiba, kod: ");
        Serial.println(httpCode);
    }

    http.end();
    return active;
}

void checkConfig() {
    bool active = fetchMeasurementActive();
    if (active != measurementActive) {
        measurementActive = active;
        Serial.print("Meres allapota megvaltozott: ");
        Serial.println(measurementActive ? "AKTIV" : "INAKTIV");
    }
}

void sendRssiToSupabase(const char *targetNodeId, int rssi) {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    HTTPClient http;
    String url = String(SUPABASE_URL) + "/rest/v1/rssi_measurements";

    if (http.begin(secureClient, url)) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("apikey", SUPABASE_ANON_KEY);
        http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);

        String payload = String("{\"node_id\":\"") + NODE_ID +
                         "\",\"target_node_id\":\"" + targetNodeId +
                         "\",\"rssi\":" + String(rssi) +
                         ",\"is_baseline\":false}";

        int httpCode = http.POST(payload);
        Serial.print("Supabase POST (");
        Serial.print(targetNodeId);
        Serial.print(") valasz: ");
        Serial.println(httpCode);

        http.end();
    } else {
        Serial.println("HTTP kapcsolat letrehozasa sikertelen");
    }
}

void scanForOtherNodes() {
    int found = WiFi.scanNetworks();
    if (found <= 0) {
        Serial.println("Nincs talalat a WiFi scan soran");
        return;
    }

    for (int i = 0; i < found; i++) {
        uint8_t *bssid = WiFi.BSSID(i);
        int rssi = WiFi.RSSI(i);

        for (size_t n = 0; n < knownNodeCount; n++) {
            uint8_t knownMac[6];
            if (!parseMac(knownNodes[n].mac, knownMac)) {
                continue;
            }

            if (memcmp(bssid, knownMac, 6) == 0) {
                Serial.print(knownNodes[n].node_id);
                Serial.print(" RSSI: ");
                Serial.print(rssi);
                Serial.println(" dBm");

                sendRssiToSupabase(knownNodes[n].node_id, rssi);
            }
        }
    }

    WiFi.scanDelete();
}

// ------------------------------------------------------------------

void setup() {
    Serial.begin(115200);

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID);

    secureClient.setInsecure();
    // RX puffer: 4096 byte kell a Supabase TLS tanusitvanylanc fogadasahoz.
    // TX puffer: 512 byte eleg a kis JSON payload kuldésehez.
    secureClient.setBufferSizes(4096, 512);

    Serial.println(WiFi.softAPmacAddress());

    connectToWifi();

    // Indulaskor azonnal lekerdezes: csak akkor indul a meres,
    // ha a system_config-ban measurement_active = true.
    Serial.println("system_config lekerdezes...");
    checkConfig();

    // Scan ticker: 2 masodpercenkent jelez, de csak akkor fut le
    // a scan+upload, ha measurementActive == true.
    scanTicker.attach(SCAN_INTERVAL_S, onScanTick);

    // Config ticker: 10 masodpercenkent ujra lekerdi az aktiv allapotot.
    configTicker.attach(CONFIG_INTERVAL_S, onConfigTick);
}

void loop() {
    if (configRequested) {
        configRequested = false;
        checkConfig();
    }

    if (scanRequested) {
        scanRequested = false;
        if (measurementActive) {
            scanForOtherNodes();
        }
    }
}
