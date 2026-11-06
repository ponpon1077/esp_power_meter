#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration initiale pour le service IoT unifié.
 */
typedef struct {
    const char *ap_ssid;            /*!< SSID du point d'accès de secours (ex: "ESP32-PowerMeter") */
    const char *littlefs_base_path; /*!< Point de montage LittleFS dans le VFS (ex: "/spiffs") */
    const char *littlefs_partition; /*!< Nom du label de la partition LittleFS (ex: "storage") */
    const char *sntp_server;        /*!< Serveur NTP (ex: "fr.pool.ntp.org") */
    const char *timezone;           /*!< Chaîne de configuration POSIX TZ (ex: "CET-1CEST,M3.5.0,M10.5.0/3") */
} iot_service_config_t;

/**
 * @brief Initialise l'ensemble des sous-systèmes réseau et utilitaires :
 *        NVS, LittleFS, Wi-Fi (station ou point d'accès), SNTP, MQTT et le serveur HTTP de base.
 * 
 * @param config Pointeur vers la structure de configuration du service.
 * @return ESP_OK en cas de succès, code d'erreur esp_err_t sinon.
 */
esp_err_t iot_service_init(const iot_service_config_t *config);

/**
 * @brief Enregistre une route HTTP spécifique à l'application métier sur le serveur HTTP existant.
 * 
 * @param uri_handler Structure httpd_uri_t décrivant l'URI, la méthode (GET, POST, etc.) et le handler.
 * @return ESP_OK en cas de succès, ESP_FAIL si le serveur n'est pas démarré.
 */
esp_err_t iot_service_register_http_uri(const httpd_uri_t *uri_handler);

/**
 * @brief Publie un message MQTT sous le topic racine configuré.
 *        Le topic complet résultant est : "<mqtt_topic>/<subtopic>".
 * 
 * @param subtopic Sous-chemin du topic (ex: "circuit1/puissance" ou "optocoupleur/etat").
 * @param payload Chaîne de caractères contenant les données à publier.
 * @return ESP_OK si la publication a été mise en file d'attente avec succès, ESP_FAIL sinon.
 */
esp_err_t iot_service_mqtt_publish(const char *subtopic, const char *payload);

/**
 * @brief Indique si le client MQTT est actuellement connecté au broker.
 * 
 * @return true si connecté, false sinon.
 */
bool iot_service_mqtt_is_connected(void);

/**
 * @brief Récupère le topic racine MQTT actuellement enregistré dans la configuration.
 * 
 * @return Chaîne constante vers le topic de base (ex: "maison/energie").
 */
const char* iot_service_get_mqtt_base_topic(void);

#ifdef __cplusplus
}
#endif
