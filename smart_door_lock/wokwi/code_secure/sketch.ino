#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "mbedtls/md.h"
#include <ArduinoJson.h>
#include <time.h>

// ── CONFIGURATION WIFI / MQTT ────────────────────────────────────
#ifndef WIFI_SSID
  #define WIFI_SSID     "Wokwi-GUEST"
  #define WIFI_PASSWORD ""
#endif

#ifndef MQTT_BROKER
  #define MQTT_BROKER   "url"
  #define MQTT_PORT     8883
  #define MQTT_USER     "username"
  #define MQTT_PASS     "password"
#endif

#define TOPIC_STATUS  "door/status"
#define TOPIC_COMMAND "door/command"
#define TOPIC_ALERT   "door/alert"

// ── PINOUT ───────────────────────────────────────────────────────
#define SERVO_PIN 15

// ── SÉCURITÉ : MOT DE PASSE ─────────────────────────────────────
#define PASSWORD_MIN_LENGTH  4
#define PASSWORD_MAX_LENGTH  6
#define MAX_ATTEMPTS         3
#define LOCKOUT_DURATION_MS  30000UL

// ── SÉCURITÉ : MQTT & COMMANDES ─────────────────────────────────
#define CMD_MAX_AGE_MS       60000UL   // ✅ 60s (doublon supprimé)
#define MIN_CMD_INTERVAL_MS  2000UL
#define NONCE_WINDOW_RAM     100
#define NONCE_WINDOW_NVS     20

// ── RECONNEXION ─────────────────────────────────────────────────
#define RECONNECT_INTERVAL_MS 5000UL

// ── NTP ─────────────────────────────────────────────────────────
#define USE_NTP true
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"

// ── CERTIFICAT ROOT CA ───────────────────────────────────────────
static const char* ROOT_CA = certificate;

// ── OBJETS GLOBAUX ──────────────────────────────────────────────
WiFiClientSecure   wifiClient;
PubSubClient       mqttClient(wifiClient);
Servo              servo;
LiquidCrystal_I2C  lcd(0x27, 16, 2);
Preferences        prefs;

// Keypad
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'}, {'4','5','6','B'},
  {'7','8','9','C'}, {'*','0','#','D'}
};
byte rowPins[ROWS] = {23, 19, 18, 5};
byte colPins[COLS] = {17, 16, 4, 2};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ── ÉTAT DU SYSTÈME ─────────────────────────────────────────────
bool isDoorLocked = true;
char enteredPassword[PASSWORD_MAX_LENGTH + 1] = {0};
byte pwdIndex = 0;

// Sécurité brute-force
byte failedAttempts = 0;
unsigned long lockoutStart = 0;
bool isLockedOut = false;

// Sécurité MQTT
unsigned long lastCmdTime = 0;

// ── NONCE : BUFFER CIRCULAIRE ────────────────────────────────────
uint32_t nonceHistoryRAM[NONCE_WINDOW_RAM];
byte nonceCountRAM = 0;
byte nonceHeadRAM  = 0;

// ── RECONNEXION ─────────────────────────────────────────────────
unsigned long lastReconnectAttempt = 0;

// ── ÉTAT LCD (anti-redraw) ───────────────────────────────────────
bool  lastLockedState    = true;
byte  lastPwdIndex       = 255;
bool  lastIsLockedOut    = false;
bool  lcdNeedsFullRedraw = true;

// ── MESSAGE LCD NON-BLOQUANT ──────────────────────────────────
struct LcdMessage {
  char line1[17];
  char line2[17];
  unsigned long showUntil;
  bool active;
};
LcdMessage pendingMsg = {"", "", 0, false};

// ── CONSTANTES NVS ──────────────────────────────────────────────
#define PREFS_NS     "lockcfg"
#define KEY_PWD_HASH "ph"
#define KEY_SALT     "ps"
#define KEY_INIT     "init"
#define KEY_NONCE_DB "nonce_db"

#define SALT_LEN 16
#define HASH_LEN 32

uint8_t storedSalt[SALT_LEN];
uint8_t storedHash[HASH_LEN];
bool    pwdSystemReady = false;

// Structure nonces NVS
struct NonceStore {
  uint32_t nonces[NONCE_WINDOW_NVS];
  uint8_t  count;
  uint8_t  head;
};

// ── PROTOTYPES ──────────────────────────────────────────────────
void initHardware();
void initWiFi();
void initMQTT();
void initPasswordSystem();
void initNTP();
void generateSalt(uint8_t* salt);
void computeSHA256(const char* input, uint8_t* output, const uint8_t* salt, size_t saltLen);
bool verifyPassword(const char* input);
bool savePasswordHash(const uint8_t* hash, const uint8_t* salt);
bool loadPasswordHash(uint8_t* hash, uint8_t* salt);
bool reconnectMQTT();
void publishStatus();
void publishAlert(const char* type, const char* detail);
void mqttCallback(char* topic, byte* payload, unsigned int length);
bool isValidCommand(const String& msg, String& action, uint64_t& timestamp, uint32_t& nonce); // ✅ uint64_t
bool isNonceValid(uint32_t nonce);
bool persistNonce(uint32_t nonce);
bool isTimestampValid(uint64_t timestamp); // ✅ uint64_t
void processKey(char key);
void handleDoorToggle();
void resetEnteredPassword();
void updateLCD();
void displayMessage(const char* l1, const char* l2, uint16_t duration);
void servoLock();
void servoUnlock();

// ────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] Smart Lock v2");

  initHardware();
  initPasswordSystem();
  initWiFi();
  initMQTT();

  #if USE_NTP
  initNTP();
  #endif

  displayMessage("Systeme pret", isDoorLocked ? "VERROUILLE" : "OUVERT", 2000);
  publishStatus();
  publishAlert("BOOT", "Systeme demarre - v2.6");
}

// ────────────────────────────────────────────────────────────────
void loop() {
  yield();

  if (!mqttClient.connected()) {
    if (millis() - lastReconnectAttempt >= RECONNECT_INTERVAL_MS) {
      lastReconnectAttempt = millis();
      reconnectMQTT();
    }
  } else {
    mqttClient.loop();
  }

  if (isLockedOut && (millis() - lockoutStart >= LOCKOUT_DURATION_MS)) {
    isLockedOut    = false;
    failedAttempts = 0;
    displayMessage("Debloque", "Reessayez", 1500);
    publishAlert("LOCKOUT_EXPIRED", "Clavier debloque");
  }

  updateLCD();

  char key = keypad.getKey();
  if (key != NO_KEY) {
    delay(50);
    processKey(key);
  }
}

// ────────────────────────────────────────────────────────────────
void initHardware() {
  servo.attach(SERVO_PIN);
  servoLock();

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Door LOCK v2.6");
  delay(1500);
  lcd.clear();

  randomSeed(esp_random());
}

// ────────────────────────────────────────────────────────────────
void initNTP() {
  #if USE_NTP
  Serial.print("[NTP] Synchronisation...");
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
  setenv("TZ", TZ_INFO, 1);

  unsigned long start = millis();
  while (millis() - start < 10000) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      Serial.println("OK");
      Serial.printf("[NTP] Heure: %04d-%02d-%02d %02d:%02d:%02d\n",
        timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      Serial.printf("[NTP] time(nullptr) = %lu\n", (unsigned long)time(nullptr));
      return;
    }
    delay(500);
  }
  Serial.println("ECHEC");
  #endif
}

// ────────────────────────────────────────────────────────────────
void initWiFi() {
  Serial.print("[WiFi] Connexion");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] OK - IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] ECHEC - Mode offline");
    displayMessage("WiFi echec", "Mode offline", 2000);
  }
}

// ────────────────────────────────────────────────────────────────
void initMQTT() {
  wifiClient.setCACert(ROOT_CA);
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(60);
  reconnectMQTT();
}

// ────────────────────────────────────────────────────────────────
void initPasswordSystem() {
  prefs.begin(PREFS_NS, false);

  if (loadPasswordHash(storedHash, storedSalt)) {
    pwdSystemReady = true;
    Serial.println("[PWD] Hash charge depuis NVS");
  } else {
    Serial.println("[PWD] Initialisation premier demarrage");
    generateSalt(storedSalt);
    computeSHA256("1234", storedHash, storedSalt, SALT_LEN);

    if (savePasswordHash(storedHash, storedSalt)) {
      pwdSystemReady = true;
      prefs.putBool(KEY_INIT, true);
      Serial.println("[PWD] Hash initialise et sauvegarde");
    } else {
      Serial.println("[PWD] ERREUR sauvegarde NVS");
      displayMessage("Erreur NVS", "Redemarrage...", 2000);
      delay(2000);
      ESP.restart();
    }
  }
  prefs.end();
}

// ────────────────────────────────────────────────────────────────
void generateSalt(uint8_t* salt) {
  for (int i = 0; i < SALT_LEN; i++) {
    salt[i] = (uint8_t)esp_random();
  }
}

// ────────────────────────────────────────────────────────────────
void computeSHA256(const char* input, uint8_t* output, const uint8_t* salt, size_t saltLen) {
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;

  size_t inputLen = strlen(input);
  size_t totalLen = saltLen + inputLen;
  uint8_t* buffer = (uint8_t*)malloc(totalLen);

  if (!buffer) {
    Serial.println("[SHA256] Erreur allocation memoire");
    return;
  }

  memcpy(buffer, salt, saltLen);
  memcpy(buffer + saltLen, input, inputLen);

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, buffer, totalLen);
  mbedtls_md_finish(&ctx, output);
  mbedtls_md_free(&ctx);

  volatile uint8_t* vbuf = (volatile uint8_t*)buffer;
  for (size_t i = 0; i < totalLen; i++) vbuf[i] = 0;
  free(buffer);
}

// ────────────────────────────────────────────────────────────────
bool verifyPassword(const char* input) {
  if (!pwdSystemReady) return false;

  uint8_t inputHash[HASH_LEN];
  computeSHA256(input, inputHash, storedSalt, SALT_LEN);

  volatile uint8_t result = 0;
  for (int i = 0; i < HASH_LEN; i++) {
    result |= inputHash[i] ^ storedHash[i];
  }

  volatile uint8_t* vh = (volatile uint8_t*)inputHash;
  for (int i = 0; i < HASH_LEN; i++) vh[i] = 0;

  return (result == 0);
}

// ────────────────────────────────────────────────────────────────
bool savePasswordHash(const uint8_t* hash, const uint8_t* salt) {
  prefs.begin(PREFS_NS, false);
  bool ok = prefs.putBytes(KEY_PWD_HASH, hash, HASH_LEN) == HASH_LEN &&
            prefs.putBytes(KEY_SALT, salt, SALT_LEN) == SALT_LEN;
  prefs.end();
  return ok;
}

bool loadPasswordHash(uint8_t* hash, uint8_t* salt) {
  prefs.begin(PREFS_NS, false);
  bool ok = prefs.getBytesLength(KEY_PWD_HASH) == HASH_LEN &&
            prefs.getBytesLength(KEY_SALT) == SALT_LEN &&
            prefs.getBytes(KEY_PWD_HASH, hash, HASH_LEN) == HASH_LEN &&
            prefs.getBytes(KEY_SALT, salt, SALT_LEN) == SALT_LEN;
  prefs.end();
  return ok;
}

// ────────────────────────────────────────────────────────────────
bool reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[MQTT] WiFi non connecte, abandon");
    return false;
  }

  Serial.print("[MQTT] Connexion...");
  String clientId = "ESP32Lock-" + WiFi.macAddress();

  if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
    Serial.println("OK");
    mqttClient.subscribe(TOPIC_COMMAND);
    publishStatus();
    return true;
  }

  Serial.printf("ECHEC (code %d)\n", mqttClient.state());
  return false;
}

// ────────────────────────────────────────────────────────────────
void publishStatus() {
  if (!mqttClient.connected()) return;
  const char* msg = isDoorLocked ? "LOCKED" : "UNLOCKED";
  mqttClient.publish(TOPIC_STATUS, msg, false);
  Serial.printf("[MQTT] Status: %s\n", msg);
}

// ────────────────────────────────────────────────────────────────
void publishAlert(const char* type, const char* detail) {
  if (!mqttClient.connected()) {
    Serial.printf("[ALERT] (offline) %s: %s\n", type, detail);
    return;
  }

  StaticJsonDocument<256> doc;
  doc["type"]   = type;
  doc["detail"] = detail;
  doc["ts"]     = millis();
  doc["locked"] = isDoorLocked;
  doc["fw"]     = "v2.6";

  char buffer[300];
  serializeJson(doc, buffer);
  mqttClient.publish(TOPIC_ALERT, buffer, false);
  Serial.printf("[ALERT] %s: %s\n", type, detail);
}

// ────────────────────────────────────────────────────────────────
// ✅ FIX 1 : uint64_t pour supporter les timestamps Unix en millisecondes
// ✅ FIX 2 : fallback si NTP non sync (Wokwi)
// ✅ FIX 3 : fenêtre 60s (doublon supprimé)
bool isTimestampValid(uint64_t timestamp) {
  if (timestamp == 0) return true;

  time_t now = time(nullptr);

  // Fallback Wokwi : si NTP non sync, time() retourne une valeur < 2023
  if (now < 1700000000UL) {
    Serial.println("[TS] NTP non sync — commande acceptee");
    return true;
  }

  // Timestamp PC en ms → convertir en secondes
  uint64_t tsSeconds = timestamp / 1000;

  uint64_t diff = (tsSeconds > (uint64_t)now)
                  ? tsSeconds - (uint64_t)now
                  : (uint64_t)now - tsSeconds;

  Serial.printf("[TS] PC=%llu ESP=%lu diff=%llus (max=%lus)\n",
                tsSeconds, (unsigned long)now, diff, (unsigned long)(CMD_MAX_AGE_MS / 1000));

  if (diff > (CMD_MAX_AGE_MS / 1000)) {
    Serial.printf("[TS] Rejete — ecart: %llus\n", diff);
    return false;
  }
  return true;
}

// ────────────────────────────────────────────────────────────────
bool persistNonce(uint32_t nonce) {
  prefs.begin(PREFS_NS, false);
  NonceStore store;

  if (prefs.getBytesLength(KEY_NONCE_DB) == sizeof(NonceStore)) {
    prefs.getBytes(KEY_NONCE_DB, &store, sizeof(NonceStore));
  } else {
    store.count = 0;
    store.head  = 0;
    memset(store.nonces, 0, sizeof(store.nonces));
  }

  store.nonces[store.head] = nonce;
  store.head = (store.head + 1) % NONCE_WINDOW_NVS;
  if (store.count < NONCE_WINDOW_NVS) store.count++;

  bool ok = prefs.putBytes(KEY_NONCE_DB, &store, sizeof(NonceStore)) == sizeof(NonceStore);
  prefs.end();
  return ok;
}

// ────────────────────────────────────────────────────────────────
bool isNonceValid(uint32_t nonce) {
  if (nonce == 0) {
    Serial.println("[NONCE] Format simple (pas d'anti-replay)");
    return true;
  }

  // 1. Vérification RAM
  for (byte i = 0; i < nonceCountRAM; i++) {
    byte idx = (nonceHeadRAM + i) % NONCE_WINDOW_RAM;
    if (nonceHistoryRAM[idx] == nonce) {
      Serial.println("[NONCE] Replay detecte en RAM");
      return false;
    }
  }

  // 2. Vérification NVS
  prefs.begin(PREFS_NS, true);
  if (prefs.getBytesLength(KEY_NONCE_DB) == sizeof(NonceStore)) {
    NonceStore store;
    prefs.getBytes(KEY_NONCE_DB, &store, sizeof(NonceStore));
    for (uint8_t i = 0; i < store.count; i++) {
      if (store.nonces[i] == nonce) {
        prefs.end();
        Serial.println("[NONCE] Replay detecte en NVS");
        return false;
      }
    }
  }
  prefs.end();

  // 3. Ajout RAM
  nonceHistoryRAM[nonceHeadRAM] = nonce;
  nonceHeadRAM = (nonceHeadRAM + 1) % NONCE_WINDOW_RAM;
  if (nonceCountRAM < NONCE_WINDOW_RAM) nonceCountRAM++;

  // 4. Persistance NVS
  persistNonce(nonce);

  Serial.printf("[NONCE] Accepte (RAM:%d NVS:%d)\n", nonceCountRAM, NONCE_WINDOW_NVS);
  return true;
}

// ────────────────────────────────────────────────────────────────
// ✅ FIX : uint64_t pour timestamp + strtoull
bool isValidCommand(const String& msg, String& action, uint64_t& timestamp, uint32_t& nonce) {
  String trimmed = msg;
  trimmed.trim();

  if (trimmed.length() == 0) {
    publishAlert("INVALID_FORMAT", "Payload vide");
    return false;
  }

  int sep1 = trimmed.indexOf(':');

  // Format simple : "ACTION"
  if (sep1 < 0) {
    action    = trimmed;
    timestamp = 0;
    nonce     = 0;
    Serial.printf("[CMD] Format simple: '%s' (pas d'anti-replay)\n", action.c_str());

    unsigned long now = millis();
    if (now - lastCmdTime < MIN_CMD_INTERVAL_MS) {
      publishAlert("FLOOD_DETECTED", "Trop de commandes");
      return false;
    }
    lastCmdTime = now;
    return true;
  }

  // Format complet : "ACTION:TIMESTAMP:NONCE"
  int sep2 = trimmed.indexOf(':', sep1 + 1);

  if (sep2 <= sep1 || sep2 >= (int)trimmed.length() - 1) {
    Serial.printf("[CMD] Format invalide: '%s'\n", trimmed.c_str());
    publishAlert("INVALID_FORMAT", "Syntaxe: ACTION:TS:NONCE");
    return false;
  }

  action    = trimmed.substring(0, sep1);
  // ✅ strtoull au lieu de strtoul (supporte les grands nombres 64 bits)
  timestamp = strtoull(trimmed.substring(sep1 + 1, sep2).c_str(), nullptr, 10);
  nonce     = strtoul(trimmed.substring(sep2 + 1).c_str(), nullptr, 10);

  if (!isNonceValid(nonce)) {
    publishAlert("REPLAY_ATTACK", "Nonce rejoue");
    return false;
  }

  if (!isTimestampValid(timestamp)) {
    publishAlert("EXPIRED_CMD", "Commande perimee");
    return false;
  }

  unsigned long now = millis();
  if (now - lastCmdTime < MIN_CMD_INTERVAL_MS) {
    publishAlert("FLOOD_DETECTED", "Trop de commandes");
    return false;
  }

  lastCmdTime = now;
  Serial.printf("[CMD] Valide: '%s' TS:%llu NONCE:%lu\n", action.c_str(), timestamp, nonce);
  return true;
}

// ────────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != TOPIC_COMMAND) return;

  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("[MQTT] Recu: '%s'\n", msg.c_str());

  String action;
  uint64_t timestamp;
  uint32_t nonce;

  if (!isValidCommand(msg, action, timestamp, nonce)) {
    Serial.println("[MQTT] Commande rejetee");
    return;
  }

  if (action == "UNLOCK") {
    if (!isDoorLocked) {
      // ✅ Déjà ouverte → on informe sans erreur
      publishAlert("ALREADY_UNLOCKED", "Porte deja ouverte");
      publishStatus();
    } else {
      servoUnlock();
      isDoorLocked = false;
      displayMessage("Commande MQTT", "OUVERT", 1500);
      publishStatus();
      publishAlert("UNLOCK_MQTT", "Ouverture distante");
    }

  } else if (action == "LOCK") {
    if (isDoorLocked) {
      // ✅ Déjà verrouillée → on informe sans erreur
      publishAlert("ALREADY_LOCKED", "Porte deja verrouillee");
      publishStatus();
    } else {
      servoLock();
      isDoorLocked = true;
      displayMessage("Commande MQTT", "VERROUILLE", 1500);
      publishStatus();
      publishAlert("LOCK_MQTT", "Fermeture distante");
    }

  } else if (action == "STATUS") {
    publishStatus();

  } else {
    // Vraie commande inconnue (ex: "OPEN", "RESET"...)
    publishAlert("UNKNOWN_CMD", action.c_str());
  }

}
// ────────────────────────────────────────────────────────────────
void servoLock() {
  servo.write(10);
  delay(300);
}

void servoUnlock() {
  servo.write(180);
  delay(300);
}

// ────────────────────────────────────────────────────────────────
void processKey(char key) {
  if (isLockedOut) return;

  if (key == '#') {
    resetEnteredPassword();
  } else if (key == '*') {
    handleDoorToggle();
  } else if (pwdIndex < PASSWORD_MAX_LENGTH && key >= '0' && key <= '9') {
    enteredPassword[pwdIndex++] = key;
    enteredPassword[pwdIndex]   = '\0';
  }
}
// ────────────────────────────────────────────────────────────────
void handleDoorToggle() {
  if (isLockedOut) {
    unsigned long remaining = (LOCKOUT_DURATION_MS - (millis() - lockoutStart)) / 1000 + 1;
    char buf[20];
    snprintf(buf, sizeof(buf), "Att. %lus", remaining);
    displayMessage("BLOQUE", buf, 1000);
    publishAlert("LOCKOUT_ACTIVE", "Tentative clavier bloquee");
    return;
  }

  if (pwdIndex < PASSWORD_MIN_LENGTH) {
    displayMessage("Code trop court", "", 1200);
    publishAlert("SHORT_PWD", "Longueur insuffisante");
    resetEnteredPassword();
    return;
  }

  if (verifyPassword(enteredPassword)) {
    failedAttempts = 0;

    if (isDoorLocked) {
      isDoorLocked = false;
      servoUnlock();
      displayMessage("Correct", "OUVERT", 1200);
      publishAlert("UNLOCK_LOCAL", "Ouverture par clavier");
    } else {
      isDoorLocked = true;
      servoLock();
      displayMessage("Correct", "VERROUILLE", 1200);
      publishAlert("LOCK_LOCAL", "Fermeture par clavier");
    }
    publishStatus();

  } else {
    failedAttempts++;
    Serial.printf("[PWD] Echec %d/%d\n", failedAttempts, MAX_ATTEMPTS);

    if (failedAttempts >= MAX_ATTEMPTS) {
      isLockedOut  = true;
      lockoutStart = millis();
      displayMessage("BLOQUE 30s", "Trop d'erreurs", 2000);
      publishAlert("BRUTE_FORCE", "3 echecs consecutifs");
    } else {
      char buf[20];
      snprintf(buf, sizeof(buf), "%d/%d", failedAttempts, MAX_ATTEMPTS);
      displayMessage("Code faux", buf, 1000);
      publishAlert("WRONG_PWD", buf);
    }
  }

  resetEnteredPassword();
}

// ────────────────────────────────────────────────────────────────
void resetEnteredPassword() {
  volatile char* vp = (volatile char*)enteredPassword;
  for (byte i = 0; i <= PASSWORD_MAX_LENGTH; i++) vp[i] = 0;
  pwdIndex     = 0;
  lastPwdIndex = 255;
}

// ────────────────────────────────────────────────────────────────
void displayMessage(const char* l1, const char* l2, uint16_t duration) {
  strncpy(pendingMsg.line1, l1, 16);
  strncpy(pendingMsg.line2, l2, 16);
  pendingMsg.line1[16] = '\0';
  pendingMsg.line2[16] = '\0';
  pendingMsg.showUntil = millis() + duration;
  pendingMsg.active    = true;

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(pendingMsg.line1);
  lcd.setCursor(0, 1); lcd.print(pendingMsg.line2);

  lcdNeedsFullRedraw = true;
  lastPwdIndex       = 255;
}

// ────────────────────────────────────────────────────────────────
void updateLCD() {
  if (pendingMsg.active) {
    if (millis() < pendingMsg.showUntil) return;
    pendingMsg.active  = false;
    lcdNeedsFullRedraw = true;
    lastPwdIndex       = 255;
    lcd.clear();
  }

  if (isLockedOut) {
    if (!lastIsLockedOut || lcdNeedsFullRedraw) {
      lcd.setCursor(0, 0);
      lcd.print("BLOQUE          ");
      lastIsLockedOut    = true;
      lcdNeedsFullRedraw = false;
    }
    unsigned long remaining = (LOCKOUT_DURATION_MS - (millis() - lockoutStart)) / 1000 + 1;
    char buf[17];
    snprintf(buf, sizeof(buf), "Att. %02lus sec     ", remaining);
    lcd.setCursor(0, 1);
    lcd.print(buf);
    return;
  }

  if (isDoorLocked != lastLockedState || lastIsLockedOut || lcdNeedsFullRedraw) {
    lcd.setCursor(0, 0);
    lcd.print(isDoorLocked ? "  DOOR LOCKED   " : "  DOOR OPEN     ");
    lastLockedState    = isDoorLocked;
    lastIsLockedOut    = false;
    lcdNeedsFullRedraw = false;
    lastPwdIndex       = 255;
  }

  if (pwdIndex != lastPwdIndex) {
    lcd.setCursor(0, 1);
    if (pwdIndex == 0) {
      lcd.print("Code:           ");
    } else {
      lcd.print("Code: ");
      for (byte i = 0; i < pwdIndex; i++) lcd.print('*');
      for (byte i = pwdIndex; i < PASSWORD_MAX_LENGTH; i++) lcd.print(' ');
      lcd.print("  ");
    }
    lastPwdIndex = pwdIndex;
  }
}
