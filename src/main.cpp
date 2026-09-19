#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Configuration matérielle
// ---------------------------------------------------------------------------
static const int PIN_R = 25;
static const int PIN_G = 26;
static const int PIN_B = 27;

static const int PWM_FREQ_HZ    = 5000;
static const int PWM_RESOLUTION = 8;
static const int CH_R = 0, CH_G = 1, CH_B = 2;

// ---------------------------------------------------------------------------
// Provisioning wifi
// ---------------------------------------------------------------------------
// ATTENTION : WPA2 exige un mot de passe d'au moins 8 caractères. "123456"
// (6 caractères) est en dessous du minimum : l'ESP32 créera donc un réseau
// OUVERT (sans mot de passe), le SDK ignore silencieusement un mdp trop court.
// Allonge-le (ex: "123456789") si tu veux une vraie protection.
static const char* AP_SSID = "Lumière";
static const char* AP_PASS = "123456789";
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

DNSServer dnsServer;
bool provisioning = false;

// Serveur web + websocket (utilisé dans les deux modes)
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

Preferences ledPrefs;
Preferences wifiPrefs;

// ---------------------------------------------------------------------------
// Etat courant de l'éclairage
// ---------------------------------------------------------------------------
enum Mode { MODE_FIXED, MODE_BREATHE, MODE_FADE, MODE_WAVE };
struct Color { uint8_t r, g, b; };

Mode currentMode = MODE_FIXED;
Color color1 = {255, 120, 0};
Color color2 = {0, 80, 255};
uint8_t brightness = 255;

struct WakeConfig {
  bool enabled = false;
  uint8_t hour = 7;
  uint8_t minute = 0;
  bool days[7] = {false, true, true, true, true, true, false};
  uint16_t durationSec = 20 * 60;
};
WakeConfig wake;

bool wakeRunning = false;
unsigned long wakeStartTime = 0;
int lastWakeTriggerDay = -1;

// ---------------------------------------------------------------------------
// Bas niveau : PWM
// ---------------------------------------------------------------------------
void writeRGB(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(CH_R, (r * brightness) / 255);
  ledcWrite(CH_G, (g * brightness) / 255);
  ledcWrite(CH_B, (b * brightness) / 255);
}

// ---------------------------------------------------------------------------
// Persistance de l'état LED
// ---------------------------------------------------------------------------
void saveState() {
  ledPrefs.putUChar("mode", currentMode);
  ledPrefs.putUChar("c1r", color1.r); ledPrefs.putUChar("c1g", color1.g); ledPrefs.putUChar("c1b", color1.b);
  ledPrefs.putUChar("c2r", color2.r); ledPrefs.putUChar("c2g", color2.g); ledPrefs.putUChar("c2b", color2.b);
  ledPrefs.putUChar("bright", brightness);
  ledPrefs.putBool("wEn", wake.enabled);
  ledPrefs.putUChar("wH", wake.hour);
  ledPrefs.putUChar("wM", wake.minute);
  ledPrefs.putUShort("wDur", wake.durationSec);
  uint8_t daysMask = 0;
  for (int i = 0; i < 7; i++) if (wake.days[i]) daysMask |= (1 << i);
  ledPrefs.putUChar("wDays", daysMask);
}

void loadState() {
  currentMode = (Mode)ledPrefs.getUChar("mode", MODE_FIXED);
  color1.r = ledPrefs.getUChar("c1r", 255); color1.g = ledPrefs.getUChar("c1g", 120); color1.b = ledPrefs.getUChar("c1b", 0);
  color2.r = ledPrefs.getUChar("c2r", 0);   color2.g = ledPrefs.getUChar("c2g", 80);  color2.b = ledPrefs.getUChar("c2b", 255);
  brightness = ledPrefs.getUChar("bright", 255);
  wake.enabled = ledPrefs.getBool("wEn", false);
  wake.hour = ledPrefs.getUChar("wH", 7);
  wake.minute = ledPrefs.getUChar("wM", 0);
  wake.durationSec = ledPrefs.getUShort("wDur", 20 * 60);
  uint8_t daysMask = ledPrefs.getUChar("wDays", 0b00111110);
  for (int i = 0; i < 7; i++) wake.days[i] = daysMask & (1 << i);
}

// ---------------------------------------------------------------------------
// Diffusion de l'état courant (websocket)
// ---------------------------------------------------------------------------
void broadcastState() {
  JsonDocument doc;
  const char* modeNames[] = {"fixed", "breathe", "fade", "wave"};
  doc["mode"] = modeNames[currentMode];
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", color1.r, color1.g, color1.b);
  doc["color1"] = buf;
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", color2.r, color2.g, color2.b);
  doc["color2"] = buf;
  doc["brightness"] = brightness;
  doc["wakeEnabled"] = wake.enabled;
  doc["wakeHour"] = wake.hour;
  doc["wakeMinute"] = wake.minute;
  doc["wakeDuration"] = wake.durationSec;
  JsonArray daysArr = doc["wakeDays"].to<JsonArray>();
  for (int i = 0; i < 7; i++) daysArr.add(wake.days[i]);
  doc["wakeRunning"] = wakeRunning;

  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

// ---------------------------------------------------------------------------
// Commandes reçues depuis l'UI
// ---------------------------------------------------------------------------
Color parseHexColor(const String& hex) {
  Color c = {0, 0, 0};
  if (hex.length() >= 7) {
    c.r = strtol(hex.substring(1, 3).c_str(), nullptr, 16);
    c.g = strtol(hex.substring(3, 5).c_str(), nullptr, 16);
    c.b = strtol(hex.substring(5, 7).c_str(), nullptr, 16);
  }
  return c;
}

void handleCommand(const String& msg) {
  JsonDocument doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok) return;

  String cmd = doc["cmd"] | "";

  if (cmd == "setMode") {
    String m = doc["mode"] | "fixed";
    if (m == "fixed") currentMode = MODE_FIXED;
    else if (m == "breathe") currentMode = MODE_BREATHE;
    else if (m == "fade") currentMode = MODE_FADE;
    else if (m == "wave") currentMode = MODE_WAVE;
    if (doc["color1"].is<const char*>()) color1 = parseHexColor(doc["color1"].as<String>());
    if (doc["color2"].is<const char*>()) color2 = parseHexColor(doc["color2"].as<String>());
    wakeRunning = false;
    saveState();
  } else if (cmd == "setBrightness") {
    brightness = constrain((int)doc["value"], 0, 255);
    saveState();
  } else if (cmd == "setWake") {
    wake.enabled = doc["enabled"] | false;
    wake.hour = doc["hour"] | 7;
    wake.minute = doc["minute"] | 0;
    wake.durationSec = doc["duration"] | 1200;
    JsonArray daysArr = doc["days"];
    for (int i = 0; i < 7 && i < (int)daysArr.size(); i++) wake.days[i] = daysArr[i];
    saveState();
  }

  broadcastState();
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
               void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    broadcastState();
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      String msg((char*)data, len);
      handleCommand(msg);
    }
  }
}

// ---------------------------------------------------------------------------
// Moteur d'effets
// ---------------------------------------------------------------------------
Color hsvToRgb(float h, float s, float v) {
  float r, g, b;
  int i = int(h * 6);
  float f = h * 6 - i;
  float p = v * (1 - s);
  float q = v * (1 - f * s);
  float t = v * (1 - (1 - f) * s);
  switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return {(uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255)};
}

void effectsTask(void* pv) {
  for (;;) {
    unsigned long t = millis();

    if (wakeRunning) {
      float progress = (float)(t - wakeStartTime) / (wake.durationSec * 1000.0f);
      uint8_t oldBrightness = brightness;
      if (progress >= 1.0f) {
        brightness = 255;
      } else {
        brightness = (uint8_t)(progress * 255);
      }
      writeRGB(255, 200, 120);
      brightness = oldBrightness;
    } else {
      switch (currentMode) {
        case MODE_FIXED:
          writeRGB(color1.r, color1.g, color1.b);
          break;
        case MODE_BREATHE: {
          float phase = (sinf(t / 1500.0f) + 1.0f) / 2.0f;
          uint8_t oldBrightness = brightness;
          brightness = (uint8_t)(phase * oldBrightness);
          writeRGB(color1.r, color1.g, color1.b);
          brightness = oldBrightness;
          break;
        }
        case MODE_FADE: {
          float phase = (sinf(t / 3000.0f) + 1.0f) / 2.0f;
          uint8_t r = color1.r + (color2.r - color1.r) * phase;
          uint8_t g = color1.g + (color2.g - color1.g) * phase;
          uint8_t b = color1.b + (color2.b - color1.b) * phase;
          writeRGB(r, g, b);
          break;
        }
        case MODE_WAVE: {
          float phase = (sinf(t / 2000.0f) + 1.0f) / 2.0f;
          Color c = hsvToRgb(phase, 1.0f, 1.0f);
          writeRGB(c.r, c.g, c.b);
          break;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---------------------------------------------------------------------------
// Vérification du déclenchement du réveil
// ---------------------------------------------------------------------------
void wakeCheckTask(void* pv) {
  for (;;) {
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);

    if (wake.enabled && !wakeRunning &&
        t.tm_hour == wake.hour && t.tm_min == wake.minute &&
        wake.days[t.tm_wday] && lastWakeTriggerDay != t.tm_yday) {
      wakeRunning = true;
      wakeStartTime = millis();
      lastWakeTriggerDay = t.tm_yday;
      broadcastState();
    }

    if (wakeRunning && (millis() - wakeStartTime) > (unsigned long)(wake.durationSec * 1000UL + 5000UL)) {
      wakeRunning = false;
      broadcastState();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ---------------------------------------------------------------------------
// Démarrage de l'app "normale" une fois connecté à la box
// ---------------------------------------------------------------------------
void startMainApp() {
  if (MDNS.begin("eclairage")) {
    Serial.println("mDNS actif : http://eclairage.local");
  }

  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.begin();

  xTaskCreatePinnedToCore(effectsTask, "effects", 4096, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(wakeCheckTask, "wakeCheck", 4096, nullptr, 1, nullptr, 1);
}

// ---------------------------------------------------------------------------
// Provisioning : l'ESP32 émet son propre réseau "Lumière"
// ---------------------------------------------------------------------------
void startProvisioning() {
  provisioning = true;
  writeRGB(0, 0, 0);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("Mode provisioning, réseau \"");
  Serial.print(AP_SSID);
  Serial.print("\" - IP : ");
  Serial.println(apIP);

  // Redirige toutes les requêtes DNS vers l'ESP32 (portail captif)
  dnsServer.start(53, "*", apIP);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/config.html", "text/html");
  });

  server.on("/wifi-config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("ssid", true)) {
      request->send(400, "text/plain", "SSID manquant");
      return;
    }
    String ssid = request->getParam("ssid", true)->value();
    String pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value() : "";

    wifiPrefs.putString("ssid", ssid);
    wifiPrefs.putString("pass", pass);

    request->send(LittleFS, "/wifi-saved.html", "text/html");

    // Laisse le temps à la réponse HTTP de partir avant de redémarrer
    delay(1500);
    ESP.restart();
  });

  // Détection de portail captif par les OS (Android/iOS/Windows) : on redirige
  // tout le reste vers la page de config pour que la popup de connexion s'ouvre.
  server.onNotFound([apIP](AsyncWebServerRequest *request) {
    request->redirect("http://" + apIP.toString() + "/");
  });

  server.begin();
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  ledcSetup(CH_R, PWM_FREQ_HZ, PWM_RESOLUTION);
  ledcSetup(CH_G, PWM_FREQ_HZ, PWM_RESOLUTION);
  ledcSetup(CH_B, PWM_FREQ_HZ, PWM_RESOLUTION);
  ledcAttachPin(PIN_R, CH_R);
  ledcAttachPin(PIN_G, CH_G);
  ledcAttachPin(PIN_B, CH_B);
  writeRGB(0, 0, 0);

  ledPrefs.begin("led-esp32", false);
  wifiPrefs.begin("wifi-cfg", false);
  loadState();

  if (!LittleFS.begin(true)) {
    Serial.println("Erreur LittleFS");
  }

  String savedSsid = wifiPrefs.getString("ssid", "");
  String savedPass = wifiPrefs.getString("pass", "");

  bool connected = false;
  if (savedSsid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    Serial.print("Connexion à \"" + savedSsid + "\"");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      delay(300);
      Serial.print(".");
    }
    Serial.println();
    connected = (WiFi.status() == WL_CONNECTED);
  }

  if (connected) {
    Serial.print("Connecté, IP : ");
    Serial.println(WiFi.localIP());
    startMainApp();
  } else {
    if (savedSsid.length() > 0) {
      Serial.println("Échec de connexion avec les identifiants enregistrés.");
    } else {
      Serial.println("Aucun identifiant wifi enregistré.");
    }
    startProvisioning();
  }
}

void loop() {
  if (provisioning) {
    dnsServer.processNextRequest();
  } else {
    ws.cleanupClients();
  }
  delay(20);
}
