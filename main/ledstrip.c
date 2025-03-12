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
#include "driver/pwm.h"


#define PWM_RED_IO_NUM   14  // RED 
#define PWM_GREEN_IO_NUM 12  // GREEN 
#define PWM_BLUE_IO_NUM  13  // BLUE 

#define PWM_PERIOD    (1000)  // 1KHz



static const char *TAG = "wifi_softap_sta";

#define WIFI_SSID "LedStrip"
#define WIFI_PASS "12345678"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_NUM_14) | (1ULL<<GPIO_NUM_13) | (1ULL<<GPIO_NUM_12))

static int s_retry_num = 0;
static const int MAX_RETRY = 10;
static bool ap_mode_enabled = false;


const uint32_t pin_num[3] = {
    PWM_RED_IO_NUM,
    PWM_GREEN_IO_NUM,
    PWM_BLUE_IO_NUM,
};

// Duty cycle values (0-1000 corresponds to 0%-100%)
uint32_t duties[3] = { 1000, 1000, 1000 };  // Default to LED OFF (common-anode)
float phase[3] = { 0, 0, 0 };  // No phase shift

// Function to set RGB values (adjusted for common-anode)
void set_color(uint32_t r, uint32_t g, uint32_t b) {
    duties[0] = 1000 - r;  // Red
    duties[1] = 1000 - g;  // Green
    duties[2] = 1000 - b;  // Blue

    pwm_set_duties(duties);
    pwm_start();
    ESP_LOGI(TAG, "Color set to R:%d, G:%d, B:%d", r, g, b);
}
// Function to set off
void set_off() {
    set_color(1000, 1000, 1000);
}

// Function to set Red
void set_red() {
set_off();
    set_color(1000, 1000, 0);
}

// Function to set Green
void set_green() {
set_off();
    set_color(1000, 0, 1000);
}

// Function to set Blue
void set_blue() {
set_off();
    
    set_color(0, 1000, 1000);
}

// Function to set White (all LEDs ON with phase shift)
void set_yellow() {
set_off();
    float white_phase[3] = { 45.0, -45.0, -45.0 };  // Adjusted phase shift
    pwm_set_phases(white_phase);  // Apply phase shift
    set_color(500, 500, 500);  // Set all colors to max
}

void set_white() {
set_off();
    float white_phase[3] = { 45.0, -90.0, -45.0 };  // Adjusted phase shift
    pwm_set_phases(white_phase);  // Apply phase shift
    set_color(500, 500, 500);  // Set all colors to max
}


static esp_err_t wifi_event_handler(void *ctx, system_event_t *event) {
    nvs_handle_t nvs_handle;
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        ESP_LOGI(TAG, "got ip:%s",
                 ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
        s_retry_num = 0; // Reset retry count on successful connection
        ap_mode_enabled = false;
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        if (!ap_mode_enabled && s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP (attempt %d)", s_retry_num);
        } else {
            ESP_LOGI(TAG, "failed to connect to the AP after %d attempts, clearing NVS and rebooting", MAX_RETRY);
            // Delete SSID and password from NVS
            ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
            ESP_ERROR_CHECK(nvs_erase_key(nvs_handle, "ssid"));
            ESP_ERROR_CHECK(nvs_erase_key(nvs_handle, "password"));
            ESP_ERROR_CHECK(nvs_commit(nvs_handle));
            nvs_close(nvs_handle);
            ESP_LOGI(TAG, "Deleted SSID and password from NVS");

            // Set a flag in NVS to indicate that the device should start in AP mode after reboot
            ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
            ESP_ERROR_CHECK(nvs_set_u8(nvs_handle, "start_ap_mode", 1));
            ESP_ERROR_CHECK(nvs_commit(nvs_handle));
            nvs_close(nvs_handle);

            // Reboot the system
            esp_restart();
        }
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

    char *ssid_start = strstr(content, "ssid=") + 5;
    char *password_start = strstr(content, "password=") + 9;
    char *ssid_end = strstr(ssid_start, "&");
    size_t ssid_len = ssid_end - ssid_start;
    size_t password_len = strlen(password_start);

    char ssid[32] = {0};
    char password[64] = {0};

    strncpy(ssid, ssid_start, ssid_len);
    strncpy(password, password_start, password_len);

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

    if (strstr(content, "red=on")) {
       // gpio_set_level(GPIO_NUM_14, 1);
        set_red();
        ESP_LOGI(TAG, "Red on");
    } else if (strstr(content, "red=off")) {
       // gpio_set_level(GPIO_NUM_14, 0);
       set_off();
        ESP_LOGI(TAG, "gpio 14 off");
    }

    if (strstr(content, "blue=on")) {
        //gpio_set_level(GPIO_NUM_13, 1);
        set_blue();
        ESP_LOGI(TAG, "Blue on");
    } else if (strstr(content, "light2=off")) {
        gpio_set_level(GPIO_NUM_13, 0);
        ESP_LOGI(TAG, "gpio 13 off");
    }

    if (strstr(content, "green=on")) {
       // gpio_set_level(GPIO_NUM_12, 1);
       set_green();
        ESP_LOGI(TAG, "Green on");
    } else if (strstr(content, "blue=off")) {
     //   gpio_set_level(GPIO_NUM_12, 0);
     set_off();
        ESP_LOGI(TAG, "gpio 12 off");
    }

    if (strstr(content, "yellow=on")) {
  
     
    set_yellow();
        ESP_LOGI(TAG, "Yellow on");
    } else if (strstr(content, "yellow=off")) {
set_off();
        ESP_LOGI(TAG, "Lights off");
    }
    
        if (strstr(content, "white=on")) {
  
     
    set_white();
        ESP_LOGI(TAG, "White on");
    } else if (strstr(content, "white=off")) {
set_off();
        ESP_LOGI(TAG, "Lights off");
    }
    
            if (strstr(content, "lights=off")) {

set_off();
        ESP_LOGI(TAG, "Lights off");
    }
    
    const char resp[] = "Command Executed!";
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
    
       const char resp3[] = "OTA update is now starting. Don't close your browser. ";
    httpd_resp_send(req, resp3, strlen(resp3));
    

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

    const char resp[] = "OTA Update is done! Updating boot partition. Rebooting...";
    httpd_resp_send(req, resp, strlen(resp));
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

 
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

    // Check if the device should start in AP mode
    uint8_t start_ap_mode = 0;
    nvs_get_u8(nvs_handle, "start_ap_mode", &start_ap_mode);
    nvs_close(nvs_handle);

    if (start_ap_mode == 1) {
        ESP_LOGI(TAG, "Starting in AP mode as per NVS flag");
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

        // Clear the flag in NVS
        ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
        ESP_ERROR_CHECK(nvs_set_u8(nvs_handle, "start_ap_mode", 0));
        ESP_ERROR_CHECK(nvs_commit(nvs_handle));
        nvs_close(nvs_handle);
    } else {
        size_t ssid_len = 32;
        size_t pass_len = 64;
        char ssid[32] = {0};
        char password[64] = {0};
        nvs_get_str(nvs_handle, "ssid", ssid, &ssid_len);
        nvs_get_str(nvs_handle, "password", password, &pass_len);
        nvs_close(nvs_handle);

        ESP_LOGI(TAG, "Retrieved SSID: %s, Password: %s from NVS", ssid, password);

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

            ESP_LOGI(TAG, "Connecting to WiFi SSID: %s, Password: %s", ssid, password);

            ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_sta_config));
            esp_err_t connect_err = esp_wifi_connect();
            if (connect_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to connect to WiFi, error: %s", esp_err_to_name(connect_err));
                ESP_ERROR_CHECK(esp_wifi_stop());
                ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
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
}

void start_mdns_service(void) {
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("ledstrip"));
    ESP_ERROR_CHECK(mdns_instance_name_set("ledstrip Application"));
    ESP_LOGI(TAG, "mDNS started with hostname: ledstrip.local");
}

void app_main(void) {


    ESP_ERROR_CHECK(nvs_flash_init());
   // init_gpio();
   
    pwm_init(PWM_PERIOD, duties, 3, pin_num);
    pwm_set_phases(phase);
    pwm_start();
    
    wifi_init_softap_sta();
    start_webserver();
    start_mdns_service();
    
    ESP_LOGI(TAG, "LedStrip is now ready");

}
