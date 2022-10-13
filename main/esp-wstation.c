#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#include "dht.h"

#define TAG         "esp-wstation"

#define BLINK_DUR   400
#define BLINK_RATE  50

#define TEMP_POLINT 60000

#define BLINK(ms) (blink_ms = ms)
#define DELAY(ms) (vTaskDelay((ms) / portTICK_PERIOD_MS))
#define FHEIT(t) ((t/10.0f) * 9.0f / 5.0f  + 32.0f)
#define UNITS(x) (x / 10)
#define DCMLS(x) (abs(x % 10))

#define FMT_ROOT "%s\n\nTemperature: %d.%d`C / %0.2f`F\nHumidity: %d.%d%%\n"
#define FMT_JSON "{\"temp\": %d.%d, \"humidity\": %d.%d}"

int16_t temp, humidity, blink_ms;
char *hostname;
esp_err_t sensor_status;

static void handler_wifi_disconnected(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    ESP_LOGW(TAG, "Disconnected from '" CONFIG_SSID "'. Attempting to reconnect...");
    esp_wifi_connect();
}


static void handler_gotip(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    xEventGroupSetBits(*(EventGroupHandle_t*)arg, 1);
}


void init_wifi()
{
    // Create wifi station -- returns pointer to esp-netif instance
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

    // Setup wifi station with the default wifi configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg));

    // Init event group
    EventGroupHandle_t wifi_event_group = xEventGroupCreate();

    esp_event_handler_instance_t wifi_handler_event_instance;
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            WIFI_EVENT_STA_DISCONNECTED,
            &handler_wifi_disconnected,
            NULL,
            &wifi_handler_event_instance
        )
    );

    esp_event_handler_instance_t got_ip_event_instance;
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &handler_gotip,
            // Give event group to handler so it can signal that network is ready
            &wifi_event_group,
            &got_ip_event_instance
        )
    );

    // Set wireless mode to station (client)
    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA));

    // Set wifi config
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_SSID,
            .password = CONFIG_PASSWORD,
         .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    ESP_ERROR_CHECK(
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config)
    );

    // Start the wifi driver
    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    ESP_LOGI(TAG, "Attempting to connect to SSID '" CONFIG_SSID "'...");
    esp_wifi_connect();

    // Wait "for a bit" ;) -- until I have an IP
    EventBits_t wait_bits = xEventGroupWaitBits(
        wifi_event_group,  // Event group being tested
        1,                 // Bit(s) to wait for
        pdFALSE,
        pdFALSE,
        portMAX_DELAY      // Amount of time to wait
    );

    // Get interface hostname
    ESP_ERROR_CHECK(
        esp_netif_get_hostname(netif, (const char **) &hostname)
    );

    // Not sure what else to do at this point
    if (! wait_bits) {
        ESP_LOGE(TAG, "Tragically unable to connect to network. I will die now ;(");
        exit(1);
    }

    ESP_ERROR_CHECK(
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_event_instance)
    );
    vEventGroupDelete(wifi_event_group);
}


void t_poll_sensor()
{
    
    for (;;) {
        sensor_status = dht_read_data(DHT_TYPE_AM2301, CONFIG_PINSEN, &humidity, &temp);

        if (sensor_status != ESP_OK) {
            ESP_LOGE(TAG, "Could not determine temperature and humidty: %s", esp_err_to_name(sensor_status));
        } else {
            ESP_LOGI(TAG, "Latest sensor data: temp=%d humidity=%d", temp, humidity);
        }

        DELAY(TEMP_POLINT);
    }
}


void t_blink_ctrl()
{
    for (;;) {
        if (blink_ms >= BLINK_RATE) {
            gpio_set_level(CONFIG_PINLED, 1);
            DELAY(BLINK_RATE/2);

            gpio_set_level(CONFIG_PINLED, 0);
            DELAY(BLINK_RATE/2);

            blink_ms -= BLINK_RATE;
        } else {
            DELAY(BLINK_RATE);
        }
    }
}

static esp_err_t http_get(httpd_req_t *req, const char *buf, const char *cont_type)
{
    // Blink some lights to let me know something is happening
    BLINK(400);

    esp_err_t resp_error;

    ESP_LOGI(TAG, "HTTP GET");

    if (sensor_status == ESP_OK) {
        httpd_resp_set_type(req, cont_type);
        resp_error = httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        ESP_LOGI(TAG, HTTPD_200);

    } else {
        httpd_resp_set_status(req, HTTPD_500);

        char error_msg[128];
        sprintf(error_msg, "Sensor error: %s\n", esp_err_to_name(sensor_status));

        resp_error = httpd_resp_send(req, error_msg, HTTPD_RESP_USE_STRLEN);
        ESP_LOGE(TAG, HTTPD_500);
    }

    return resp_error;
}


esp_err_t get_root(httpd_req_t *req)
{
    char body[256];
    sprintf(body, FMT_ROOT, hostname, UNITS(temp), DCMLS(temp), FHEIT(temp), UNITS(humidity), DCMLS(humidity));
    
    return http_get(req, body, "text/plain");
}


esp_err_t get_json(httpd_req_t *req)
{
    // Enable CORS
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char body[256];
    sprintf(body, FMT_JSON, UNITS(temp), DCMLS(temp), UNITS(humidity), DCMLS(humidity));

    return http_get(req, body, "application/json");
}

void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

     // Start the httpd server 
    ESP_ERROR_CHECK(
        httpd_start(&server, &config)
    );

    static httpd_uri_t uri_get_root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = get_root,
        .user_ctx = NULL
    };

    static httpd_uri_t uri_get_json = {
        .uri      = "/json",
        .method   = HTTP_GET,
        .handler  = get_json,
        .user_ctx = NULL
    };

    // Register handlers for URIs
    httpd_register_uri_handler(server, &uri_get_root);
    httpd_register_uri_handler(server, &uri_get_json);
}


void app_main()
{
    // "Initial" initializations
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Set up LED
    gpio_reset_pin(CONFIG_PINLED);
    gpio_set_direction(CONFIG_PINLED, GPIO_MODE_OUTPUT);

    init_wifi();
    
    xTaskCreate(t_poll_sensor, "t_poll_sensor", 4096, NULL, 5, NULL);
    xTaskCreate(t_blink_ctrl, "t_blink_ctrl", 4096, NULL, 5, NULL);

    start_webserver();
}