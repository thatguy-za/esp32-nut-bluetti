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
 * Bring the LED up on `gpio` (-1 to leave it off). `enabled` is the
 * user's on/off setting; when false the pin is left dark but the setting
 * can still be flipped later without a reboot. Returns 0 on success.
 */
int led_status_init(int gpio, bool enabled);

/*
 * Move the LED to a different pin at runtime — for finding which GPIO a
 * given board wired its addressable LED to without a rebuild. -1 turns it
 * off. Returns 0 on success.
 */
int led_status_reinit(int gpio);

/* The pin currently in use, or -1 if none. */
int led_status_gpio(void);

/* Change what is being shown. Safe before init and when disabled. */
void led_status_set(led_state_t state);

/* The user's toggle. Applies immediately. */
void led_status_enable(bool on);
bool led_status_enabled(void);

/*
 * Flash red, green, blue, then off — a visible "is this the right pin?"
 * check. Runs at full brightness and ignores the on/off toggle. Blocks
 * ~1.6 s. Returns false if no LED is initialised.
 */
bool led_status_identify(void);

#ifdef __cplusplus
}
#endif
