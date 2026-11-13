#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_service.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_littlefs.h"
#include "mqtt_client.h"
#include "esp_sntp.h"
#include <dirent.h>
#include "esp_ota_ops.h"

static const char *TAG = "IOT_SERVICE";
#define FILE_PATH_MAX 1024
#define MAX_WIFI_RETRY 5
#define WIFI_RETRY_INTERVAL_US (60 * 1000000ULL)

static httpd_handle_t s_server = NULL;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static esp_timer_handle_t s_wifi_retry_timer = NULL;
static bool s_mqtt_connected = false;
static bool s_mqtt_started = false;
static bool s_ap_fallback_active = false;
static int s_retry_num = 0;

static char s_ap_ssid[32] = "ESP32-PowerMeter";
static char s_wifi_ssid[32] = "";
static char s_wifi_pass[64] = "";
static char s_mqtt_uri[128] = "mqtt://192.168.1.50:1883";
static char s_mqtt_topic[64] = "maison/energie";
static char s_base_path[32] = "/spiffs";

/* --- Utilitaires --- */
static void url_decode(char *dst, const char *src, size_t max_len) {
    char a, b;
    size_t i = 0;
    while (*src && i < max_len - 1) {
        if ((*src == '%') && ((a = *(src + 1)) && (b = *(src + 2))) && (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            dst[i++] = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* --- NVS --- */
static void load_nvs_settings(void) {
    nvs_handle_t h;
    if (nvs_open("iot_cfg", NVS_READONLY, &h) == ESP_OK) {
        size_t s1 = sizeof(s_wifi_ssid), s2 = sizeof(s_wifi_pass);
        size_t s3 = sizeof(s_mqtt_uri), s4 = sizeof(s_mqtt_topic);
        nvs_get_str(h, "ssid", s_wifi_ssid, &s1);
        nvs_get_str(h, "pass", s_wifi_pass, &s2);
        nvs_get_str(h, "uri",  s_mqtt_uri,  &s3);
        nvs_get_str(h, "topic", s_mqtt_topic, &s4);
        nvs_close(h);
    }
}

static void save_str_nvs(const char *key, const char *val) {
    nvs_handle_t h;
    if (nvs_open("iot_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* --- Gestion du Mode AP de Secours et Timer --- */
static void configure_ap_interface(void) {
    wifi_config_t ap_cfg = {
        .ap = {
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4
        }
    };
    strncpy((char*)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
}

static void wifi_retry_timer_callback(void *arg) {
    ESP_LOGI(TAG, "Tentative periodique de reconnexion au Wi-Fi '%s'...", s_wifi_ssid);
    esp_wifi_connect();
}

static void enable_ap_fallback(void) {
    if (s_ap_fallback_active) return;

    ESP_LOGW(TAG, "Activation du Point d'Acces de secours '%s' (192.168.4.1)", s_ap_ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    configure_ap_interface();
    s_ap_fallback_active = true;

    if (s_wifi_retry_timer != NULL && !esp_timer_is_active(s_wifi_retry_timer)) {
        esp_timer_start_periodic(s_wifi_retry_timer, WIFI_RETRY_INTERVAL_US);
    }
}

static void disable_ap_fallback(void) {
    if (!s_ap_fallback_active) return;

    ESP_LOGI(TAG, "Wi-Fi retabli : arret du Point d'Acces de secours");
    if (s_wifi_retry_timer != NULL && esp_timer_is_active(s_wifi_retry_timer)) {
        esp_timer_stop(s_wifi_retry_timer);
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_ap_fallback_active = false;
}

/* --- Gestionnaires d'Événements Wi-Fi et IP --- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (!s_ap_fallback_active) {
            if (s_retry_num < MAX_WIFI_RETRY) {
                s_retry_num++;
                ESP_LOGW(TAG, "Wi-Fi deconnecte (raison: %d), tentative %d/%d...", disc->reason, s_retry_num, MAX_WIFI_RETRY);
                esp_wifi_connect();
            } else {
                enable_ap_fallback();
            }
        } else {
            ESP_LOGW(TAG, "Echec de la tentative periodique (raison: %d). Prochain essai dans 1 min.", disc->reason);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_retry_num = 0;
        ESP_LOGI(TAG, "Wi-Fi connecte ! Adresse IP : " IPSTR, IP2STR(&event->ip_info.ip));

        disable_ap_fallback();

        if (s_mqtt_client != NULL && !s_mqtt_started) {
            esp_mqtt_client_start(s_mqtt_client);
            s_mqtt_started = true;
        }
    }
}

/* --- MQTT Event Handler --- */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_mqtt_connected = true;
            ESP_LOGI(TAG, "MQTT connecte au broker");
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT deconnecte");
            break;
        default:
            break;
    }
}

/* --- HTTP Handlers Internes --- */
static esp_err_t set_mime(httpd_req_t *req, const char *path) {
    if (strstr(path, ".html")) return httpd_resp_set_type(req, "text/html");
    if (strstr(path, ".css"))  return httpd_resp_set_type(req, "text/css");
    if (strstr(path, ".js"))   return httpd_resp_set_type(req, "application/javascript");
    if (strstr(path, ".json")) return httpd_resp_set_type(req, "application/json");
    return httpd_resp_set_type(req, "text/plain");
}

static esp_err_t static_file_handler(httpd_req_t *req) {
    char filepath[FILE_PATH_MAX];
    if (strcmp(req->uri, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "%s/index.html", s_base_path);
    } else {
        snprintf(filepath, sizeof(filepath), "%s%s", s_base_path, req->uri);
    }

    FILE *f = fopen(filepath, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Fichier non trouve");
        return ESP_FAIL;
    }

    set_mime(req, filepath);
    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t wifi_cfg_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char raw_ssid[96] = {0}, raw_pass[192] = {0};
    char ssid[32] = {0}, pass[64] = {0};

    sscanf(buf, "ssid=%95[^&]&pass=%191s", raw_ssid, raw_pass);
    url_decode(ssid, raw_ssid, sizeof(ssid));
    url_decode(pass, raw_pass, sizeof(pass));

    save_str_nvs("ssid", ssid);
    save_str_nvs("pass", pass);

    httpd_resp_sendstr(req, "<html><body><h1>Wi-Fi enregistre. Redemarrage...</h1></body></html>");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t mqtt_cfg_handler(httpd_req_t *req) {
    char buf[384];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char raw_uri[256] = {0}, raw_topic[128] = {0};
    char uri[128] = {0}, topic[64] = {0};

    sscanf(buf, "mqtt_uri=%255[^&]&mqtt_topic=%127s", raw_uri, raw_topic);
    url_decode(uri, raw_uri, sizeof(uri));
    url_decode(topic, raw_topic, sizeof(topic));

    save_str_nvs("uri", uri);
    save_str_nvs("topic", topic);

    httpd_resp_sendstr(req, "<html><body><h1>MQTT enregistre. Redemarrage...</h1></body></html>");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}


static esp_err_t api_files_handler(httpd_req_t *req) {
    DIR *dir = opendir(s_base_path);
    if (!dir) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", HTTPD_RESP_USE_STRLEN);

    struct dirent *entry;
    struct stat st;
    char fullpath[FILE_PATH_MAX];
    char item[320];
    bool first = true;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            snprintf(fullpath, sizeof(fullpath), "%s/%s", s_base_path, entry->d_name);
            long size = (stat(fullpath, &st) == 0) ? st.st_size : 0;
            snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"size\":%ld}", first ? "" : ",", entry->d_name, size);
            httpd_resp_send_chunk(req, item, HTTPD_RESP_USE_STRLEN);
            first = false;
        }
    }
    closedir(dir);

    httpd_resp_send_chunk(req, "]", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}


static esp_err_t api_cfg_handler(httpd_req_t *req) {
    char resp[256];
    snprintf(resp, sizeof(resp), "{\"mqtt_uri\":\"%s\",\"mqtt_topic\":\"%s\"}", s_mqtt_uri, s_mqtt_topic);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t upload_handler(httpd_req_t *req) {
    char filepath[FILE_PATH_MAX];
    char raw_name[128] = {0};
    char clean_name[128] = {0};

    const char *q = strstr(req->uri, "?file=");
    if (q) {
        strncpy(raw_name, q + 6, sizeof(raw_name) - 1);
        url_decode(clean_name, raw_name, sizeof(clean_name));
    } else {
        strcpy(clean_name, "index.html");
    }

    snprintf(filepath, sizeof(filepath), "%s/%s", s_base_path, clean_name);

    FILE *f = fopen(filepath, "w");
    if (!f) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char buf[512];
    int rem = req->content_len;
    while (rem > 0) {
        int r = httpd_req_recv(req, buf, MIN(rem, sizeof(buf)));
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            fclose(f);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        fwrite(buf, 1, r, f);
        rem -= r;
    }
    fclose(f);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t ota_upload_handler(httpd_req_t *req) {
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Partition OTA introuvable");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Partition OTA introuvable");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Debut OTA vers la partition '%s' (offset 0x%lx, taille %d octets)",
             update_partition->label, update_partition->address, req->content_len);

    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin a echoue (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur initialisation OTA");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = req->content_len;

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Erreur reception reseau pendant OTA");
            esp_ota_abort(update_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur transfert");
            return ESP_FAIL;
        }

        err = esp_ota_write(update_handle, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write a echoue (%s)", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur ecriture flash");
            return ESP_FAIL;
        }
        remaining -= recv_len;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end a echoue (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Firmware invalide ou corrompu");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition a echoue (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur changement partition boot");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA reussie ! Redemarrage dans 1.5s...");
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}



/* --- Initialisation Principale --- */
esp_err_t iot_service_init(const iot_service_config_t *config) {
    strncpy(s_base_path, config->littlefs_base_path, sizeof(s_base_path) - 1);
    strncpy(s_ap_ssid, config->ap_ssid, sizeof(s_ap_ssid) - 1);

    // 1. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
	nvs_flash_init();
    }
    load_nvs_settings();
    // 2. LittleFS
    esp_vfs_littlefs_conf_t lfs_cfg = {
      .base_path = s_base_path,
      .partition_label = config->littlefs_partition,
      .format_if_mount_failed = true,
      .dont_mount = false,
    };
    esp_err_t lfs_ret = esp_vfs_littlefs_register(&lfs_cfg);
    if (lfs_ret != ESP_OK) {
      ESP_LOGE(TAG, "Erreur montage LittleFS (%s)", esp_err_to_name(lfs_ret));
    } else {
      size_t total = 0, used = 0;
      esp_littlefs_info(lfs_cfg.partition_label, &total, &used);
      ESP_LOGI(TAG, "LittleFS monte : %d / %d octets utilises", (int)used, (int)total);
    }
    // 3. Initialisation Réseau et Création des deux interfaces (STA + AP)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t w_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&w_cfg));
    const esp_timer_create_args_t timer_args = {
      .callback = &wifi_retry_timer_callback,
      .name = "wifi_retry_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_wifi_retry_timer));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
							ESP_EVENT_ANY_ID,
							&wifi_event_handler,
							NULL,
							NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
							IP_EVENT_STA_GOT_IP,
							&wifi_event_handler,
							NULL,
							NULL));
    // 4. Préparation MQTT
    if (strlen(s_wifi_ssid) > 0) {
      esp_mqtt_client_config_t m_cfg = {
	.broker.address.uri = s_mqtt_uri,
      };
      s_mqtt_client = esp_mqtt_client_init(&m_cfg);
      esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    }
    // 5. Démarrage Wi-Fi (STA si SSID configuré, sinon AP pur)
    if (strlen(s_wifi_ssid) > 0) {
      wifi_config_t sta_cfg = {0};
      strncpy((char*)sta_cfg.sta.ssid, s_wifi_ssid, sizeof(sta_cfg.sta.ssid));
      strncpy((char*)sta_cfg.sta.password, s_wifi_pass, sizeof(sta_cfg.sta.password));
      ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
      ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
      ESP_ERROR_CHECK(esp_wifi_start());
      esp_wifi_connect();
    } else {
      ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
      configure_ap_interface();
      ESP_ERROR_CHECK(esp_wifi_start());
    }
    // 6. SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, config->sntp_server);
    esp_sntp_init();
    setenv("TZ", config->timezone, 1);
    tzset();
    // 7. Serveur HTTP
    httpd_config_t h_cfg = HTTPD_DEFAULT_CONFIG();
    h_cfg.stack_size = 8192;
    h_cfg.max_uri_handlers = 14;
    h_cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_server, &h_cfg) == ESP_OK) {
      httpd_uri_t u_wifi = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = wifi_cfg_handler};
      httpd_uri_t u_mqtt = {
        .uri = "/mqtt",
        .method = HTTP_POST,
        .handler = mqtt_cfg_handler};
      httpd_uri_t u_cfg = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = api_cfg_handler};
      httpd_uri_t u_up = {
        .uri = "/upload*",
        .method = HTTP_POST,
        .handler = upload_handler };
      httpd_uri_t u_files = {.uri = "/api/files",
                             .method = HTTP_GET,
                             .handler = api_files_handler};
      httpd_uri_t u_ota = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_upload_handler};

      httpd_register_uri_handler(s_server, &u_wifi);
      httpd_register_uri_handler(s_server, &u_mqtt);
      httpd_register_uri_handler(s_server, &u_cfg);
      httpd_register_uri_handler(s_server, &u_up);
      httpd_register_uri_handler(s_server, &u_files);
      httpd_register_uri_handler(s_server, &u_ota);
    }
    return ESP_OK;
}
esp_err_t iot_service_register_http_uri(const httpd_uri_t *uri_handler) {
  if (!s_server) return ESP_FAIL;
  return httpd_register_uri_handler(s_server, uri_handler);
}
esp_err_t iot_service_register_fallback_handler(void) {
  if (!s_server) return ESP_FAIL;
  httpd_uri_t u_file = { .uri = "/*", .method = HTTP_GET, .handler = static_file_handler };
  return httpd_register_uri_handler(s_server, &u_file);
}
esp_err_t iot_service_mqtt_publish(const char *subtopic, const char *payload) {
  if (!s_mqtt_client || !s_mqtt_connected) return ESP_FAIL;
  char full_topic[160];
  snprintf(full_topic, sizeof(full_topic), "%s/%s", s_mqtt_topic, subtopic);
  return esp_mqtt_client_publish(s_mqtt_client, full_topic, payload, 0, 0, 0) >= 0 ? ESP_OK : ESP_FAIL;
}
const char* iot_service_get_mqtt_base_topic(void) {
  return s_mqtt_topic;
}
bool iot_service_mqtt_is_connected(void) {
  return s_mqtt_connected;
}
