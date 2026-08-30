#include "led_status.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

static const char *TAG = "led";

/*
 * WS2812 bit timing. At 10 MHz one RMT tick is 0.1 us, so:
 *   bit 0 -> 0.3 us high, 0.9 us low
 *   bit 1 -> 0.9 us high, 0.3 us low
 * These are the ESP-IDF led_strip example's numbers and sit inside the
 * WS2812/WS2812B/SK6812 tolerance.
 */
#define LED_RES_HZ  10000000
#define T_SHORT     3
#define T_LONG      9

/*
 * The reset/latch gap. WS2812B wants the line held low for >280 us after
 * the last bit before it acts on the data. Older parts want >50 us. A
 * bytes-only encoder does not emit this, and while the seconds-long gap
 * between our updates covers it in steady state, the first frame after a
 * channel is enabled would not latch. So the frame ends with an explicit
 * low period. Split in two because one RMT symbol tops out at 32767 ticks.
 */
#define RESET_TICKS (LED_RES_HZ / 1000000 * 300 / 2)   /* 300 us total */

/* Dim on purpose: this sits on a shelf, often in a bedroom. */
#define LEVEL       24

static struct {
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t bytes_enc;
    rmt_encoder_handle_t copy_enc;
    rmt_symbol_word_t    reset_code;
    int                  gpio;
    bool                 ready;
    bool                 enabled;
    led_state_t          state;
} L;

/* One WS2812 frame: 24 data bits, then the reset low period. */
static void send(uint8_t r, uint8_t g, uint8_t b)
{
    if (!L.ready) {
        return;
    }
    const uint8_t grb[3] = { g, r, b };     /* WS2812 takes green first */
    const rmt_transmit_config_t tx = { .loop_count = 0 };

    esp_err_t err = rmt_transmit(L.chan, L.bytes_enc, grb, sizeof(grb), &tx);
    if (err == ESP_OK) {
        err = rmt_transmit(L.chan, L.copy_enc, &L.reset_code,
                           sizeof(L.reset_code), &tx);
    }
    if (err == ESP_OK) {
        rmt_tx_wait_all_done(L.chan, 100);
    } else {
        ESP_LOGW(TAG, "transmit failed: %s", esp_err_to_name(err));
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

static void teardown(void)
{
    L.ready = false;
    if (L.chan) {
        rmt_disable(L.chan);
        rmt_del_channel(L.chan);
        L.chan = NULL;
    }
    if (L.bytes_enc) {
        rmt_del_encoder(L.bytes_enc);
        L.bytes_enc = NULL;
    }
    if (L.copy_enc) {
        rmt_del_encoder(L.copy_enc);
        L.copy_enc = NULL;
    }
}

static int build(int gpio)
{
    teardown();
    L.gpio = gpio;

    if (gpio < 0) {
        ESP_LOGI(TAG, "status LED disabled (gpio -1)");
        return 0;
    }

    rmt_tx_channel_config_t cc = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = gpio,
        .mem_block_symbols  = 64,
        .resolution_hz     = LED_RES_HZ,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&cc, &L.chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO %d unusable for the LED: %s", gpio,
                 esp_err_to_name(err));
        return -1;
    }

    const rmt_bytes_encoder_config_t bc = {
        .bit0 = { .level0 = 1, .duration0 = T_SHORT,
                  .level1 = 0, .duration1 = T_LONG },
        .bit1 = { .level0 = 1, .duration0 = T_LONG,
                  .level1 = 0, .duration1 = T_SHORT },
        .flags.msb_first = 1,
    };
    const rmt_copy_encoder_config_t copy_cfg = {};

    if ((err = rmt_new_bytes_encoder(&bc, &L.bytes_enc)) != ESP_OK ||
        (err = rmt_new_copy_encoder(&copy_cfg, &L.copy_enc)) != ESP_OK) {
        ESP_LOGW(TAG, "LED encoder failed: %s", esp_err_to_name(err));
        teardown();
        return -1;
    }

    L.reset_code = (rmt_symbol_word_t){
        .level0 = 0, .duration0 = RESET_TICKS,
        .level1 = 0, .duration1 = RESET_TICKS,
    };

    if ((err = rmt_enable(L.chan)) != ESP_OK) {
        ESP_LOGW(TAG, "LED channel enable failed: %s", esp_err_to_name(err));
        teardown();
        return -1;
    }

    L.ready = true;
    ESP_LOGI(TAG, "status LED ready on GPIO %d (%s)", gpio,
             L.enabled ? "on" : "off");
    paint();
    return 0;
}

int led_status_init(int gpio, bool enabled)
{
    L.enabled = enabled;
    L.state   = LED_STATE_BOOT;
    return build(gpio);
}

int led_status_reinit(int gpio)
{
    if (gpio == L.gpio && L.ready) {
        return 0;
    }
    ESP_LOGI(TAG, "moving status LED to GPIO %d", gpio);
    return build(gpio);
}

int led_status_gpio(void)
{
    return L.gpio;
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

bool led_status_identify(void)
{
    if (!L.ready) {
        return false;
    }
    /* Full brightness here — the point is to be unmistakable. Ignores the
     * on/off toggle for the same reason; paint() puts things back after. */
    const uint8_t seq[4][3] = {
        { 64, 0, 0 }, { 0, 64, 0 }, { 0, 0, 64 }, { 0, 0, 0 },
    };
    for (int i = 0; i < 4; i++) {
        send(seq[i][0], seq[i][1], seq[i][2]);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    paint();
    return true;
}
