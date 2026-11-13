# ⚡ ESP32 Dual Power Meter & Solar Diverter

Système embarqué haute performance de mesure de puissance active bidirectionnelle (230 V AC) pour deux circuits indépendants, conçu pour **ESP32-WROOM-32** sous **ESP-IDF v5.x / v6.x**.

Le projet intègre une interface web réactive servie directement par l'ESP32 via **LittleFS**, un routage automatique de surplus solaire par **optocoupleur**, une publication des données via **MQTT**, une historisation multi-échelles synchronisée par **NTP**, ainsi que les mises à jour **OTA (Firmware & Interface Web)** directement depuis le navigateur.

---

## 📋 Sommaire

- [Fonctionnalités principales](#-fonctionnalités-principales)
- [Architecture Matérielle & Câblage](#-architecture-matérielle--câblage)
- [Architecture Logicielle & Multitâche](#-architecture-logicielle--multitâche)
- [Interface Web Embarquée](#-interface-web-embarquée)
- [Structure du Projet](#-structure-du-projet)
- [Compilation & Flash](#-compilation--flash)
- [Configuration & Mises à jour (OTA)](#-configuration--mises-à-jour-ota)

---

## 🚀 Fonctionnalités principales

1. **Mesure de puissance active 2 voies ($P = U \times I$) :**
   - Calcul en temps réel de la puissance active instantanée avec détection du signe (consommation $> 0$, injection solaire $< 0$).
   - Calcul des tensions et courants efficaces ($V_{\text{RMS}}$, $I_{1\text{RMS}}$, $I_{2\text{RMS}}$).
   - Échantillonnage haute cadence sur convertisseur ADC externe **ADS1115** (16 bits, I2C à 400 kHz).

2. **Routage de surplus solaire (Optocoupleur) :**
   - Pilotage d'un optocoupleur sur GPIO dédié dès détection d'injection sur le réseau.
   - Hystérésis paramétrable (seuils ON et OFF configurables depuis l'interface web et stockés en **NVS**).
   - Prévient le cyclage rapide (*pompage*) lors des passages nuageux.

3. **Architecture Multitâche Dual-Core FreeRTOS :**
   - **Cœur 1 (Temps réel strict) :** boucle d'échantillonnage ADC, calculs RMS, intégration de puissance et pilotage du GPIO optocoupleur.
   - **Cœur 0 (Réseau & Web) :** pile Wi-Fi, serveur HTTP, publication MQTT et synchronisation horaire SNTP.
   - Données partagées protégées par **Mutex** (exclusion mutuelle).

4. **Connectivité & Services Réseau :**
   - **Wi-Fi Manager résilient :** Connexion en mode Station (STA) avec gestion du décodage URL (espaces et caractères spéciaux). En cas de 5 échecs consécutifs, bascule automatique en mode mixte (`APSTA`) diffusant le point d'accès de secours **`ESP32-PowerMeter`** (`192.168.4.1`) tout en conservant les identifiants en NVS et en testant la reconnexion à la box toutes les 60 secondes.
   - **Synchronisation SNTP :** Horodatage précis des mesures avec gestion automatique des fuseaux horaires et passages heure d'été/hiver.
   - **Client MQTT :** Démarrage synchronisé dès l'obtention d'une adresse IP et publication des puissances et états de l'optocoupleur (compatible Home Assistant, Node-RED, etc.).

5. **Interface Web, Historique & Mises à jour OTA :**
   - Fichiers statiques (`index.html`, `style.css`, `script.js`) empaquetés dans l'image **LittleFS** lors de la compilation (`FLASH_IN_PROJECT`).
   - Cartes pliables/escamotables pour les réglages (Wi-Fi, MQTT, Seuils optocoupleur, Mises à jour).
   - Graphique interactif (**Chart.js**) affichant l'historique sur 3 périodes : **12 heures**, **7 jours** et **12 mois**.
   - **Mise à jour sans fil (OTA) intégrée :** Téléversement simultané de plusieurs fichiers web (`index.html`, `style.css`, `script.js`) sur LittleFS et flashage du firmware (`.bin`) sur double partition (`ota_0` / `ota_1`) avec barre de progression en temps réel.

---

## 🔌 Architecture Matérielle & Câblage

### Composants requis

- **1x ESP32-WROOM-32** (4 Mo Flash)
- **1x Module ADC ADS1115** (I2C, 16 bits)
- **1x Capteur de tension AC ZMPT101B**
- **2x Pinces ampèremétriques** (ex: XT-SCT-T24 ou SCT-013-000)
- **1x Optocoupleur** (ex: PC817 ou module optocoupleur isolé)
- Résistances de charge (*burden*) et pont diviseur / offset DC à 1,65 V pour centrer les alternances AC sur la plage analogique.

### Brochage (Pinout ESP32-WROOM-32)

| Composant | Signal / Fonction | Broche ESP32 | Remarques |
| :--- | :--- | :--- | :--- |
| **ADS1115** | I2C SDA | **GPIO 21** | Ligne de données I2C |
| **ADS1115** | I2C SCL | **GPIO 22** | Horloge I2C (400 kHz) |
| **ADS1115** | VDD / GND | **3.3V** / **GND** | Alimenter en 3.3V pour cohérence I2C |
| **Optocoupleur** | Commande Anode | **GPIO 18** | Via résistance série (~330 $\Omega$) |

### Affectation des entrées analogiques (ADS1115)

| Entrée ADS1115 | Mesure | Capteur associé |
| :--- | :--- | :--- |
| **A0** | Tension AC Réseau ($V$) | Module transformateur ZMPT101B |
| **A1** | Courant Circuit 1 ($I_1$) | Pince ampèremétrique 1 |
| **A2** | Courant Circuit 2 ($I_2$) | Pince ampèremétrique 2 |
| **A3** | *Libre / Réserve* | — |

---

## 🧠 Architecture Logicielle & Multitâche

```text
                            ESP32 DUAL CORE
       ┌───────────────────────────────┐   ┌───────────────────────────────┐
       │            CORE 0             │   │            CORE 1             │
       │       (Réseau / Système)      │   │          (Temps Réel)         │
       ├───────────────────────────────┤   ├───────────────────────────────┤
       │ • Pile Wi-Fi (STA / APSTA)    │   │ • Acquisition ADC ADS1115     │
       │ • Pile lwIP TCP/IP            │   │ • Intégration P = V x I       │
       │ • Serveur HTTPD & OTA         │   │ • Calculs RMS (Tension/Irms)  │
       │ • Client MQTT                 │   │ • Hystérésis Optocoupleur     │
       │ • Synchronisation SNTP        │   │ • Agrégation Historique       │
       └───────────────┬───────────────┘   └───────────────┬───────────────┘
                       │                                   │
                       └─────────────► [ MUTEX ] ◄─────────┘
                                 Protection des lectures
                                  et écritures partagées
```

### Table de partitions Flash (4 Mo avec support OTA)

| Label | Type | Sous-type | Offset | Taille | Rôle |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `nvs` | data | nvs | `0x9000` | 24 Ko | Stockage des paramètres (Wi-Fi, MQTT, seuils) |
| `otadata` | data | ota | `0xf000` | 8 Ko | Sélection de la partition de démarrage active |
| `phy_init` | data | phy | `0x11000` | 4 Ko | Données d'étalonnage RF |
| `ota_0` | app | ota_0 | `0x20000` | 1,5 Mo | Emplacement Firmware A |
| `ota_1` | app | ota_1 | `0x1A0000` | 1,5 Mo | Emplacement Firmware B |
| `storage` | data | littlefs | `0x320000` | 896 Ko | Système de fichiers embarqué (Interface Web) |

---

## 🌐 Interface Web Embarquée

L'interface web hébergée sur l'ESP32 offre une expérience utilisateur moderne et autonome :
- **Carte des mesures :** Affichage en grand format de la puissance ($W$), des courants ($A$), de la tension secteur ($V$) et de l'état du relais.
- **Coloration dynamique :** Affichage bleu lors de la consommation réseau, et passage immédiat en vert avec badge *« Injection Solaire »* si la puissance devient négative.
- **Accordéons escamotables :** Les cartes d'historique, de réglages et de mise à jour se replient pour conserver une vue dégagée sur mobile.
- **Graphique dynamique :** Visualisation des 12 dernières heures, 7 derniers jours ou 12 derniers mois avec mise à l'échelle temporelle.
- **Gestionnaire de fichiers & OTA :** Liste en direct des fichiers stockés sur LittleFS, sélection multiple pour mise à jour web et flashage OTA du firmware `.bin` avec indicateur de progression.

---

## 📁 Structure du Projet

```text
esp32_power_meter/
├── CMakeLists.txt                # Configuration racine CMake ESP-IDF
├── partitions.csv                # Table de partitionnement Flash (Dual OTA + LittleFS)
├── README.md                     # Documentation du projet
├── components/
│   └── iot_service/              # Module autonome Wi-Fi / Web / MQTT / NTP / OTA
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── iot_service.h
│       └── iot_service.c
├── main/
│   ├── idf_component.yml         # Dépendance gérée (LittleFS)
│   ├── CMakeLists.txt            # Enregistrement du composant main et image LittleFS
│   └── main.c                    # Mesures, régulation optocoupleur et points d'accès API
└── web/
    ├── index.html                # Page HTML du tableau de bord
    ├── style.css                 # Feuilles de style
    └── script.js                 # Logique JavaScript, graphiques Chart.js et client OTA
```

---

## 🛠 Compilation & Flash

### 1. Prérequis
- [ESP-IDF v5.x ou v6.x](https://espressif.com) installé et activé dans votre terminal.

### 2. Configuration et compilation
```bash
# Définir la cible matérielle
idf.py set-target esp32

# Compiler le firmware et générer l'image LittleFS (storage.bin)
idf.py build
```

### 3. Premier flashage par câble USB
La partition LittleFS est automatiquement incluse dans le flashage global :
```bash
# Effacer l'ancienne table de partitions (recommandé lors du passage à l'OTA), flasher et ouvrir le moniteur
# (Sous Windows, remplacer /dev/ttyUSB0 par le port COM correspondant, ex: COM3)
idf.py -p /dev/ttyUSB0 erase-flash flash monitor
```

---

## ⚙️ Configuration & Mises à jour (OTA)

### Configuration initiale
1. Au premier démarrage (aucun réseau Wi-Fi enregistré), l'ESP32 diffuse un point d'accès ouvert intitulé **`ESP32-PowerMeter`**.
2. Connectez-vous à ce réseau avec votre téléphone ou PC *(sur smartphone, activez le Mode Avion + Wi-Fi pour éviter que les données mobiles ne prennent le relais)*, puis ouvrez **`http://192.168.4`**.
3. Ouvrez les volets escamotables :
   - **Configuration Wi-Fi :** Renseignez le SSID et le mot de passe de votre box (les espaces et caractères spéciaux sont pris en charge).
   - **Configuration MQTT :** Entrez l'adresse de votre broker (ex: `mqtt://192.168.1.50:1883`) et le topic de base.
   - **Configuration Optocoupleur :** Ajustez vos seuils d'activation/désactivation de surplus en Watts (ex: `-50 W` et `-10 W`).
4. Validez pour enregistrer : l'ESP32 redémarre et se connecte à votre réseau domestique.
   - *Note : Si la box est injoignable après 5 tentatives, l'ESP32 réactive automatiquement le point d'accès `ESP32-PowerMeter` en parallèle et retente la connexion toutes les 60 secondes.*

### Mises à jour sans fil (OTA)
Une fois l'ESP32 connecté à votre réseau, ouvrez le volet **🛠️ Mise à jour Interface Web** en bas du tableau de bord :
- **Interface Web :** Sélectionnez un ou plusieurs fichiers modifiés (`index.html`, `style.css`, `script.js`) et cliquez sur **Envoyer les fichiers**. Actualisez la page pour voir les changements.
- **Firmware C :** Compilez votre projet avec `idf.py build`, sélectionnez le fichier `build/esp32_power_mete
