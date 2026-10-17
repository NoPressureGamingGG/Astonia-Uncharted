#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "server.h"
#include "client.h"
#include "effect.h"
#include "death.h"
#include "mem.h"
#include "dps.h"

struct dps_state {
    int serial, cursor, sent;
    uint32_t tick;
    uint64_t buckets[DPS_WINDOW_TICKS], sum, peak, last;
    uint32_t sent_values[3];
};
static struct dps_state states[TOTAL_MAXCHARS];
struct dps_owner { int effect_serial, source, source_serial; };
static struct dps_owner *owners;
static int owner_capacity;
static int guarded_source, guarded_serial, guarded;

void dps_reset(int cn) {
    if (cn > 0 && cn < TOTAL_MAXCHARS) memset(states + cn, 0, sizeof(states[cn]));
}

static struct dps_state *dps_state(int cn) {
    struct dps_state *s;
    uint32_t elapsed;
    if (cn < 1 || cn >= MAXCHARS || cn >= TOTAL_MAXCHARS || !(ch[cn].flags & CF_PLAYER)) return NULL;
    s = states + cn;
    if (s->serial != ch[cn].serial) {
        dps_reset(cn); s->serial = ch[cn].serial; s->tick = (uint32_t)ticker;
    }
    elapsed = (uint32_t)ticker - s->tick;
    if (elapsed >= DPS_WINDOW_TICKS) {
        memset(s->buckets, 0, sizeof(s->buckets)); s->sum = 0; s->cursor = 0;
    } else while (elapsed--) {
        s->cursor = (s->cursor + 1) % DPS_WINDOW_TICKS;
        s->sum -= s->buckets[s->cursor]; s->buckets[s->cursor] = 0;
    }
    s->tick = (uint32_t)ticker;
    return s;
}

int dps_damage_source_serial(int cn) {
    int expected = guarded ? (guarded_source == cn ? guarded_serial : 0) :
        (cn > 0 && cn < MAXCHARS ? ch[cn].serial : 0);
    guarded = 0; /* Only this hurt consumes the override; callbacks have their own owner. */
    return expected;
}

void dps_record_damage(int cn, int serial, int target, int hp_damage, int shield_damage) {
    struct dps_state *s;
    uint64_t amount, accepted;
    if (cn < 1 || cn >= MAXCHARS || cn == target || !serial || ch[cn].serial != serial) return;
    s = dps_state(cn); if (!s) return;
    amount = (uint64_t)(hp_damage > 0 ? hp_damage : 0) + (uint64_t)(shield_damage > 0 ? shield_damage : 0);
    if (!amount) return;
    accepted = amount < UINT64_MAX - s->sum ? amount : UINT64_MAX - s->sum;
    s->buckets[s->cursor] += accepted; s->sum += accepted; s->last = amount;
    if (s->sum / 5 > s->peak) s->peak = s->sum / 5;
}

static uint32_t dps_wire_value(uint64_t value) { return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value; }
int dps_build_packet(int cn, int version, unsigned char *buf) {
    struct dps_state *s;
    uint32_t values[3];
    int i, n;
    if (version < DPS_CLIENT_VERSION) return 0;
    s = dps_state(cn); if (!s) return 0;
    values[0] = dps_wire_value(s->sum / 5); values[1] = dps_wire_value(s->peak); values[2] = dps_wire_value(s->last);
    if (s->sent && !memcmp(values, s->sent_values, sizeof(values))) return 0;
    buf[0] = SV_DPS;
    for (i = 0; i < 3; i++) for (n = 0; n < 4; n++) buf[1 + i * 4 + n] = (unsigned char)(values[i] >> (n * 8));
    memcpy(s->sent_values, values, sizeof(values)); s->sent = 1;
    return DPS_PACKET_SIZE;
}

void dps_bind_effect(int fn, int source) {
    if (!owners) {
        if (MAXEFFECT < 1 || (size_t)MAXEFFECT > INT_MAX / sizeof(*owners)) return;
        owners = xcalloc((int)((size_t)MAXEFFECT * sizeof(*owners)), IM_BASE);
        if (!owners) return;
        owner_capacity = MAXEFFECT;
    }
    if (fn < 1 || fn >= MAXEFFECT || fn >= owner_capacity) return;
    owners[fn].effect_serial = ef[fn].serial; owners[fn].source = source;
    owners[fn].source_serial = source > 0 && source < MAXCHARS && ch[source].flags ? ch[source].serial : 0;
}
void dps_copy_effect_owner(int to, int from) {
    if (!owners || to < 1 || from < 1 || to >= MAXEFFECT || from >= MAXEFFECT || to >= owner_capacity || from >= owner_capacity) return;
    if (owners[from].effect_serial != ef[from].serial) { owners[to].source_serial = 0; return; }
    owners[to] = owners[from]; owners[to].effect_serial = ef[to].serial;
}
void dps_forget_effect(int fn) {
    if (owners && fn > 0 && fn < owner_capacity) memset(owners + fn, 0, sizeof(owners[fn]));
}
int dps_hurt_effect(int fn, int target, int damage, int source, int armordiv, int armorper, int shieldper) {
    int result, old_guard = guarded, old_source = guarded_source, old_serial = guarded_serial;
    guarded = 1; guarded_source = source; guarded_serial = 0;
    if (owners && fn > 0 && fn < MAXEFFECT && fn < owner_capacity &&
        owners[fn].effect_serial == ef[fn].serial && owners[fn].source == source)
        guarded_serial = owners[fn].source_serial;
    result = hurt(target, damage, source, armordiv, armorper, shieldper);
    guarded = old_guard; guarded_source = old_source; guarded_serial = old_serial;
    return result;
}
