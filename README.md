# ⚡ ESP32 Dual Power Meter & Solar Diverter

Système embarqué haute performance de mesure de puissance active bidirectionnelle (230 V AC) pour deux circuits indépendants, conçu pour **ESP32-WROOM-32** sous **ESP-IDF v5.x**.

Le projet intègre une interface web réactive servie directement par l'ESP32 via **LittleFS**, un routage automatique de surplus solaire par **optocoupleur**, une publication des données via **MQTT**, ainsi qu'une historisation multi-échelles synchronisée par **NTP**.

---

## 📋 Sommaire

- [Fonctionnalités principales](#-fonctionnalités-principales)
- [Architecture Matérielle & Câblage](#-architecture-matérielle--câblage)
- [Architecture Logicielle & Multitâche](#-architecture-logicielle--multitâche)
- [Interface Web Embarquée](#-interface-web-embarquée)
- [Structure du Projet](#-structure-du-projet)
- [Compilation & Flash](#-compilation--flash)
- [Configuration Initiale](#-configuration-initiale)

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
   - **Wi-Fi Manager :** Connexion en mode Station (STA) avec bascule automatique en Point d'Accès de secours (`ESP32-PowerMeter` sur `192.168.4.1`) si aucun réseau n'est configuré.
   - **Synchronisation SNTP :** Horodatage précis des mesures avec gestion automatique des fuseaux horaires et passages heure d'été/hiver.
   - **Client MQTT :** Publication périodique des puissances et alertes d'état de l'optocoupleur (compatible Home Assistant, Node-RED, etc.).

5. **Interface Web & Historique :**
   - Fichiers statiques (`index.html`, `style.css`, `script.js`) stockés et servis depuis la mémoire flash via **LittleFS**.
   - Cartes pliables/escamotables pour les réglages (Wi-Fi, MQTT, Seuils optocoupleur).
   - Graphique interactif (**Chart.js**) affichant l'historique sur 3 périodes : **12 heures**, **7 jours** et **12 mois**.
   - Possibilité de téléverser et mettre à jour les fichiers web sans reflasher le firmware via `/upload.html`.

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

```
                            ESP32 DUAL CORE
       ┌───────────────────────────────┐   ┌───────────────────────────────┐
       │            CORE 0             │   │            CORE 1             │
       │       (Réseau / Système)      │   │          (Temps Réel)         │
       ├───────────────────────────────┤   ├───────────────────────────────┤
       │ • Pile Wi-Fi L2/L3 (STA/AP)   │   │ • Acquisition ADC ADS1115     │
       │ • Pile lwIP TCP/IP            │   │ • Intégration P = V x I       │
       │ • Serveur HTTPD (LittleFS)    │   │ • Calculs RMS (Tension/Irms)  │
       │ • Client MQTT                 │   │ • Hystérésis Optocoupleur     │
       │ • Synchronisation SNTP        │   │ • Agrégation Historique       │
       └───────────────┬───────────────┘   └───────────────┬───────────────┘
                       │                                   │
                       └─────────────► [ MUTEX ] ◄─────────┘
                                 Protection des lectures
                                  et écritures partagées
```

---

## 🌐 Interface Web Embarquée

L'interface web hébergée sur l'ESP32 offre une expérience utilisateur moderne :
- **Carte des mesures :** Affichage en grand format de la puissance ($W$), des courants ($A$), de la tension secteur ($V$) et de l'état du relais.
- **Coloration dynamique :** Affichage bleu lors de la consommation réseau, et passage immédiat en vert avec badge *« Injection Solaire »* si la puissance devient négative.
- **Accordéons escamotables :** Les cartes d'historique et de réglages se replient pour conserver une vue dégagée sur mobile.
- **Graphique dynamique :** Visualisation des 12 dernières heures, 7 derniers jours ou 12 derniers mois avec mise à l'échelle temporelle.

---

## 📁 Structure du Projet

```text
esp32_power_meter/
├── CMakeLists.txt                # Configuration racine CMake ESP-IDF
├── partitions.csv                # Table de partitionnement Flash (App + LittleFS)
├── README.md                     # Documentation du projet
├── components/
│   └── iot_service/              # Module autonome Wi-Fi / Web / MQTT / NTP
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── iot_service.h
│       └── iot_service.c
├── main/
|   ├── idf_component.yml         # Dépendance gérée (LittleFS)
|   ├── CMakeLists.txt            # Enregistrement du composant main
|   └── main.c                    # Mesures, régulation optocoupleur et points d'accès API
└── web/
    ├── index.html                # Page HTML du tableau de bord
    ├── style.css                 # Feuilles de style
    └── script.js                 # Logique JavaScript et graphiques Chart.js
```

---

## 🛠 Compilation & Flash

### 1. Prérequis
- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) installé et activé dans votre terminal.

### 2. Configuration et compilation
```bash
# Définir la cible matérielle
idf.py set-target esp32

# Compiler le firmware
idf.py build
```

### 3. Programmation de l'ESP32
```bash
# Flasher le microcontrôleur et ouvrir la console série
# (Sous Windows, remplacer /dev/ttyUSB0 par le port COM correspondant, ex: COM3)
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## ⚙️ Configuration Initiale

1. Au premier démarrage, l'ESP32 diffuse un point d'accès Wi-Fi ouvert intitulé **`ESP32-PowerMeter`**.
2. Connectez-vous à ce réseau avec votre téléphone ou PC, puis accédez à **`http://192.168.4.1/upload.html`**.
3. Téléversez les fichiers de l'interface un à un :
   - `/upload?file=index.html` $\rightarrow$ sélectionnez `index.html`
   - `/upload?file=style.css` $\rightarrow$ sélectionnez `style.css`
   - `/upload?file=script.js` $\rightarrow$ sélectionnez `script.js`
4. Retournez sur la page d'accueil (`http://192.168.4.1/`), ouvrez les volets escamotables :
   - **Configuration Wi-Fi :** Renseignez le SSID et mot de passe de votre box.
   - **Configuration MQTT :** Entrez l'adresse de votre broker (ex: `mqtt://192.168.1.50:1883`).
   - **Configuration Optocoupleur :** Ajustez vos seuils d'activation/désactivation de surplus en Watts (ex: `-50 W` et `-10 W`).
5. Validez pour enregistrer : l'ESP32 redémarre et se connecte à votre réseau domestique.
