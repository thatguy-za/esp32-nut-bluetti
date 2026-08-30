#pragma once
/*
 * The board's status LED.
 *
 * Red from boot until the BLUETTI link is up, green once it is — so the
 * bridge's actual job is readable from across the room without opening
 * the admin page.
 *
 * Drives a single WS2812-style addressable LED, which is what the common
 * ESP32-S3 dev boards fit and what makes two colours possible from one
 * pin. A board with a plain single-colour LED will not light: there is
 * nothing to send a colour to.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_STATE_BOOT = 0,   /* red: running, not talking to the unit yet */
    LED_STATE_LINKED,     /* green: connected over BLE                 */
} led_state_t;

/*
 * Bring the LED up on `gpio`. `enabled` is the user's on/off setting;
 * when false the pin is left dark but the setting can still be flipped
 * later without a reboot. Returns 0 on success.
 */
int led_status_init(int gpio, bool enabled);

/* Change what is being shown. Safe before init and when disabled. */
void led_status_set(led_state_t state);

/* The user's toggle. Applies immediately. */
void led_status_enable(bool on);
bool led_status_enabled(void);

#ifdef __cplusplus
}
#endif
