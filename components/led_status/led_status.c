#include "led_status.h"

#include <string.h>

#include "esp_log.h"
#include "driver/rmt_tx.h"

static const char *TAG = "led";

/*
 * WS2812B wants 0.3 µs / 0.9 µs pulses. At 10 MHz one RMT tick is 0.1 µs,
 * so the two symbols are 3 and 9 ticks — comfortably inside the part's
 * ±150 ns tolerance, and no clock division to get wrong.
 */
#define LED_RES_HZ  10000000
#define T_SHORT     3
#define T_LONG      9

/* Dim on purpose. This sits on a shelf, often in a bedroom, and full
 * brightness on these LEDs is genuinely unpleasant at night. */
#define LEVEL       24

static struct {
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t enc;
    bool                 ready;
    bool                 enabled;
    led_state_t          state;
} L;

static void send(uint8_t r, uint8_t g, uint8_t b)
{
    if (!L.ready) {
        return;
    }
    /* WS2812 takes green first. */
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(L.chan, L.enc, grb, sizeof(grb), &tx);
    if (err == ESP_OK) {
        /* Block until it is out: the next call reuses the same encoder,
         * and the whole frame is 24 bits — microseconds, not milliseconds. */
        rmt_tx_wait_all_done(L.chan, 100);
    }
}

static void paint(void)
{
    if (!L.enabled) {
        send(0, 0, 0);
        return;
    }
    switch (L.state) {
    case LED_STATE_LINKED: send(0, LEVEL, 0); break;
    case LED_STATE_BOOT:
    default:               send(LEVEL, 0, 0); break;
    }
}

int led_status_init(int gpio, bool enabled)
{
    L.enabled = enabled;
    L.state   = LED_STATE_BOOT;

    if (gpio < 0) {
        ESP_LOGI(TAG, "no status LED configured");
        return 0;
    }

    rmt_tx_channel_config_t cc = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = gpio,
        .mem_block_symbols = 64,
        .resolution_hz     = LED_RES_HZ,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&cc, &L.chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO %d unusable for the LED (%s)", gpio,
                 esp_err_to_name(err));
        return -1;
    }

    rmt_bytes_encoder_config_t bc = {
        .bit0 = { .level0 = 1, .duration0 = T_SHORT,
                  .level1 = 0, .duration1 = T_LONG },
        .bit1 = { .level0 = 1, .duration0 = T_LONG,
                  .level1 = 0, .duration1 = T_SHORT },
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&bc, &L.enc);
    if (err != ESP_OK) {
        rmt_del_channel(L.chan);
        L.chan = NULL;
        ESP_LOGW(TAG, "LED encoder failed (%s)", esp_err_to_name(err));
        return -1;
    }
    if ((err = rmt_enable(L.chan)) != ESP_OK) {
        ESP_LOGW(TAG, "LED enable failed (%s)", esp_err_to_name(err));
        return -1;
    }

    L.ready = true;
    ESP_LOGI(TAG, "status LED on GPIO %d, %s", gpio, enabled ? "on" : "off");
    paint();
    return 0;
}

void led_status_set(led_state_t state)
{
    if (L.state == state) {
        return;
    }
    L.state = state;
    paint();
}

void led_status_enable(bool on)
{
    L.enabled = on;
    paint();
}

bool led_status_enabled(void)
{
    return L.enabled;
}
