#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_server.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/ip_addr.h"
#include "lwip/ip6_addr.h"

#include "mdns.h"

static const char *TAG = "wifi_softap_sta";

#define WIFI_SSID "SideLights"
#define WIFI_PASS "12345678"

#define MIN(a, b) ((a) < (b) ? (a) : (b))




#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_NUM_14) | (1ULL<<GPIO_NUM_13) | (1ULL<<GPIO_NUM_12))

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event) {
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        ESP_LOGI(TAG, "got ip:%s",
                 ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        esp_wifi_connect();
        ESP_LOGI(TAG, "retry to connect to the AP");
        break;
    case SYSTEM_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "station:" MACSTR " join, AID=%d",
                 MAC2STR(event->event_info.sta_connected.mac),
                 event->event_info.sta_connected.aid);
        break;
    case SYSTEM_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "station:" MACSTR "leave, AID=%d",
                 MAC2STR(event->event_info.sta_disconnected.mac),
                 event->event_info.sta_disconnected.aid);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t save_credentials_handler(httpd_req_t *req) {
    char content[100];
    size_t recv_size = MIN(req->content_len, sizeof(content));
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[recv_size] = '\0';

    char ssid[32] = {0};
    char password[64] = {0};
    sscanf(content, "ssid=%31s&password=%63s", ssid, password);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs_handle, "ssid", ssid);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs_handle, "password", password);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    nvs_close(nvs_handle);

    // Stop SoftAP mode
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Connect to the new WiFi credentials
    wifi_config_t wifi_sta_config = {
        .sta = {
            .ssid = "",
            .password = "",
        },
    };
    strcpy((char *)wifi_sta_config.sta.ssid, ssid);
    strcpy((char *)wifi_sta_config.sta.password, password);

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_sta_config));
    esp_err_t connect_err = esp_wifi_connect();
    if (connect_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to WiFi, error: %s", esp_err_to_name(connect_err));
        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    const char resp[] = "Credentials saved! Connecting to WiFi...";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req) {
    const char resp[] = "<html><body>"
                        "<form action=\"/save\" method=\"POST\">"
                        "SSID: <input type=\"text\" name=\"ssid\"><br>"
                        "Password: <input type=\"password\" name=\"password\"><br>"
                        "<input type=\"submit\" value=\"Save\">"
                        "</form>"
                        "</body></html>";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t control_lights_handler(httpd_req_t *req) {
    char content[100];
    size_t recv_size = MIN(req->content_len, sizeof(content));
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[recv_size] = '\0';

    if (strstr(content, "light1=on")) {
        gpio_set_level(GPIO_NUM_14, 1);
        ESP_LOGI(TAG, "gpio 14 on");
    } else if (strstr(content, "light1=off")) {
        gpio_set_level(GPIO_NUM_14, 0);
        ESP_LOGI(TAG, "gpio 14 off");
    }

    if (strstr(content, "light2=on")) {
        gpio_set_level(GPIO_NUM_13, 1);
        ESP_LOGI(TAG, "gpio 13 on");
    } else if (strstr(content, "light2=off")) {
        gpio_set_level(GPIO_NUM_13, 0);
        ESP_LOGI(TAG, "gpio 13 off");
    }

    if (strstr(content, "light3=on")) {
        gpio_set_level(GPIO_NUM_12, 1);
        ESP_LOGI(TAG, "gpio 12 on");
    } else if (strstr(content, "light3=off")) {
        gpio_set_level(GPIO_NUM_12, 0);
        ESP_LOGI(TAG, "gpio 12 off");
    }

    if (strstr(content, "lights=on")) {
        gpio_set_level(GPIO_NUM_12, 1);
         gpio_set_level(GPIO_NUM_13, 1);
          gpio_set_level(GPIO_NUM_14, 1);
        ESP_LOGI(TAG, "Lights on");
    } else if (strstr(content, "lights=off")) {
        gpio_set_level(GPIO_NUM_12, 0);
        gpio_set_level(GPIO_NUM_13, 0);
        gpio_set_level(GPIO_NUM_14, 0);
        ESP_LOGI(TAG, "Lights on");
    }
    const char resp[] = "Commamd Executed!";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t ota_update_handler(httpd_req_t *req) {
    esp_err_t err;
    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char ota_buff[1024];
    int binary_file_length = 0;
    int data_read;
    while ((data_read = httpd_req_recv(req, ota_buff, sizeof(ota_buff))) > 0) {
        err = esp_ota_write(ota_handle, (const void *)ota_buff, data_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        binary_file_length += data_read;
        ESP_LOGI(TAG, "Written image length %d", binary_file_length);
    }

    if (data_read < 0) {
        if (data_read == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        ESP_LOGE(TAG, "OTA data read error");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    const char resp[] = "OTA update successful! Rebooting...";
    httpd_resp_send(req, resp, strlen(resp));
    ESP_LOGI(TAG, "OTA update successful, rebooting...");
    esp_restart();
    return ESP_OK;
}

static httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
};

static httpd_uri_t save_uri = {
    .uri       = "/save",
    .method    = HTTP_POST,
    .handler   = save_credentials_handler,
    .user_ctx  = NULL
};

static httpd_uri_t control_lights_uri = {
    .uri       = "/control",
    .method    = HTTP_POST,
    .handler   = control_lights_handler,
    .user_ctx  = NULL
};

static httpd_uri_t ota_update_uri = {
    .uri       = "/ota",
    .method    = HTTP_POST,
    .handler   = ota_update_handler,
    .user_ctx  = NULL
};

static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &save_uri);
        httpd_register_uri_handler(server, &control_lights_uri);
        httpd_register_uri_handler(server, &ota_update_uri);
    }
    return server;
}

void init_gpio(void) {
    gpio_config_t io_conf;

    // Configure GPIO 14, 13, 12 as outputs with no pull-up, no pull-down, and no interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
}

void wifi_init_softap_sta(void) {
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

    size_t ssid_len = 32;
    size_t pass_len = 64;
    char ssid[32] = {0};
    char password[64] = {0};
    nvs_get_str(nvs_handle, "ssid", ssid, &ssid_len);
    nvs_get_str(nvs_handle, "password", password, &pass_len);
    nvs_close(nvs_handle);

    if (strlen(ssid) > 0 && strlen(password) > 0) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

        wifi_config_t wifi_sta_config = {
            .sta = {
                .ssid = "",
                .password = "",
            },
        };
        strcpy((char *)wifi_sta_config.sta.ssid, ssid);
        strcpy((char *)wifi_sta_config.sta.password, password);

        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_sta_config));
        esp_err_t connect_err = esp_wifi_connect();
        if (connect_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to connect to WiFi, error: %s", esp_err_to_name(connect_err));
            ESP_ERROR_CHECK(esp_wifi_stop());
           // re-do implement for softAp
             wifi_config_t wifi_ap_config = {
            .ap = {
                .ssid = WIFI_SSID,
                .password = WIFI_PASS,
                .ssid_len = strlen(WIFI_SSID),
                .max_connection = 4,
                .authmode = WIFI_AUTH_WPA_WPA2_PSK
            },
        };
        if (strlen(WIFI_PASS) == 0) {
            wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_ap_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        
        }
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));



        wifi_config_t wifi_ap_config = {
            .ap = {
                .ssid = WIFI_SSID,
                .password = WIFI_PASS,
                .ssid_len = strlen(WIFI_SSID),
                .max_connection = 4,
                .authmode = WIFI_AUTH_WPA_WPA2_PSK
            },
        };
        if (strlen(WIFI_PASS) == 0) {
            wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_ap_config));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
}

void start_mdns_service(void) {
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("sidelights"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Sidelights Application"));
    ESP_LOGI(TAG, "mDNS started with hostname: sidelights.local");
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    init_gpio();
    wifi_init_softap_sta();
    start_webserver();
    start_mdns_service();
}
