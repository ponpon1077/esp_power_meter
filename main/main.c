#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "iot_service.h"

static const char *TAG = "MAIN_APP";

/* --- Configuration Matérielle --- */
#define I2C_PORT            I2C_NUM_0
#define SDA_PIN             GPIO_NUM_21
#define SCL_PIN             GPIO_NUM_22
#define ADS1115_ADDR        0x48
#define ADS1115_CONF_REG    0x01
#define ADS1115_CONV_REG    0x00

#define OPTO_GPIO           GPIO_NUM_18

/* Facteurs de calibration */
#define V_CALIB             230.0f
#define I1_CALIB            100.0f  // Adapté aux XT-SCT-T24 (ex: 100A/1V)
#define I2_CALIB            100.0f

/* --- Tailles des Tampons d'Historique --- */
#define HIST_12H_SIZE       144     // 12h @ 1 pt / 5 min
#define HIST_7D_SIZE        168     // 7 jours @ 1 pt / 1 heure
#define HIST_12M_SIZE       365     // 12 mois @ 1 pt / jour

typedef struct {
    float p1[HIST_12H_SIZE];
    float p2[HIST_12H_SIZE];
    time_t ts[HIST_12H_SIZE];
    int count;
} history_buffer_t;

typedef struct {
    float power_active;
    float v_rms;
    float i_rms;
} circuit_measure_t;

/* --- Variables d'État et de Partage --- */
static i2c_master_dev_handle_t ads_dev_handle = NULL;
static SemaphoreHandle_t data_mutex = NULL;

static circuit_measure_t circuit1 = {0};
static circuit_measure_t circuit2 = {0};

static history_buffer_t hist_12h = {0};
static history_buffer_t hist_7d  = {0};
static history_buffer_t hist_12m = {0};

static float sum_p1_5m = 0, sum_p2_5m = 0;
static int samples_5m = 0;

/* Seuils d'activation de l'optocoupleur (Watts, négatif = injection) */
static float opto_th_on  = -50.0f;
static float opto_th_off = -10.0f;
static bool opto_active  = false;

/* --- Gestion de l'ADS1115 (I2C) --- */
static float read_ads1115_volts(uint8_t channel) {
    // Single-shot, FSR ±4.096V, 860 SPS
    uint16_t config = 0x8000 | ((0x04 + channel) << 12) | (0x01 << 9) | (0x01 << 8) | (0x07 << 5) | 0x0003;
    uint8_t write_buf[3] = { ADS1115_CONF_REG, (uint8_t)(config >> 8), (uint8_t)(config & 0xFF) };
    
    i2c_master_transmit(ads_dev_handle, write_buf, 3, -1);
    vTaskDelay(pdMS_TO_TICKS(2));

    uint8_t reg = ADS1115_CONV_REG;
    uint8_t read_buf[2];
    i2c_master_transmit_receive(ads_dev_handle, &reg, 1, read_buf, 2, -1);

    int16_t raw = (int16_t)((read_buf[0] << 8) | read_buf[1]);
    return raw * 0.000125f; // LSB = 125uV pour FSR ±4.096V
}

static void i2c_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &ads_dev_handle));
}

/* --- Gestion de l'Optocoupleur et NVS --- */
static void init_optocoupler_gpio(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << OPTO_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(OPTO_GPIO, 0);
}

static void save_opto_thresholds(float th_on, float th_off) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        char buf_on[16], buf_off[16];
        snprintf(buf_on, sizeof(buf_on), "%.1f", th_on);
        snprintf(buf_off, sizeof(buf_off), "%.1f", th_off);
        nvs_set_str(h, "opto_on", buf_on);
        nvs_set_str(h, "opto_off", buf_off);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_opto_thresholds(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
        char buf_on[16], buf_off[16];
        size_t len_on = sizeof(buf_on), len_off = sizeof(buf_off);
        if (nvs_get_str(h, "opto_on", buf_on, &len_on) == ESP_OK) {
            opto_th_on = atof(buf_on);
        }
        if (nvs_get_str(h, "opto_off", buf_off, &len_off) == ESP_OK) {
            opto_th_off = atof(buf_off);
        }
        nvs_close(h);
    }
}

static void update_optocoupler_state(float power_value) {
    bool previous = opto_active;

    if (power_value < opto_th_on) {
        opto_active = true;
    } else if (power_value > opto_th_off) {
        opto_active = false;
    }

    gpio_set_level(OPTO_GPIO, opto_active ? 1 : 0);

    if (opto_active != previous) {
        ESP_LOGI(TAG, "Optocoupleur bascule : %s (Puissance : %.1f W)",
                 opto_active ? "ACTIF" : "INACTIF", power_value);
        iot_service_mqtt_publish("optocoupleur/etat", opto_active ? "ON" : "OFF");
    }
}

/* --- Gestion de l'Historique --- */
static void push_history(history_buffer_t *hist, float val1, float val2, int max_size) {
    time_t now;
    time(&now);
    if (hist->count < max_size) {
        hist->p1[hist->count] = val1;
        hist->p2[hist->count] = val2;
        hist->ts[hist->count] = now;
        hist->count++;
    } else {
        memmove(&hist->p1[0], &hist->p1[1], sizeof(float) * (max_size - 1));
        memmove(&hist->p2[0], &hist->p2[1], sizeof(float) * (max_size - 1));
        memmove(&hist->ts[0], &hist->ts[1], sizeof(time_t) * (max_size - 1));
        hist->p1[max_size - 1] = val1;
        hist->p2[max_size - 1] = val2;
        hist->ts[max_size - 1] = now;
    }
}

/* --- Handlers HTTP API --- */
static esp_err_t api_data_handler(httpd_req_t *req) {
    char json[192];
    if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(json, sizeof(json),
                 "{\"p1\":%.2f,\"i1\":%.2f,\"p2\":%.2f,\"i2\":%.2f,\"v\":%.1f,\"opto\":%s}",
                 circuit1.power_active, circuit1.i_rms,
                 circuit2.power_active, circuit2.i_rms,
                 circuit1.v_rms, opto_active ? "true" : "false");
        xSemaphoreGive(data_mutex);
    } else {
        snprintf(json, sizeof(json), "{\"error\":\"busy\"}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_history_handler(httpd_req_t *req) {
    char url_buf[64];
    char period[8] = "12h";

    if (httpd_req_get_url_query_str(req, url_buf, sizeof(url_buf)) == ESP_OK) {
        httpd_query_key_value(url_buf, "period", period, sizeof(period));
    }

    history_buffer_t *target = &hist_12h;
    if (strcmp(period, "7d") == 0) target = &hist_7d;
    else if (strcmp(period, "12m") == 0) target = &hist_12m;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{\"p1\":[", HTTPD_RESP_USE_STRLEN);

    char buf[32];
    if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < target->count; i++) {
            snprintf(buf, sizeof(buf), "%.1f%s", target->p1[i], (i == target->count - 1) ? "" : ",");
            httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        }

        httpd_resp_send_chunk(req, "],\"p2\":[", HTTPD_RESP_USE_STRLEN);

        for (int i = 0; i < target->count; i++) {
            snprintf(buf, sizeof(buf), "%.1f%s", target->p2[i], (i == target->count - 1) ? "" : ",");
            httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        }

        httpd_resp_send_chunk(req, "],\"ts\":[", HTTPD_RESP_USE_STRLEN);

        for (int i = 0; i < target->count; i++) {
            snprintf(buf, sizeof(buf), "%ld%s", (long)target->ts[i], (i == target->count - 1) ? "" : ",");
            httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        }
        xSemaphoreGive(data_mutex);
    }

    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t opto_post_handler(httpd_req_t *req) {
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    float new_on = 0.0f, new_off = 0.0f;
    if (sscanf(buf, "th_on=%f&th_off=%f", &new_on, &new_off) == 2) {
        if (new_on < new_off) {
            if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                opto_th_on = new_on;
                opto_th_off = new_off;
                xSemaphoreGive(data_mutex);
            }
            save_opto_thresholds(new_on, new_off);
            httpd_resp_sendstr(req, "<html><body><h1>Seuils mis a jour !</h1><a href='/'>Retour</a></body></html>");
            return ESP_OK;
        }
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Valeurs invalides (Seuil ON doit etre < Seuil OFF)");
    return ESP_FAIL;
}

/* --- Tâche d'Acquisition Épinglée sur Cœur 1 --- */
static void power_measurement_task(void *pvParameters) {
    char payload[16];
    const int samples = 300;

    while (1) {
        double sum_p1 = 0, sum_p2 = 0;
        double sum_v2 = 0, sum_i1_2 = 0, sum_i2_2 = 0;

        for (int i = 0; i < samples; i++) {
            float v_raw  = read_ads1115_volts(0);
            float i1_raw = read_ads1115_volts(1);
            float i2_raw = read_ads1115_volts(2);

            float v_inst  = (v_raw - 1.65f)  * V_CALIB;
            float i1_inst = (i1_raw - 1.65f) * I1_CALIB;
            float i2_inst = (i2_raw - 1.65f) * I2_CALIB;

            sum_p1 += (v_inst * i1_inst);
            sum_p2 += (v_inst * i2_inst);
            sum_v2   += (v_inst * v_inst);
            sum_i1_2 += (i1_inst * i1_inst);
            sum_i2_2 += (i2_inst * i2_inst);
        }

        float p1_calc = sum_p1 / samples;
        float v_calc  = sqrtf(sum_v2 / samples);
        float i1_calc = sqrtf(sum_i1_2 / samples);

        float p2_calc = sum_p2 / samples;
        float i2_calc = sqrtf(sum_i2_2 / samples);

        // Mise à jour matérielle de l'optocoupleur sur le Circuit 1 (injection)
        update_optocoupler_state(p1_calc);

        if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            circuit1.power_active = p1_calc;
            circuit1.v_rms        = v_calc;
            circuit1.i_rms        = i1_calc;

            circuit2.power_active = p2_calc;
            circuit2.v_rms        = v_calc;
            circuit2.i_rms        = i2_calc;

            sum_p1_5m += p1_calc;
            sum_p2_5m += p2_calc;
            samples_5m++;

            if (samples_5m >= 300) {
                push_history(&hist_12h, sum_p1_5m / samples_5m, sum_p2_5m / samples_5m, HIST_12H_SIZE);
                sum_p1_5m = 0; sum_p2_5m = 0; samples_5m = 0;
            }

            xSemaphoreGive(data_mutex);
        }

        // Publication MQTT via iot_service
        snprintf(payload, sizeof(payload), "%.2f", p1_calc);
        iot_service_mqtt_publish("circuit1/puissance", payload);

        snprintf(payload, sizeof(payload), "%.2f", p2_calc);
        iot_service_mqtt_publish("circuit2/puissance", payload);

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* --- Point d'Entrée Principal --- */
void app_main(void) {
    data_mutex = xSemaphoreCreateMutex();

    i2c_init();
    init_optocoupler_gpio();
    load_opto_thresholds();

    // Démarrage du socle autonome (NVS, LittleFS, Wi-Fi, SNTP, MQTT, Web Server)
    iot_service_config_t iot_cfg = {
        .ap_ssid = "ESP32-PowerMeter",
        .littlefs_base_path = "/spiffs",
        .littlefs_partition = "storage",
        .sntp_server = "fr.pool.ntp.org",
        .timezone = "CET-1CEST,M3.5.0,M10.5.0/3"
    };
    iot_service_init(&iot_cfg);

    // Enregistrement des routes propres à l'application
    httpd_uri_t uri_api_data = { .uri = "/api/data",    .method = HTTP_GET,  .handler = api_data_handler };
    httpd_uri_t uri_api_hist = { .uri = "/api/history", .method = HTTP_GET,  .handler = api_history_handler };
    httpd_uri_t uri_opto_cfg = { .uri = "/opto",        .method = HTTP_POST, .handler = opto_post_handler };

    iot_service_register_http_uri(&uri_api_data);
    iot_service_register_http_uri(&uri_api_hist);
    iot_service_register_http_uri(&uri_opto_cfg);

    // Tâche d'échantillonnage temps réel épinglée sur Cœur 1
    xTaskCreatePinnedToCore(
        power_measurement_task,
        "power_sampling_core1",
        4096,
        NULL,
        5,
        NULL,
        1
    );
}
