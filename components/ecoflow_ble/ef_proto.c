#include "ef_proto.h"

#include <string.h>
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "ef_proto";

/* pr705 DisplayPropertyUpload field numbers (see ha-ef-ble / pr705.proto). */
enum {
    F_ERRCODE                 = 1,    /* uint32 */
    F_ENERGY_BACKUP_EN        = 7,    /* bool   */
    F_ENERGY_BACKUP_START_SOC = 8,    /* uint32 */
    F_POW_IN_SUM_W            = 3,    /* float  */
    F_POW_OUT_SUM_W           = 4,    /* float  */
    F_PCS_FAN_LEVEL           = 30,   /* uint32 */
    F_POW_GET_AC_IN           = 54,   /* float  */
    F_POW_GET_BMS             = 158,  /* float  */
    F_PLUG_AC_CHARGER_FLAG    = 202,  /* bool   */
    F_BMS_MAX_CELL_TEMP       = 259,  /* int32  */
    F_CMS_BATT_SOC            = 262,  /* float  */
    F_CMS_DSG_REM_TIME        = 268,  /* uint32 */
    F_CMS_CHG_REM_TIME        = 269,  /* uint32 */
    F_CMS_MAX_CHG_SOC         = 270,  /* uint32 */
    F_CMS_MIN_DSG_SOC         = 271,  /* uint32 */
    F_POW_GET_AC_OUT          = 368,  /* float  */
};

static bool read_varint(const uint8_t *p, size_t len, size_t *pos, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (*pos < len && shift < 64) {
        uint8_t b = p[(*pos)++];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return true;
        }
        shift += 7;
    }
    return false;
}

static float f32le(const uint8_t *p)
{
    uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static int iround(float f)
{
    return (int)(f < 0 ? f - 0.5f : f + 0.5f);
}

int ef_proto_apply_display(const uint8_t *pb, size_t len, ecoflow_state_t *st)
{
    size_t pos = 0;
    int matched = 0;
    int dev_low = -1, dev_max = -1;

    while (pos < len) {
        uint64_t tag;
        if (!read_varint(pb, len, &pos, &tag)) {
            break;
        }
        uint32_t field = (uint32_t)(tag >> 3);
        uint8_t wire = (uint8_t)(tag & 0x07);

        uint64_t vint = 0;
        const uint8_t *v32 = NULL;

        switch (wire) {
        case 0: /* varint */
            if (!read_varint(pb, len, &pos, &vint)) {
                return matched;
            }
            break;
        case 5: /* 32-bit */
            if (pos + 4 > len) {
                return matched;
            }
            v32 = &pb[pos];
            pos += 4;
            break;
        case 1: /* 64-bit */
            if (pos + 8 > len) {
                return matched;
            }
            pos += 8;
            continue;
        case 2: { /* length-delimited */
            uint64_t l;
            if (!read_varint(pb, len, &pos, &l) || pos + l > len) {
                return matched;
            }
            pos += (size_t)l;
            continue;
        }
        default:
            return matched;   /* groups / unknown: bail */
        }

        switch (field) {
        case F_CMS_BATT_SOC:
            if (v32) { st->soc_pct = iround(f32le(v32)); matched++; }
            break;
        case F_POW_IN_SUM_W:
            if (v32) { st->input_watts = f32le(v32); matched++; }
            break;
        case F_POW_OUT_SUM_W:
            if (v32) { st->output_watts = f32le(v32); matched++; }
            break;
        case F_POW_GET_AC_IN:
            if (v32) { st->ac_in_watts = f32le(v32); matched++; }
            break;
        case F_POW_GET_AC_OUT:
            if (v32) { st->ac_out_watts = f32le(v32); matched++; }
            break;
        case F_POW_GET_BMS:
            if (v32) { st->battery_watts = f32le(v32); matched++; }
            break;
        case F_PLUG_AC_CHARGER_FLAG:
            st->ac_input_present = (vint != 0);
            matched++;
            break;
        case F_CMS_DSG_REM_TIME:
            st->minutes_remaining = (int)vint;
            matched++;
            break;
        case F_CMS_CHG_REM_TIME:
            st->minutes_to_full = (int)vint;
            matched++;
            break;
        case F_BMS_MAX_CELL_TEMP:
            st->battery_temp_c = (float)(int32_t)vint;
            matched++;
            break;
        case F_CMS_MIN_DSG_SOC:
            dev_low = (int)vint;
            matched++;
            break;
        case F_CMS_MAX_CHG_SOC:
            dev_max = (int)vint;
            matched++;
            break;
        case F_ENERGY_BACKUP_EN:
            st->backup_mode_on = (vint != 0);
            matched++;
            break;
        case F_ENERGY_BACKUP_START_SOC:
            st->backup_reserve_pct = (int)vint;
            matched++;
            break;
        case F_ERRCODE:
            st->error_code = (uint32_t)vint;
            matched++;
            break;
        default:
            break;
        }
    }

    if (matched == 0) {
        return 0;
    }

    /* Derived fields. `pow_get_bms` sign: negative = battery discharging,
     * positive = charging (matches ha-ef-ble's battery_input/output split). */
    st->charging = st->ac_input_present && st->battery_watts > 1.0f;
    if (dev_low >= 0 && dev_low > st->soc_low_pct) {
        st->soc_low_pct = dev_low;
    }
    (void)dev_max;
    st->valid = true;
    st->updated_us = esp_timer_get_time();
    ESP_LOGD(TAG, "display: soc=%d ac_in=%d bms=%.0fW rem=%dmin fields=%d",
             st->soc_pct, st->ac_input_present, st->battery_watts,
             st->minutes_remaining, matched);
    return matched;
}
