#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

// Enter your home Wi-Fi details locally. Do not commit passwords to Git.
#define WIFI_SSID "SUNAHOME2.4GHZ"
#define WIFI_PASS "AHHAM000"

// MG90S is a positional servo, not a continuous-rotation servo.
#define SERVO_PIN GPIO_NUM_18
#define SERVO_MODE LEDC_LOW_SPEED_MODE
#define SERVO_TIMER LEDC_TIMER_0
#define SERVO_CHANNEL LEDC_CHANNEL_0
#define SERVO_RESOLUTION LEDC_TIMER_14_BIT
#define SERVO_PERIOD_US 20000U

// Conservative calibration range: approximately 0-180 degrees.
#define SERVO_MIN_US 900U
#define SERVO_MAX_US 2100U

// Adjust these after testing with the servo disconnected from the switch.
#define SERVO_CENTER_ANGLE 90
#define SERVO_ON_ANGLE 35
#define SERVO_OFF_ANGLE 145
#define SERVO_MOVE_MS 500
#define SERVO_PRESS_PAUSE_MS 200
#define SERVO_RETURN_MS 500

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "LIGHT_CONTROL";
static EventGroupHandle_t wifi_events;

static void servo_write_us(uint32_t pulse_us)
{
    uint32_t duty = (uint32_t)(((uint64_t)pulse_us * (1U << 14)) /
                               SERVO_PERIOD_US);
    ESP_ERROR_CHECK(ledc_set_duty(SERVO_MODE, SERVO_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(SERVO_MODE, SERVO_CHANNEL));
}

static void servo_set_angle(int angle)
{
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;

    uint32_t pulse_us = SERVO_MIN_US +
        ((SERVO_MAX_US - SERVO_MIN_US) * (uint32_t)angle / 180U);
    servo_write_us(pulse_us);
    ESP_LOGI(TAG, "Servo target: %d deg (%lu us)",
             angle, (unsigned long)pulse_us);
}

// The servo goes from center to the requested position and back.
static void servo_press_and_return(int target_angle)
{
    // Move to the requested angle
    servo_set_angle(target_angle);

    // Give the servo time to reach the position
    vTaskDelay(pdMS_TO_TICKS(800));

    // Stay at the target angle
    // No return to center
}
static void servo_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = SERVO_MODE,
        .duty_resolution = SERVO_RESOLUTION,
        .timer_num = SERVO_TIMER,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const ledc_channel_config_t channel = {
        .gpio_num = SERVO_PIN,
        .speed_mode = SERVO_MODE,
        .channel = SERVO_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = SERVO_TIMER,
        .duty = 0,
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    servo_set_angle(SERVO_CENTER_ANGLE);
    ESP_LOGI(TAG, "MG90S positional servo initialized (GPIO18)");
}

static const char html_page[] =
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 Light Controller</title>"
    "<style>body{font-family:Arial;text-align:center;background:#121826;"
    "color:white;padding:32px}button{width:220px;padding:22px;"
    "margin:12px;font-size:23px;border:0;border-radius:14px;color:white}"
    ".on{background:#c84343}.off{background:#159b59}</style>"
    "</head><body><h2>ESP32 Light Controller</h2>"
    "<button class='on' onclick=\"send('/on')\">LIGHT off</button><br>"
    "<button class='off' onclick=\"send('/off')\">LIGHT ON</button>"
    "<p id='status'>Ready</p>"
    "<script>async function send(path){"
    "document.getElementById('status').textContent='Sending...';"
    "try{let r=await fetch(path,{method:'POST'});"
    "document.getElementById('status').textContent=await r.text();}"
    "catch(e){document.getElementById('status').textContent='Connection error';}}"
    "</script></body></html>";

static esp_err_t home_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, html_page);
}

static esp_err_t on_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "ON: center -> %d -> center", SERVO_ON_ANGLE);
    servo_press_and_return(SERVO_ON_ANGLE);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ON movement complete");
}

static esp_err_t off_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OFF: center -> %d -> center", SERVO_OFF_ANGLE);
    servo_press_and_return(SERVO_OFF_ANGLE);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OFF movement complete");
}

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t home = {
        .uri = "/", .method = HTTP_GET, .handler = home_handler
    };
    const httpd_uri_t on = {
        .uri = "/on", .method = HTTP_POST, .handler = on_handler
    };
    const httpd_uri_t off = {
        .uri = "/off", .method = HTTP_POST, .handler = off_handler
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &home));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &on));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &off));
    ESP_LOGI(TAG, "Web server ready");
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Connecting to Wi-Fi...");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d; retrying",
                 event->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Connected! Open http://" IPSTR,
                 IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_events = xEventGroupCreate();
    configASSERT(wifi_events != NULL);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid,
             sizeof(wifi_config.sta.ssid), "%s", WIFI_SSID);
    snprintf((char *)wifi_config.sta.password,
             sizeof(wifi_config.sta.password), "%s", WIFI_PASS);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    servo_init();
    wifi_init();

    ESP_LOGI(TAG, "Waiting for home Wi-Fi and IP address...");
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    start_webserver();
    ESP_LOGI(TAG, "Ready for phone control");
}