/* Replay 5 real DisplayPropertyUpload packets captured from a River 3 UPS
 * (from rabits/ha-ef-ble tests/eflib/test_river3.py) through the actual C
 * inner-packet parser + protobuf reader, and check against ha-ef-ble's
 * documented expected values.
 *
 *   cc -I<comp>/private -I<comp>/include -Ishim ef_river3_test.c ef_stub.c \
 *      <comp>/ef_frame.c <comp>/ef_proto.c -o ef_river3_test
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ef_frame.h"
#include "ef_proto.h"

static const char *PKTS[] = {
"aa132c01292c7537e009014502210101fe157d75687575213750757521374d74353e3875757575207575757528757575751075757575fd74d57ee5747fed7475c57475cd747abd7475d87775757575d87675757575c0760c324c37a57175987175757575ed7c74d57c75807c757575b5ed7974ed7847d57875af7875ed7b9374e07a7575e337e87a7575bd37b57af511857aaa688d7a8668e56555ed6554d5656cdd656cc0657575e337c8657575bd379565aa689d65866885652f8d657fed6474bd6475a564759d6475856475ed6375dd6375c56375cd6347b86375757575f0620c324cb7b86f757575758f69257f737d776580847e7f777d767f717d7165557f777d707f717d7365707f777d727f757f757f757f757f757f757f757f757f757f757f757f757f757f757f757f757f757f757f757f757f757f757f757f753e0a",
"aa135f00b32c7e37e009014502210101fe158e7f7ee67b7ede7b7ed67b7ece7b7ec67b7ebc7b7eb67b7eae7b7ea47b7e9e767e94765d747d76a07a747d76a07a747d76a07a747d76a67d747d76ef7d747e747e747e747e747ed6737ec6737ebe707efc657eee657ede657ece657ec6657e22b0",
"aa136500c82c8a37e009014502210101fe15e28afa8af28a0a8b8a02888a62888a7a888a72888862898a7a898a7a8e8a728e8a4a83944283945a868b0287b87a8426882a9c8c4a9c8a5a9c8a529c8b6a9c8a629c8a7a9c8a729c8832908a4a908a5a908a52908a6a908a72908a00918a12918a5a963b881f06",
"aa132c01292c9237e009014502210101fe159a928f9292f2d0b79292f2d0aa93d2d9df92929292c792929292cf92929292f7929292921a9332990293980a93922293922a939d5a93923f90929292923f919292929227913897bdd04296927f96929292920a9b93329b92679b929292520a9e930a9fa0329f92489f920a9c7493079d929204d00f9d92925ad0529d12f6629d4d8f6a9d618f0282b20a82b332828b3a828b2782929204d02f8292925ad072824d8f7a82618f6282c86a82980a83935a83924283927a83926283920a84923a84922284922a84a05f849292929217853897bd505f8892929292688ec298949a908267639998909a9198969a9682b298909a9798969a94829798909a959892989298929892989298929892989298929892989298929892989298929892989298929892989298929892989298928c2e",
"aa135f00b32c9b37e009014502210101fe156b9a9b039e9b3b9e9b339e9b2b9e9b239e9b599e9b539e9b4b9e9b419e9b7b939b7193b8919893459f919893459f919893459f91989343989198930a98919b919b919b919b919b33969b23969b5b959b19809b0b809b3b809b2b809b23809bbdb3",
};

static int hexb(const char *h, uint8_t *out)
{
    int n = 0;
    for (; h[0] && h[1]; h += 2) {
        int hi, lo;
        sscanf(h, "%1x%1x", &hi, &lo);
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

static int fails;
#define OKF(c, ...) do { printf((c) ? "ok:   " : "FAIL: "); printf(__VA_ARGS__); \
                         printf("\n"); if (!(c)) fails++; } while (0)
static int close_to(float a, float b) { return fabsf(a - b) < 0.05f; }

int main(void)
{
    ecoflow_state_t st;
    memset(&st, 0, sizeof st);
    st.minutes_remaining = -1;
    st.minutes_to_full = -1;
    st.ac_in_watts = -1000.0f;
    st.ac_out_watts = -1000.0f;

    int total_matched = 0;
    for (unsigned i = 0; i < sizeof PKTS / sizeof PKTS[0]; i++) {
        uint8_t buf[512];
        int n = hexb(PKTS[i], buf);
        ef_packet_t p;
        int rc = ef_packet_parse(buf, n, &p);
        OKF(rc == 0, "pkt %u parse (len %d) rc=%d", i, n, rc);
        if (rc != 0) continue;
        OKF(p.src == 0x02 && p.cmd_set == 0xFE && p.cmd_id == 0x15,
            "pkt %u routing src=%02X set=%02X id=%02X", i, p.src, p.cmd_set, p.cmd_id);
        int m = ef_proto_apply_display(p.payload, p.payload_len, &st);
        printf("      pkt %u: payload %zu B, %d fields, soc=%d ac_in=%.2f in=%.0f out=%.0f bms=%.1f temp=%.0f rem_d=%d rem_c=%d ac=%d bkp=%d\n",
               i, p.payload_len, m, st.soc_pct, st.ac_in_watts, st.input_watts,
               st.output_watts, st.battery_watts, st.battery_temp_c,
               st.minutes_remaining, st.minutes_to_full, st.ac_input_present,
               st.backup_mode_on);
        total_matched += m;
    }

    printf("\n-- final state vs ha-ef-ble expected --\n");
    OKF(st.soc_pct == 75,                 "battery SOC = %d (exp 75)", st.soc_pct);
    OKF(close_to(st.ac_in_watts, 43.76f), "ac_input_power = %.2f (exp 43.76)", st.ac_in_watts);
    OKF(close_to(st.input_watts, 56.0f),  "input_power = %.1f (exp 56)", st.input_watts);
    OKF(close_to(st.output_watts, 56.0f), "output_power = %.1f (exp 56)", st.output_watts);
    OKF(close_to(st.battery_watts, -2.0f),"pow_get_bms = %.1f (exp -2.0 => discharging 2W)", st.battery_watts);
    OKF(st.charging == 0,                 "charging = %d (exp 0 - battery is outputting)", st.charging);
    OKF(close_to(st.battery_temp_c, 33.0f),"cell temp = %.0f (exp 33)", st.battery_temp_c);
    OKF(st.ac_input_present == 1,         "plugged_in_ac = %d (exp 1)", st.ac_input_present);
    OKF(st.backup_mode_on == 1,           "energy_backup = %d (exp 1)", st.backup_mode_on);
    OKF(st.minutes_remaining == 3807,     "remaining_discharging = %d (exp 3807)", st.minutes_remaining);
    OKF(st.minutes_to_full == 3827,       "remaining_charging = %d (exp 3827)", st.minutes_to_full);

    printf("\n%s (%d failures, %d total protobuf fields matched)\n",
           fails ? "FAILURES" : "ALL PASS", fails, total_matched);
    return fails ? 1 : 0;
}
