# Smart Door Lock  ESP32 + MQTT

Étude de sécurité d'un système de verrouillage intelligent basé sur ESP32, comparant une version vulnérable et une version sécurisée communiquant via MQTT over TLS.

---

##  Structure du projet

```
smart_door_lock/
 wokwi/
�    code_vulnerable/       # Version non sécurisée (démonstration d'attaques)
�   �    sketch.ino
�   �    diagram.json
�   �    libraries.txt
�   �    wokwi-project.txt
�    code_secure/           # Version sécurisée (contre-mesures implémentées)
�        sketch.ino
�        diagram.json
�        libraries.txt
�        wokwi-project.txt
 scripts/
�    flooding.py            # Attaque : maintien forcé de la porte ouverte
�    mqtt_replay_attack.py  # Attaque : rejeu de commandes MQTT
�    generation_nonce.py    # Client sécurisé avec nonces cryptographiques
 nodered/
     flows.json             # Dashboard Node-RED (surveillance & contrôle)
```

---

##  Matériel & Technologies

| Composant | Détail |
|-----------|--------|
| Microcontrôleur | ESP32 (simulé sur [Wokwi](https://wokwi.com)) |
| Clavier | Keypad matriciel 4×4 |
| Affichage | LCD 16×2 (I2C, adresse 0x27) |
| Actionneur | Servo-moteur (pin 15) |
| Protocole | MQTT over TLS (port 8883) |
| Broker | EMQX Cloud (`eu-central-1`) |
| Dashboard | Node-RED avec UI (boutons, LED, historique) |

---

##  Bibliothèques Arduino

### Version vulnérable
- `ESP32Servo`
- `LiquidCrystal I2C`
- `Keypad`
- `Password`
- `PubSubClient`

### Version sécurisée (ajouts)
- `ArduinoJson`  parsing/validation des commandes JSON
- `mbedtls/md.h`  HMAC-SHA256 pour signature des commandes
- `Preferences`  stockage sécurisé des nonces en NVS
- `time.h`  synchronisation NTP pour horodatage

---

##  Version vulnérable

La version vulnérable illustre les failles courantes d'un système IoT mal configuré.

**Failles présentes :**
- Mot de passe codé en dur (`"1234"`) sans possibilité de changement
- Commandes MQTT en texte brut (`UNLOCK` / `LOCK`)
- Aucune authentification des commandes re�ues
- Aucune protection anti-rejeu
- Pas de limite de tentatives de connexion

---

##  Version sécurisée

La version sécurisée implémente plusieurs couches de défense :

| Mécanisme | Description |
|-----------|-------------|
| **TLS 8883** | Chiffrement du canal MQTT (certificat DigiCert) |
| **Auth MQTT** | Identifiants `smart_lock` / mot de passe |
| **Nonces** | Anti-rejeu  fen�tre RAM (100) + NVS (20) |
| **Horodatage** | Commandes rejetées si âge > 60 secondes |
| **Rate limiting** | Intervalle minimum de 2 s entre commandes |
| **Lockout** | Verrouillage 30 s après 3 tentatives incorrectes |
| **NVS** | Stockage sécurisé du mot de passe (non codé en dur) |
| **LWT** | Message de dernière volonté MQTT en cas de déconnexion |

---

##  Scripts d'attaque

### `flooding.py`
Écoute le topic `door/status` et maintient la porte ouverte de force dès qu'un statut `UNLOCKED` est détecté. Démontre l'absence de contrôle d'accès sur les commandes.

### `mqtt_replay_attack.py`
Rejoue 5 fois la commande `UNLOCK` avec un délai de 2 secondes entre chaque envoi. Démontre la vulnérabilité � l'absence de nonces/timestamps.

### `generation_nonce.py`
Client MQTT sécurisé Python pour interagir avec la version sécurisée. Génère automatiquement des nonces cryptographiques 32-bit (via `secrets`) et établit une connexion TLS vérifiée.

```bash
# Utilisation
pip install paho-mqtt
python generation_nonce.py --action unlock
python generation_nonce.py --action lock
```

---

##  Dashboard Node-RED

Le fichier `nodered/flows.json` fournit une interface de supervision avec :
- **LED de statut**  état de la porte en temps réel
- **Boutons LOCK / UNLOCK**  contrôle distant
- **Historique des alertes**  log des événements `door/alert`
- **Connexion TLS**  broker EMQX Cloud sécurisé

Pour importer : Node-RED � Menu � **Import** � coller le contenu de `flows.json`.

---

##  Simulation sur Wokwi

1. Ouvrir [wokwi.com](https://wokwi.com)
2. Créer un nouveau projet ESP32
3. Importer `sketch.ino`, `diagram.json` et `libraries.txt`
4. Lancer la simulation
5. Utiliser le clavier virtuel pour saisir le code (`1234` pour la version vulnérable)

---

##  Résumé de l'étude

Ce projet démontre l'impact concret de l'absence de mesures de sécurité sur un système IoT en comparant deux configurations identiques sur le plan fonctionnel :

- **Version vulnérable**  10 vulnérabilités identifiées et exploitées
- **Version sécurisée**  contre-mesures actives � chaque niveau (transport, authentification, intégrité, disponibilité)

---

##  Avertissement

Les scripts d'attaque fournis sont strictement destinés � des fins pédagogiques dans un environnement de simulation contrôlé. Ne pas utiliser sur des systèmes réels sans autorisation explicite.
