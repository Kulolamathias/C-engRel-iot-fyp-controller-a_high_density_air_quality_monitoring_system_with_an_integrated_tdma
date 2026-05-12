/**
 * @file components/network/src/network.c
 * @brief Implementation of the common network module.
 *
 * @author Matthithyahu
 * @date 2026/05/12
 */

#include "network.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Hard‑coded credentials
 * ------------------------------------------------------------------------- */
#define WIFI_SSID       "Mathias' Sxx U..."
#define WIFI_PASSWORD   "1234567890223"
#define MQTT_BROKER_URI "mqtt://102.223.8.140:1883"
#define MQTT_USERNAME   "mqtt_user"
#define MQTT_PASSWORD   "ega12345"

static const char *TAG = "network";

/* -------------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------------- */
static esp_mqtt_client_handle_t mqtt_client = NULL;
static network_mqtt_data_cb_t    data_cb    = NULL;
static char                      node_id[16];

/* -------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------- */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data);
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data);
static void ip_got_handler(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data);
static void wait_for_ip(void);

/* Semaphore for IP wait – static so the callback can access it */
static SemaphoreHandle_t ip_sem = NULL;

/* ===================================================================== */
esp_err_t network_start(void)
{
    esp_err_t ret;

    /* ---- 1. NVS flash ---- */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ---- 2. TCP/IP stack and event loop ---- */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* ---- 3. Wi‑Fi station ---- */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_got_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi‑Fi SSID \"%s\" ...", WIFI_SSID);
    wait_for_ip();   /* blocks until STA_GOT_IP */
    ESP_LOGI(TAG, "Wi‑Fi connected, got IP");

    /* ---- 4. MQTT client ---- */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!mqtt_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
    ESP_LOGI(TAG, "MQTT client started");

    /* ---- 5. SNTP sync ---- */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    /* Wait for time to be set (with timeout) */
    int retry = 0;
    time_t now;
    while (retry < 20) {
        time(&now);
        if (now > 1000000000) break;   /* reasonable timestamp */
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
    }
    if (now <= 1000000000) {
        ESP_LOGW(TAG, "SNTP sync failed, but continuing without UTC time");
    } else {
        ESP_LOGI(TAG, "SNTP sync done, current time: %lld", (long long)now);
    }

    return ESP_OK;
}

/* ===================================================================== */
void network_get_node_id(char *buf, size_t buflen, const char *prefix)
{
    if (!buf || buflen < 4) return;

    if (node_id[0] == '\0') {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(node_id, sizeof(node_id), "%s_%02X%02X",
                 prefix, mac[4], mac[5]);
    }
    strlcpy(buf, node_id, buflen);
}

/* ===================================================================== */
esp_err_t network_mqtt_publish(const char *topic,
                               const char *data,
                               size_t data_len,
                               int qos,
                               bool retain)
{
    if (!mqtt_client) return ESP_ERR_INVALID_STATE;
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, data,
                                         data_len, qos, retain);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

/* ===================================================================== */
esp_err_t network_mqtt_subscribe(const char *topic, int qos)
{
    if (!mqtt_client) return ESP_ERR_INVALID_STATE;
    int msg_id = esp_mqtt_client_subscribe(mqtt_client, topic, qos);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

/* ===================================================================== */
void network_mqtt_set_data_callback(network_mqtt_data_cb_t cb)
{
    data_cb = cb;
}

/* =====================================================================
 * Internal helpers
 * ===================================================================== */

/** Wi‑Fi event handler – only used for logging. */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi‑Fi station started, connecting...");
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi‑Fi disconnected, reconnecting...");
        esp_wifi_connect();
    }
}

/** IP‑got handler – gives the semaphore to unblock wait_for_ip(). */
static void ip_got_handler(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    if (ip_sem) {
        xSemaphoreGive(ip_sem);
    }
}

/** Block until the Wi‑Fi station obtains an IP address. */
static void wait_for_ip(void)
{
    ip_sem = xSemaphoreCreateBinary();
    if (!ip_sem) {
        ESP_LOGE(TAG, "Could not create IP semaphore");
        return;
    }
    xSemaphoreTake(ip_sem, pdMS_TO_TICKS(15000));
    vSemaphoreDelete(ip_sem);
    ip_sem = NULL;
}

/**
 * @brief MQTT client event handler.
 *
 * Processes connection/disconnection and incoming data.
 */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_DATA: {
        esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
        if (data_cb) {
            data_cb(event->topic, event->data, event->data_len);
        }
        break;
    }
    default:
        break;
    }
}