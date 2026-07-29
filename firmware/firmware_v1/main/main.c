#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"

#define FIRMWARE_VERSION "1.0.0"
//#define FIRMWARE_VERSION "2.0.0"
#define DEVICE_ID        "esp32-01"

#define WIFI_SSID        "Wokwi-GUEST"
#define WIFI_PASSWORD    ""

#define OTA_CHECK_URL \
    "https://glf13cehhe.execute-api.us-east-1.amazonaws.com/check-update" \
    "?device_id=" DEVICE_ID \
    "&current_version=" FIRMWARE_VERSION

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1
#define MAXIMUM_RETRY      5

#define HTTP_RESPONSE_SIZE 4096

static const char *TAG = "FIRMWARE_V1";
//static const char *TAG = "FIRMWARE_V2";

static EventGroupHandle_t wifi_event_group;
static int wifi_retry_count = 0;

static char http_response[HTTP_RESPONSE_SIZE];
static int http_response_length = 0;

/* =========================================================
 * Wi-Fi event handler
 * ========================================================= */
static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        esp_wifi_connect();
    }

    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {

        xEventGroupClearBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );

        if (wifi_retry_count < MAXIMUM_RETRY) {
            wifi_retry_count++;

            ESP_LOGW(
                TAG,
                "Retrying Wi-Fi connection: %d/%d",
                wifi_retry_count,
                MAXIMUM_RETRY
            );

            esp_wifi_connect();
        } else {
            xEventGroupSetBits(
                wifi_event_group,
                WIFI_FAILED_BIT
            );
        }
    }

    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(
            TAG,
            "IP address: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );

        wifi_retry_count = 0;

        xEventGroupClearBits(
            wifi_event_group,
            WIFI_FAILED_BIT
        );

        xEventGroupSetBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );
    }
}

/* =========================================================
 * Connect ESP32 to Wokwi Wi-Fi
 * ========================================================= */
static esp_err_t wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_config =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&wifi_init_config)
    );

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL,
            NULL
        )
    );

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_OPEN
        }
    };

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to %s...", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected successfully");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Wi-Fi connection failed");
    return ESP_FAIL;
}

/* =========================================================
 * Store API response
 * ========================================================= */
static esp_err_t http_event_handler(
    esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA &&
        event->data != NULL &&
        event->data_len > 0) {

        int available_space =
            sizeof(http_response) -
            http_response_length - 1;

        int copy_length = event->data_len;

        if (copy_length > available_space) {
            copy_length = available_space;
        }

        if (copy_length > 0) {
            memcpy(
                http_response + http_response_length,
                event->data,
                copy_length
            );

            http_response_length += copy_length;
            http_response[http_response_length] = '\0';
        }
    }

    return ESP_OK;
}


/* =========================================================
 * Download firmware and install it
 * ========================================================= */
static esp_err_t perform_ota_update(const char *download_url)
{
    if (download_url == NULL || download_url[0] == '\0') {
        ESP_LOGE(TAG, "Invalid firmware download URL");
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Check the OTA partition table before downloading.
     */
    const esp_partition_t *running_partition =
        esp_ota_get_running_partition();

    const esp_partition_t *next_partition =
        esp_ota_get_next_update_partition(NULL);

    if (running_partition == NULL) {
        ESP_LOGE(TAG, "Running application partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(
        TAG,
        "Running partition: %s, address: 0x%08lx, size: %lu bytes",
        running_partition->label,
        (unsigned long)running_partition->address,
        (unsigned long)running_partition->size
    );

    if (next_partition == NULL) {
        ESP_LOGE(TAG, "No passive OTA partition found");
        ESP_LOGE(
            TAG,
            "Partition table must contain otadata, ota_0 and ota_1"
        );
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(
        TAG,
        "Next OTA partition: %s, address: 0x%08lx, size: %lu bytes",
        next_partition->label,
        (unsigned long)next_partition->address,
        (unsigned long)next_partition->size
    );

    ESP_LOGI(TAG, "Starting firmware download...");
    ESP_LOGI(TAG, "Writing firmware to partition: %s",
             next_partition->label);

    esp_http_client_config_t http_config = {
        .url = download_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 60000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 2048
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,

        /*
         * Keep this disabled because the S3 presigned URL previously
         * returned HTTP 403 when partial downloads were enabled.
         */
        .partial_http_download = false
    };

    esp_err_t result = esp_https_ota(&ota_config);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "OTA update failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "OTA update completed successfully");
    ESP_LOGI(TAG, "Restarting into the new firmware...");
    ESP_LOGI(TAG, "================================");

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

/* =========================================================
 * Parse AWS JSON response
 * ========================================================= */
static esp_err_t process_update_response(void)
{
    cJSON *root = cJSON_Parse(http_response);

    if (root == NULL) {
        ESP_LOGE(TAG, "Invalid JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *ready =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "ready"
        );

    cJSON *update_available =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "update_available"
        );

    cJSON *latest_version =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "latest_version"
        );

    cJSON *firmware_status =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "firmware_status"
        );

    cJSON *download_url =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "download_url"
        );

    if (!cJSON_IsTrue(ready)) {
        ESP_LOGE(TAG, "AWS API reported failure");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    if (cJSON_IsString(latest_version)) {
        ESP_LOGI(
            TAG,
            "Current version: %s",
            FIRMWARE_VERSION
        );

        ESP_LOGI(
            TAG,
            "Latest version: %s",
            latest_version->valuestring
        );
    }

    if (cJSON_IsString(firmware_status)) {
        ESP_LOGI(
            TAG,
            "Firmware status: %s",
            firmware_status->valuestring
        );
    }

    if (!cJSON_IsTrue(update_available)) {
        ESP_LOGI(TAG, "No firmware update available");
        cJSON_Delete(root);
        return ESP_OK;
    }

    if (!cJSON_IsString(download_url) ||
        download_url->valuestring == NULL ||
        strlen(download_url->valuestring) == 0) {

        ESP_LOGE(
            TAG,
            "Update exists but download_url is missing"
        );

        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /*
     * Copy the URL before deleting the JSON object,
     * because valuestring belongs to the JSON tree.
     */
    char *firmware_url =
        strdup(download_url->valuestring);

    cJSON_Delete(root);

    if (firmware_url == NULL) {
        ESP_LOGE(TAG, "Failed to allocate firmware URL");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGW(TAG, "New firmware update is available");

    esp_err_t result =
        perform_ota_update(firmware_url);

    free(firmware_url);

    return result;
}

/* =========================================================
 * Request update information from AWS
 * ========================================================= */
static esp_err_t check_for_update(void)
{
    memset(http_response, 0, sizeof(http_response));
    http_response_length = 0;

    ESP_LOGI(TAG, "Checking AWS for updates...");
    ESP_LOGI(TAG, "Device ID: %s", DEVICE_ID);

    esp_http_client_config_t config = {
        .url = "https://glf13cehhe.execute-api.us-east-1.amazonaws.com/check-update",
        .method = HTTP_METHOD_GET,   
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .buffer_size = 2048
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return ESP_FAIL;
    }

    esp_err_t result =
        esp_http_client_perform(client);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "OTA check failed: %s",
            esp_err_to_name(result)
        );

        esp_http_client_cleanup(client);
        return result;
    }

    int status_code =
        esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "HTTP status: %d", status_code);
    ESP_LOGI(TAG, "Server response: %s", http_response);

    esp_http_client_cleanup(client);

    if (status_code != 200) {
        ESP_LOGE(
            TAG,
            "AWS API returned HTTP status %d",
            status_code
        );

        return ESP_FAIL;
    }

    if (http_response_length == 0) {
        ESP_LOGE(TAG, "AWS API returned an empty response");
        return ESP_FAIL;
    }

    return process_update_response();
}

/* =========================================================
 * Main application
 * ========================================================= */
void app_main(void)
{
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "Device: %s", DEVICE_ID);
    ESP_LOGI(TAG, "Firmware version: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "Starting AWS OTA client");
    ESP_LOGI(TAG, "================================");

    esp_err_t nvs_result =
        nvs_flash_init();

    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_result);
    }

    if (wifi_init() == ESP_OK) {
        esp_err_t ota_result =
            check_for_update();

        if (ota_result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Update process ended with error: %s",
                esp_err_to_name(ota_result)
            );
        }
    }

    while (1) {
        ESP_LOGI(
            TAG,
            "Firmware %s is running",
            FIRMWARE_VERSION
        );

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}