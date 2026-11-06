#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_service.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_littlefs.h"
#include "mqtt_client.h"
#include "esp_sntp.h"

static const char *TAG = "IOT_SERVICE";
#define FILE_PATH_MAX 128

static httpd_handle_t s_server = NULL;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;

static char s_wifi_ssid[32] = "";
static char s_wifi_pass[64] = "";
static char s_mqtt_uri[128] = "mqtt://192.168.1.50:1883";
static char s_mqtt_topic[64] = "maison/energie";
static char s_base_path[32] = "/spiffs";

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
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char ssid[32] = {0}, pass[64] = {0};
    sscanf(buf, "ssid=%31[^&]&pass=%63s", ssid, pass);
    save_str_nvs("ssid", ssid);
    save_str_nvs("pass", pass);

    httpd_resp_sendstr(req, "<html><body><h1>Wi-Fi enregistre. Redemarrage...</h1></body></html>");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t mqtt_cfg_handler(httpd_req_t *req) {
    char buf[192];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char uri[128] = {0}, topic[64] = {0};
    sscanf(buf, "mqtt_uri=%127[^&]&mqtt_topic=%63s", uri, topic);
    save_str_nvs("uri", uri);
    save_str_nvs("topic", topic);

    httpd_resp_sendstr(req, "<html><body><h1>MQTT enregistre. Redemarrage...</h1></body></html>");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
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
    const char *q = strstr(req->uri, "?file=");
    snprintf(filepath, sizeof(filepath), "%s/%s", s_base_path, q ? (q + 6) : "index.html");

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
    httpd_resp_sendstr(req, "<html><body><h1>Upload termine</h1><a href='/'>Accueil</a></body></html>");
    return ESP_OK;
}

/* --- Initialisation Principale --- */
esp_err_t iot_service_init(const iot_service_config_t *config) {
    strncpy(s_base_path, config->littlefs_base_path, sizeof(s_base_path) - 1);

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
    esp_vfs_littlefs_register(&lfs_cfg);

    // 3. Wi-Fi
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t w_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&w_cfg));

    if (strlen(s_wifi_ssid) > 0) {
        esp_netif_create_default_wifi_sta();
        wifi_config_t sta_cfg = {0};
        strncpy((char*)sta_cfg.sta.ssid, s_wifi_ssid, sizeof(sta_cfg.sta.ssid));
        strncpy((char*)sta_cfg.sta.password, s_wifi_pass, sizeof(sta_cfg.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_wifi_connect();
    } else {
        esp_netif_create_default_wifi_ap();
        wifi_config_t ap_cfg = {
            .ap = {
                .channel = 1,
                .authmode = WIFI_AUTH_OPEN,
                .max_connection = 4
            }
        };
        strncpy((char*)ap_cfg.ap.ssid, config->ap_ssid, sizeof(ap_cfg.ap.ssid));
        ap_cfg.ap.ssid_len = strlen(config->ap_ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    // 4. SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, config->sntp_server);
    esp_sntp_init();
    setenv("TZ", config->timezone, 1);
    tzset();

    // 5. MQTT
    esp_mqtt_client_config_t m_cfg = {
        .broker.address.uri = s_mqtt_uri,
    };
    s_mqtt_client = esp_mqtt_client_init(&m_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);

    // 6. Serveur HTTP
    httpd_config_t h_cfg = HTTPD_DEFAULT_CONFIG();
    h_cfg.max_uri_handlers = 14;
    h_cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_server, &h_cfg) == ESP_OK) {
        httpd_uri_t u_wifi = { .uri = "/wifi", .method = HTTP_POST, .handler = wifi_cfg_handler };
        httpd_uri_t u_mqtt = { .uri = "/mqtt", .method = HTTP_POST, .handler = mqtt_cfg_handler };
        httpd_uri_t u_cfg  = { .uri = "/api/config", .method = HTTP_GET, .handler = api_cfg_handler };
        httpd_uri_t u_up   = { .uri = "/upload*", .method = HTTP_POST, .handler = upload_handler };
        httpd_uri_t u_file = { .uri = "/*", .method = HTTP_GET, .handler = static_file_handler };

        httpd_register_uri_handler(s_server, &u_wifi);
        httpd_register_uri_handler(s_server, &u_mqtt);
        httpd_register_uri_handler(s_server, &u_cfg);
        httpd_register_uri_handler(s_server, &u_up);
        httpd_register_uri_handler(s_server, &u_file);
    }
    return ESP_OK;
}

esp_err_t iot_service_register_http_uri(const httpd_uri_t *uri_handler) {
    if (!s_server) return ESP_FAIL;
    return httpd_register_uri_handler(s_server, uri_handler);
}

esp_err_t iot_service_mqtt_publish(const char *subtopic, const char *payload) {
    if (!s_mqtt_client) return ESP_FAIL;
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
