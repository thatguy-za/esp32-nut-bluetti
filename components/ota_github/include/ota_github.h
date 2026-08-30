#pragma once
/*
 * Firmware updates pulled straight from the project's GitHub releases.
 *
 * Two halves: ota_gh_check() lists what is published, ota_gh_start() takes
 * one of those versions and flashes it. The download is plain HTTPS with
 * the ESP-IDF certificate bundle, and the image lands in the spare OTA
 * slot, so a build that will not boot is rolled back by the bootloader.
 *
 * There is no image signature check — trust here is TLS plus GitHub. A
 * device that can be pointed at another host, or whose CA store is
 * tampered with, can be given another image. Secure boot is what fixes
 * that, and it is not enabled in this project.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_GH_MAX_RELEASES 10

typedef struct {
    char version[16];      /* "0.3.0" — the tag with its leading v removed */
    bool prerelease;
    bool newer;            /* newer than what is running */
} ota_gh_release_t;

/*
 * Fetch the release list, newest first. Returns the number written, or -1
 * with a reason in `err` (which is what the admin page shows, so it should
 * read as a sentence).
 */
int ota_gh_check(ota_gh_release_t *out, size_t max, char *err, size_t errlen);

/* Compare "1.2.3" strings: >0 if a is newer, 0 if equal, <0 if older. */
int ota_gh_vercmp(const char *a, const char *b);

typedef enum {
    OTA_GH_IDLE = 0,
    OTA_GH_RUNNING,
    OTA_GH_DONE,       /* flashed; the caller reboots */
    OTA_GH_FAILED,
} ota_gh_state_t;

/*
 * Download and flash the given version in the background. Returns 0 if the
 * task started; only one runs at a time. Progress comes from ota_gh_status().
 */
int ota_gh_start(const char *version);

/* Current state; `pct` is -1 until the size is known. `msg` carries the
 * failure reason when the state is OTA_GH_FAILED. */
ota_gh_state_t ota_gh_status(int *pct, char *msg, size_t msglen);

#ifdef __cplusplus
}
#endif
