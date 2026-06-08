#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Ticker.h>

#define SCAN_INTERVAL_S 2.0

#define WIFI_SSID "Bosa 3G"

#define SUPABASE_URL "https://fuabyfsracjmrykbnndg.supabase.co"
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

// Ujrahasznositott TLS kliens: ESP8266-on a BearSSL puffer foglalasa draga
// (alapertelmezesben ~32KB), ezert egyszer hozzuk letre, es kicsire korlatozzuk
// a puffermeretet, igy nem fogyasztja el a rendelkezesre allo RAM-ot/nem fragmentalja a heap-et.
WiFiClientSecure secureClient;

// Ticker: hardver idozito megszakitas alapjan jelez, amikor scan-elni kell.
// FONTOS: a Ticker callback megszakitas-kontextusban fut, ott NEM vegezhetunk
// blokkolo/hosszu muveletet (WiFi.scanNetworks, HTTP, Serial print stb.) -
// ez azonnali Soft WDT resetet / kivetelt okozna. Ezert a callback csak egy
// volatile flaget allit be, a tenyleges scan+upload munkat a loop() vegzi el,
// amikor a flaget eszreveszi - ez a hivatalosan ajanlott, biztonsagos minta.
Ticker scanTicker;
volatile bool scanRequested = false;

void IRAM_ATTR onScanTick() {
    scanRequested = true;
}

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

void setup() {
    Serial.begin(115200);

    // STA + AP mod: a router-hez kapcsolodunk (Supabase miatt),
    // emellett sajat AP-t is futtatunk, hogy a tobbi ESP megtalaljon minket scan-nel.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID);

    // Kis TLS puffer (alapertelmezes ~16KB/irany helyett ~512 byte),
    // hogy a HTTPS POST ne emesszen fel tul sok RAM-ot.
    secureClient.setInsecure();
    secureClient.setBufferSizes(512, 512);

    Serial.println(WiFi.softAPmacAddress());

    connectToWifi();

    // Hardver idozito megszakitas: SCAN_INTERVAL_S masodpercenkent jelez
    // (delay() helyett), a scan+upload munka a loop()-ban fut le.
    scanTicker.attach(SCAN_INTERVAL_S, onScanTick);
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

void loop() {
    if (scanRequested) {
        scanRequested = false;
        scanForOtherNodes();
    }
}
