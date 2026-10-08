#include <stdio.h>
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "radio.h"
#include "xbee_frame.h"

static const char *TAG = "laser";

static const xbee_link_t LINK = { .pan = PAN_ID, .src = MONITOR_ID, .dst = DEST_ADDR };
static uint8_t s_seq, s_digi;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static int laser_read(void)
{
    return gpio_get_level(LASER_GPIO) == LASER_ACTIVE_HIGH;
}

static bool send_cmd(uint8_t cmd)
{
    uint8_t frame[XBEE_FRAME_BUF];
    xbee_build_frame(frame, &LINK, s_seq++, s_digi++, cmd);

    for (int i = 0; i < MAX_TRIES; i++) {
        radio_result_t r = radio_send(frame, ACK_WAIT_MS);
        if (r == RADIO_ACKED) {
            ESP_LOGI(TAG, "cmd %u acked (try %d)", cmd, i + 1);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2 + (esp_random() & 7)));
    }
    ESP_LOGW(TAG, "cmd %u: no ACK after %d tries", cmd, MAX_TRIES);
    return false;
}

static void gpio_setup(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << LASER_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = LASER_PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io);
}

static void tx_task(void *arg)
{
    int state = laser_read(), cand = state;
    int64_t t_change = now_ms(), t_sent = 0;

    for (;;) {
        int v = laser_read();
        int64_t t = now_ms();
        if (v != cand) { cand = v; t_change = t; }

        bool changed = cand != state && t - t_change >= DEBOUNCE_MS;
        if (changed) state = cand;

        if (changed || t - t_sent >= RESEND_INTERVAL_MS) {
            send_cmd(xbee_cmd(MONITOR_ID, state));
            t_sent = t;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void sniff_task(void *arg)
{
    uint8_t f[128];
    int8_t rssi;
    for (;;) {
        if (!radio_recv(f, 1000, &rssi)) continue;
        printf("[%4d dBm] len %2u:", rssi, f[0]);
        for (int i = 1; i <= f[0]; i++) printf(" %02X", f[i]);
        printf("\n");
    }
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_seq = esp_random();
    s_digi = esp_random();
    radio_init(RF_CHANNEL, TX_POWER_DBM, PAN_ID, MONITOR_ID, SNIFF_MODE);

#if SNIFF_MODE
    xTaskCreate(sniff_task, "sniff", 4096, NULL, 5, NULL);
#else
    gpio_setup();
    xTaskCreate(tx_task, "tx", 4096, NULL, 5, NULL);
#endif
}
