#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "sdkconfig.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "cJSON.h"

#define WIFI_SSID           CONFIG_WIFI_SSID
#define WIFI_PASS           CONFIG_WIFI_PASSWORD
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           5

// #define VERSION_URL        "http://192.168.0.101:8070/version.json"
#define VERSION_URL        CONFIG_OTA_VERSION_URL
#define OTA_BUF_SIZE        1024

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static const char *TAG = "esp32_ota";

// Wifi Event Handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data){
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START){
        esp_wifi_connect();
    }else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED){
        if(s_retry_num < MAX_RETRY){
            esp_wifi_connect();
            s_retry_num ++;
            ESP_LOGI(TAG, "Retrying Connection .... attempt %d", s_retry_num);
        }else{
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP){
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num  = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

//Wifi Initialisation
static void wifi_init_sta(void){
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid       = WIFI_SSID,
            .password   = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if(bits & WIFI_CONNECTED_BIT){
        ESP_LOGI(TAG, "Connected to Wifi Successfully");
    }else{
        ESP_LOGE(TAG, "Failed to Connect to Wifi");
    }
}

// Version Check -----------------------------------------------------------------------------------------------------

static bool parse_semver(const char *ver, int *major, int *minor, int *patch){
    return sscanf(ver, "%d.%d.%d", major, minor, patch) == 3;
}

// returns true if a is newer than b
static bool version_newer(const char *a, const char *b){
    int a_maj, a_min, a_pat, b_maj, b_min, b_pat;
    if(!parse_semver(a, &a_maj, &a_min, &a_pat) || !parse_semver(b, &b_maj, &b_min, &b_pat)){
        return false;
    }

    if(a_maj != b_maj) return a_maj > b_maj;
    if(a_min != b_min) return a_min > b_min;
    return a_pat > b_pat;
}

//compares firmware url to version.json
//If an update is available, copies the firmware url into out url and returns true
static bool check_for_update(char *out_url, size_t url_len){
    char response_buf[256] = {0};

    esp_http_client_config_t config = {
        .url = VERSION_URL,
        .method = HTTP_METHOD_GET
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    esp_err_t err = esp_http_client_open(client, 0);

    if(err != ESP_OK){
        ESP_LOGE(TAG, "Failed to open version check connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int data_read = esp_http_client_read_response(client, response_buf, sizeof(response_buf));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if(data_read <=0){
        ESP_LOGE(TAG, "Failed to read version.json");
        return false;
    }

    ESP_LOGI(TAG, "version.json: %s", response_buf);

    //Parse JSON
    cJSON *root = cJSON_Parse(response_buf);
    if(!root){
        ESP_LOGE(TAG, "JSON parse failed");
        return false;
    }

    cJSON *ver_item = cJSON_GetObjectItem(root, "version");
    cJSON *url_item = cJSON_GetObjectItem(root, "url");

    if(!cJSON_IsString(ver_item) || !cJSON_IsString(url_item)){
        ESP_LOGE(TAG, "version.json is missing either 'version', 'url' or both");
        return false;
    }

    const char *server_ver = ver_item->valuestring;
    const char *running_ver = esp_ota_get_app_description()->version;
    
    ESP_LOGI(TAG, "Running: %s  |  Server: %s", running_ver, server_ver);
    
    bool update_needed = version_newer(server_ver, running_ver);
    if(update_needed){
        strncpy(out_url, url_item->valuestring, url_len - 1);
        out_url[url_len - 1] = '\0';
        ESP_LOGI(TAG, "Update available: %s -> %s", running_ver, server_ver);
    }else{
        ESP_LOGI(TAG, "Firmware is up to date - not OTA update needed");
    }

    cJSON_Delete(root);
    return update_needed;
}

// OTA Update --------------------------------------------------------------------------------------------------------

static void ota_update(const char *url){
    ESP_LOGI(TAG, "Starting OTA Update");

    // Step 1: Find the next OTA partition to write into
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if(update_partition == NULL){
        ESP_LOGE(TAG, "No OTA Partion found");
        return;
    }
    ESP_LOGI(TAG, "Writing to partition: %s at offest 0x%lx", update_partition->label, update_partition->address);

    // Step 2: Begin OTA
    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "ota_partition_begin() failed: %s", esp_err_to_name(err));
        return;
    }

    // Step 3: Open HTTP connection and stream firmware
    esp_http_client_config_t config = {
        .url    = url,
        .method = HTTP_METHOD_GET,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    err = esp_http_client_open(client, 0);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_ota_abort(ota_handle);
        esp_http_client_cleanup(client);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "Firmware size: %d bytes", content_length);

    // Step 4: Write chunks as they arrive
    char buf[OTA_BUF_SIZE];
    int total_written = 0;

    while(1){
        int data_read = esp_http_client_read(client, buf, OTA_BUF_SIZE);

        if(data_read<0){
            ESP_LOGE(TAG, "Error reading HTTP stream");
            esp_ota_abort(ota_handle);
            esp_http_client_cleanup(client);
            return;
        }

        if(data_read == 0){
            //Check if we are done
            if(esp_http_client_is_complete_data_received(client)){
                ESP_LOGI(TAG, "Download Complete, total: %d bytes", total_written);
                break;
            }
            continue;
        }

        err = esp_ota_write(ota_handle, buf, data_read);
        if(err != ESP_OK){
            ESP_LOGE(TAG, "esp_ota_write() failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            esp_http_client_cleanup(client);
            return;
        }

        total_written += data_read;
        ESP_LOGI(TAG, "Written %d/%d bytes", total_written, content_length);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // Step 5: Validate the written image
    err = esp_ota_end(ota_handle);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "esp_ota_end() failed: %s", esp_err_to_name(err));
        return;
    }

    // Step 6: Set boot partition and restart
    err = esp_ota_set_boot_partition(update_partition);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "OTA Successful, restarting...");
    esp_restart();

}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 OTA Project");

    // NVS Init
    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND){
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret =  nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    //Connect to Wifi
    wifi_init_sta();

    //Marks firmware as valid now that wifi is up
    esp_ota_mark_app_valid_cancel_rollback();

    //OTA Update if newer version available
    char ota_url[256] = {0};
    if(check_for_update(ota_url, sizeof(ota_url))){
        ota_update(ota_url);
    }

}
