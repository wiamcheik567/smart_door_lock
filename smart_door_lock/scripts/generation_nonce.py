#!/usr/bin/env python3
"""
🔐 Smart Lock ESP32 - Client MQTT Sécurisé
• Génère automatiquement des nonces cryptographiques (anti-replay)
• Connexion TLS vérifiée
• Écoute des réponses door/status & door/alert
• Compatible paho-mqtt 1.6+ / 2.0+
"""

import ssl
import sys
import time
import json
import secrets
import argparse
import paho.mqtt.client as mqtt

# ── CONFIGURATION ───────────────────────────────────────────────
BROKER     = "yf4fb665.ala.eu-central-1.emqxsl.com"
PORT       = 8883
USERNAME   = "smart_lock"
PASSWORD   = "IoT2000****"  # ⚠️ À modifier si vous changez le mot de passe
TOPIC_CMD  = "door/command"
TOPIC_STAT = "door/status"
TOPIC_ALERT= "door/alert"

def generate_nonce() -> int:
    """Génère un nonce 32-bit non nul (1 à 4_294_967_294)"""
    return secrets.randbelow(2**32 - 1) + 1

class SmartLockClient:
    def __init__(self):
        self.client = mqtt.Client(client_id=f"ESP32Ctrl_{secrets.token_hex(4)}")
        self.client.username_pw_set(USERNAME, PASSWORD)
        
        # TLS avec certificats racine système (Let's Encrypt / DigiCert)
        self.client.tls_set(tls_version=ssl.PROTOCOL_TLS_CLIENT)
        # self.client.tls_insecure_set(True)  # ⚠️ Décommenter SEULEMENT si erreur SSL
        
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.connected = False

    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.connected = True
            print("✅ Connecté au broker MQTT")
            client.subscribe([(TOPIC_STAT, 1), (TOPIC_ALERT, 1)])
        else:
            print(f"❌ Échec connexion (code {rc})")
            sys.exit(1)

    def on_message(self, client, userdata, msg):
        payload = msg.payload.decode()
        print(f"\n📥 [{msg.topic}]")
        
        if msg.topic == TOPIC_ALERT:
            try:
                data = json.loads(payload)
                print(f"   🏷️ Type   : {data.get('type', '?')}")
                print(f"   📝 Détail : {data.get('detail', '?')}")
                print(f"   🔒 État   : {data.get('locked', '?')}")
            except json.JSONDecodeError:
                print(f"   📄 {payload}")
        else:
            print(f"   📄 {payload}")

    def connect(self):
        print(f"🔗 Connexion à {BROKER}:{PORT} (TLS)...")
        self.client.connect(BROKER, PORT, keepalive=60)
        self.client.loop_start()
        
        # Attendre la connexion (max 5s)
        for _ in range(50):
            if self.connected:
                return
            time.sleep(0.1)
        print("⏱️ Timeout connexion")
        sys.exit(1)

    def send_command(self, action: str, use_timestamp: bool = False):
        nonce = generate_nonce()
        # TS=0 désactive le check temporel (recommandé si ESP32 non sync NTP)
        ts = int(time.time() * 1000) if use_timestamp else 0
        payload = f"{action}:{ts}:{nonce}"

        print(f"📤 Publie sur {TOPIC_CMD}")
        print(f"   Payload : {payload}")
        
        info = self.client.publish(TOPIC_CMD, payload, qos=1)
        info.wait_for_publish(timeout=10)
        print(f"✅ Envoyé (nonce={nonce})")

    def stop(self):
        self.client.loop_stop()
        self.client.disconnect()
        print("🔌 Déconnecté.")

def main():
    parser = argparse.ArgumentParser(description="🔐 Client MQTT pour Smart Lock ESP32")
    parser.add_argument("action", choices=["UNLOCK", "LOCK", "STATUS"], help="Action à envoyer")
    parser.add_argument("--ts", action="store_true", help="Envoyer timestamp Unix (⚠️ nécessite NTP sur ESP32)")
    parser.add_argument("--listen", action="store_true", help="Rester en écoute après envoi (Ctrl+C pour quitter)")
    args = parser.parse_args()

    client = SmartLockClient()
    client.connect()

    try:
        client.send_command(args.action, args.ts)

        if args.listen:
            print("👂 Écoute active... (Ctrl+C pour quitter)")
            while True:
                time.sleep(1)
        else:
            print("⏳ Attente des réponses (3s)...")
            time.sleep(3)
            
    except KeyboardInterrupt:
        print("\n🛑 Interruption utilisateur")
    finally:
        client.stop()

if __name__ == "__main__":
    main()