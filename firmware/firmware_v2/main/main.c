#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"

/* =========================================================
 * Firmware V2 information
 * ========================================================= */

#define FIRMWARE_VERSION   "2.0.0"
#define DEVICE_ID          "esp32-01"

static const char *TAG = "FIRMWARE_V2";

/* =========================================================
 * Print running partition information
 * ========================================================= */

static void print_partition_information(void)
{
    const esp_partition_t *running_partition =
        esp_ota_get_running_partition();

    if (running_partition == NULL) {
        ESP_LOGE(
            TAG,
            "Could not determine the running partition"
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "Running partition: %s",
        running_partition->label
    );

    ESP_LOGI(
        TAG,
        "Partition address: 0x%08lx",
        (unsigned long)running_partition->address
    );

    ESP_LOGI(
        TAG,
        "Partition size: %lu bytes",
        (unsigned long)running_partition->size
    );
}

/* =========================================================
 * Print application information
 * ========================================================= */

static void print_application_information(void)
{
    const esp_app_desc_t *app_description =
        esp_app_get_description();

    if (app_description == NULL) {
        ESP_LOGE(
            TAG,
            "Could not read application information"
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "Project name: %s",
        app_description->project_name
    );

    ESP_LOGI(
        TAG,
        "Application version: %s",
        app_description->version
    );

    ESP_LOGI(
        TAG,
        "Build date: %s",
        app_description->date
    );

    ESP_LOGI(
        TAG,
        "Build time: %s",
        app_description->time
    );

    ESP_LOGI(
        TAG,
        "ESP-IDF version: %s",
        app_description->idf_ver
    );
}

/* =========================================================
 * Firmware V2 main task
 * ========================================================= */

static void firmware_v2_task(void *parameter)
{
    uint32_t running_seconds = 0;

    while (true) {
        ESP_LOGI(
            TAG,
            "Firmware V2 is running successfully"
        );

        ESP_LOGI(
            TAG,
            "Device ID: %s | Version: %s | Uptime: %lu seconds",
            DEVICE_ID,
            FIRMWARE_VERSION,
            (unsigned long)running_seconds
        );

        running_seconds += 5;

        vTaskDelay(
            pdMS_TO_TICKS(5000)
        );
    }
}

/* =========================================================
 * Application entry point
 * ========================================================= */

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "ESP32 Firmware V2 started successfully"
    );

    ESP_LOGI(
        TAG,
        "Device ID: %s",
        DEVICE_ID
    );

    ESP_LOGI(
        TAG,
        "Firmware version: %s",
        FIRMWARE_VERSION
    );

    ESP_LOGI(
        TAG,
        "OTA update from V1 to V2 was successful"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );

    print_application_information();
    print_partition_information();

    BaseType_t task_result = xTaskCreate(
        firmware_v2_task,
        "firmware_v2_task",
        4096,
        NULL,
        5,
        NULL
    );

    if (task_result != pdPASS) {
        ESP_LOGE(
            TAG,
            "Failed to create Firmware V2 task"
        );
    }
}