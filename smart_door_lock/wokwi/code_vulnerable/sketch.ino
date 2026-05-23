#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <Password.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

#define servoPin 15


const char* WIFI_SSID     = "Wokwi-GUEST"; 
const char* WIFI_PASSWORD = "";          
// ── EMQX Cloud (MQTT over TLS) Configuration ────────────────────────
const char* MQTT_BROKER   = "yf4fb665.ala.eu-central-1.emqxsl.com";
const int   MQTT_PORT     = 8883;
const char* TOPIC_STATUS  = "door/status";
const char* TOPIC_COMMAND = "door/command";


const char* MQTT_USER = "smart_lock";
const char* MQTT_PASS = "IoT2000****"; 

WiFiClientSecure wifiClient;
PubSubClient     mqttClient(wifiClient);

Servo servo;
LiquidCrystal_I2C lcd(0x27, 16, 2);

const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {23,19,18,5};
byte colPins[COLS]  = {17,16,4,2};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

bool     isDoorLocked          = true;
// ATTENTION: Le mot de passe "1234" est codé en dur. Implémentez un mécanisme pour le changer
// et le stocker de manière sécurisée (NVS) pour la production.
Password password              = Password("1234");
byte     maxPasswordLength     = 6;
byte     currentPasswordLength = 0;

// ── Variables pour la gestion non bloquante ────────────────────────
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL = 5000; // 5 secondes entre chaque tentative de reconnexion MQTT

unsigned long lastKeypadReadTime = 0;
const unsigned long KEYPAD_DEBOUNCE_DELAY = 150; // Délai anti-rebond pour le clavier

unsigned long messageDisplayStartTime = 0;
bool isDisplayingMessage = false;
const unsigned long MESSAGE_DISPLAY_DURATION = 2000; // Durée d'affichage des messages LCD

// ── Prototypes ────────────────────────────────────────
void connectWiFi();
bool reconnectMQTT();
void publishStatus();
void mqttCallback(char*, byte*, unsigned int);
void processNumberKey(char);
void handleDoorToggle();
void resetPassword();
void displayMessage(const char*, const char*);
void updateLCDStatus(); // Nouvelle fonction pour mettre à jour l'état de la porte sur l'LCD

// ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  servo.attach(servoPin);
  servo.write(10); // Assure que la porte est verrouillée au démarrage
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0); lcd.print("Door LOCK System");
  delay(MESSAGE_DISPLAY_DURATION); // Délai initial bloquant acceptable au démarrage
  lcd.clear();

  connectWiFi();

  wifiClient.setInsecure();
  
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(60); // Augmente le Keep-Alive à 60 secondes pour plus de tolérance

  // Première tentative de connexion MQTT (non bloquante)
  reconnectMQTT();
}

// ─────────────────────────────────────────────────────
void loop() {
  // ── Gestion connexion MQTT non bloquante ──
  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt >= RECONNECT_INTERVAL) {
      lastReconnectAttempt = now;
      // Tente de se reconnecter, si réussi, met à jour le timer
      if (reconnectMQTT()) {
        lastReconnectAttempt = millis(); // Met à jour pour respecter l'intervalle
      }
    }
  } else {
    mqttClient.loop(); // Maintien de la connexion MQTT et traitement des messages
  }

  // ── Gestion de l'affichage des messages LCD non bloquant ──
  if (isDisplayingMessage && (millis() - messageDisplayStartTime >= MESSAGE_DISPLAY_DURATION)) {
    lcd.clear();
    isDisplayingMessage = false;
    updateLCDStatus(); // Met à jour l'affichage avec l'état de la porte après le message
  }

  // ── Mise à jour de l'état de la porte sur l'LCD (si aucun message n'est affiché) ──
  if (!isDisplayingMessage) {
    updateLCDStatus();
  }

  // ── Lecture clavier non bloquante ──
  unsigned long now = millis();
  if (now - lastKeypadReadTime >= KEYPAD_DEBOUNCE_DELAY) {
    char key = keypad.getKey();
    if (key != NO_KEY) {
      lastKeypadReadTime = now; // Réinitialise le timer après une lecture de touche
      if      (key == '#') resetPassword();
      else if (key == '*') handleDoorToggle();
      else                 processNumberKey(key);
    }
  }
}

// ─────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print("Connexion WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connecté : " + WiFi.localIP().toString());
}

// ─────────────────────────────────────────────────────
bool reconnectMQTT() {
  Serial.print("Tentative connexion MQTT... ");
  // Générer un Client ID unique pour éviter les conflits si plusieurs appareils utilisent le même code
  String clientId = "ESP32_DoorLock-";
  clientId += String(WiFi.macAddress()); // Utilise l'adresse MAC pour un ID unique

  if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
    Serial.println("Connecté !");
    mqttClient.subscribe(TOPIC_COMMAND);
    publishStatus();
    return true;
  }
  Serial.printf("Échec — état broker : %d\n", mqttClient.state());
  return false;
}

// ─────────────────────────────────────────────────────
void publishStatus() {
  const char* msg = isDoorLocked ? "LOCKED" : "UNLOCKED";
  mqttClient.publish(TOPIC_STATUS, msg, true);  // retained = true
  Serial.printf("Publié [%s] : %s\n", TOPIC_STATUS, msg);
}

// ─────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.printf("Message reçu [%s] : %s\n", topic, msg.c_str());

  if (String(topic) == TOPIC_COMMAND) {
    if (msg == "UNLOCK" && isDoorLocked) {
      isDoorLocked = false;
      servo.write(180);
      displayMessage("Remote Command", "UNLOCKED");
      publishStatus();
    } else if (msg == "LOCK" && !isDoorLocked) {
      isDoorLocked = true;
      servo.write(10);
      displayMessage("Remote Command", "LOCKED");
      publishStatus();
    }
  }
}

// ─────────────────────────────────────────────────────
void processNumberKey(char key) {
  if (currentPasswordLength < maxPasswordLength) {
    lcd.setCursor(currentPasswordLength + 5, 1);
    lcd.print("*");
    password.append(key);
    currentPasswordLength++;
  }
}

void handleDoorToggle() {
  if (password.evaluate()) {
    isDoorLocked = !isDoorLocked;
    isDoorLocked ? servo.write(10) : servo.write(180);
    isDoorLocked ? displayMessage("Password Correct", "LOCKED")
                 : displayMessage("Password Correct", "UNLOCKED");
    publishStatus();
  } else {
    displayMessage("Wrong Password", "Try Again");
  }
  resetPassword();
}

void resetPassword() {
  password.reset();
  currentPasswordLength = 0;
  lcd.clear();
  lcd.setCursor(0, 0);
  updateLCDStatus(); // Met à jour l'affichage après la réinitialisation
}

void displayMessage(const char* l1, const char* l2) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(l1);
  lcd.setCursor(0, 1); lcd.print(l2);
  messageDisplayStartTime = millis();
  isDisplayingMessage = true;
}

void updateLCDStatus() {
  lcd.setCursor(3, 0);
  isDoorLocked ? lcd.print("DOOR LOCKED") : lcd.print("DOOR OPEN  ");
  // Affiche les astérisques du mot de passe si en cours de saisie
  if (currentPasswordLength > 0) {
    lcd.setCursor(5, 1);
    for (byte i = 0; i < currentPasswordLength; i++) {
      lcd.print("*");
    }
    // Efface les caractères restants si le mot de passe est plus court que maxPasswordLength
    for (byte i = currentPasswordLength; i < maxPasswordLength; i++) {
      lcd.print(" ");
    }
  } else {
    // Efface la ligne du mot de passe si aucun mot de passe n'est saisi
    lcd.setCursor(0, 1);
    lcd.print("                ");
  }
}
