#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#include "dht.h"

#define TAG         "esp-wstation"
#define GPIO_DHT    26
#define TEMP_POLINT 60000 / portTICK_PERIOD_MS

#define FAHRENHEIT(t) ((t/10.0f) * 9.0f / 5.0f  + 32.0f)
#define UNITS(x) (x / 10)
#define DCMLS(x) abs(x % 10)

#define FMT_ROOT HOSTNAME "\n\nTemperature: %d.%d`C / %0.2f`F\nHumidity: %d.%d%%\n"
#define FMT_JSON "{\"temp\": %d.%d, \"humidity\": %d.%d}"

int16_t temp, humidity;

static void handler_wifi_disconnected(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
	ESP_LOGW(TAG, "Disconnected from '" WLAN_SSID "'. Attempting to reconnect...");
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
            .ssid = WLAN_SSID,
            .password = WLAN_KEY,
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

    ESP_LOGI(TAG, "Initializing WiFI station with hostname '" HOSTNAME "'");

    // Set hostname
    ESP_ERROR_CHECK(
    	esp_netif_set_hostname(netif, HOSTNAME)
    );

    ESP_LOGI(TAG, "Attempting to connect to SSID '" WLAN_SSID "'...");
	esp_wifi_connect();

    // Wait "for a bit" ;) -- until I have an IP
    EventBits_t wait_bits = xEventGroupWaitBits(
    	wifi_event_group,  // Event group being tested
        1,			       // Bit(s) to wait for
        pdFALSE,
        pdFALSE,
        portMAX_DELAY      // Amount of time to wait
    );

    // Not sure what else to do at this point
    if (! wait_bits) {
    	ESP_LOGE(TAG, "Tragically unable to connect to network. I will die now ;(");
    	exit(1);
    }

    // Is there a good reason to unregister this? What if Wifi gets disconnected?
    // ESP_ERROR_CHECK(
    //    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_event_instance)
    // );

    ESP_ERROR_CHECK(
    	esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_event_instance)
    );
    vEventGroupDelete(wifi_event_group);
}


void poll_weather()
{
    esp_err_t dht_err;
    for (;;) {
        dht_err = dht_read_data(DHT_TYPE_AM2301, GPIO_DHT, &humidity, &temp);

        // TODO: Figure out a better way to signal that current temp is likely out of date
        if (dht_err != ESP_OK) {
            ESP_LOGE(TAG, "Could not determine temperature and humidty: %s",
                esp_err_to_name(dht_err));
        }
        vTaskDelay(TEMP_POLINT);

        ESP_LOGI(TAG, "Temp: %d.%d Humidity: %d.%d", UNITS(temp), DCMLS(temp), UNITS(humidity), DCMLS(humidity));
    }
}


esp_err_t get_root(httpd_req_t *req)
{
    // Set Content-Type: text/html
    ESP_ERROR_CHECK(
        httpd_resp_set_type(req, "text/plain")
    );

    // Format root page and send as response
    char root_page[256];
    sprintf(root_page, FMT_ROOT, UNITS(temp), DCMLS(temp), FAHRENHEIT(temp), UNITS(humidity), DCMLS(humidity));

    ESP_LOGI(TAG, "HTTP GET\n%s", root_page);

    return httpd_resp_send(req, root_page, HTTPD_RESP_USE_STRLEN);
}

httpd_uri_t uri_get_root = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = get_root,
    .user_ctx = NULL
};

esp_err_t get_json(httpd_req_t *req)
{
    // Set Content-Type: application/json
    ESP_ERROR_CHECK(
        httpd_resp_set_type(req, "application/json")
    );

    // Enable CORS
    ESP_ERROR_CHECK(
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*")
    );

    char payload[256];
    sprintf(payload, FMT_JSON, UNITS(temp), DCMLS(temp), UNITS(humidity), DCMLS(humidity));

    ESP_LOGI(TAG, "HTTP GET\n%s", payload);

    return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

httpd_uri_t uri_get_json = {
    .uri      = "/json",
    .method   = HTTP_GET,
    .handler  = get_json,
    .user_ctx = NULL
};


void start_webserver(void)
{
     // Generate default configuration 
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_handle_t server = NULL;

     // Start the httpd server 
    ESP_ERROR_CHECK(
        httpd_start(&server, &config)
    );

    // Register handles for URIs
    httpd_register_uri_handler(server, &uri_get_root);
    httpd_register_uri_handler(server, &uri_get_json);
}

void app_main()
{
    // "Initial" initializations
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    init_wifi();
    
    xTaskCreate(poll_weather, "poll_weather", 4096, NULL, 5, NULL);

    start_webserver();
}