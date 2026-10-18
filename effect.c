/*
 * Part of Astonia Server (c) Daniel Brockhaus. Please read license.txt.
 */

#include <stdlib.h>
#include <stdio.h>

#include "server.h"
#include "log.h"
#include "notify.h"
#include "death.h"
#include "light.h"
#include "tool.h"
#include "spell.h"
#include "los.h"
#include "mem.h"
#include "sector.h"
#include "create.h"
#include "item_id.h"
#include "talk.h"
#include "balance.h"
#include "drvlib.h"
#include "map.h"
#include "consistency.h"
#include "database.h"
#include "effect.h"
#include "dps.h"

// ********** effect allocation and freeing *************

struct plague_contact { int cn, serial; struct plague_contact *next; };
static struct plague_contact **plague_contacts;
static void plague_clear_contacts(int fn) {
    struct plague_contact *p, *next;
    if (!plague_contacts) return;
    for (p = plague_contacts[fn]; p; p = next) { next = p->next; xfree(p); }
    plague_contacts[fn] = NULL;
}
static void plague_apply_contact(int fn, int x, int y);

static struct effect *ueffect, *feffect;

// RGB888 fallback for a packed 5:5:5 colour (same formula live uses for its
// legacy-colour expansion); 0 stays 0, the client default.
static uint32_t effect_color32_expand(unsigned short color) {
    uint32_t r = (color >> 10) & 0x1F;
    uint32_t g = (color >> 5) & 0x1F;
    uint32_t b = color & 0x1F;
    return ((r * 255u / 31u) << 16) | ((g * 255u / 31u) << 8) | (b * 255u / 31u);
}

int used_effects, efserial = 1;

// get new effect from free list
// and add it to used list
int alloc_effect(int type) {
    int fn;
    struct effect *tmp;

    // get free effect
    tmp = feffect;
    if (!tmp) {
        elog("alloc_effect: MAXEFFECT reached!");
        return 0;
    }
    feffect = tmp->next;
    fn = tmp - ef;

    // flags effect as used
    ef[fn].type = type;
    ef[fn].start = ticker;
    // a reused slot must not inherit a previous effect's look (e.g. a green Plague burst)
    ef[fn].spell_color = 0;
    ef[fn].spell_color32 = 0;
    ef[fn].variant = 0;
    ef[fn].burn_kind = 0;

    // add effect to used list
    ef[fn].next = ueffect;
    ef[fn].prev = NULL;
    if (ueffect) ueffect->prev = ef + fn;
    ueffect = ef + fn;

    used_effects++;

    ef[fn].serial = efserial++;

    return fn;
}

// release (delete) effect to free list
// and remove it from used list
void free_effect(int fn) {
    struct effect *next, *prev;

    if (!ef[fn].type) {
        elog("trying to free already free effect %d", fn);
        return;
    }

    plague_clear_contacts(fn);
    dps_forget_effect(fn);

    // remove character from used list
    prev = ef[fn].prev;
    next = ef[fn].next;

    if (prev) prev->next = next;
    else ueffect = next;

    if (next) next->prev = prev;

    // add effect to free list
    ef[fn].next = feffect;
    feffect = ef + fn;

    // flag effect as unused
    ef[fn].type = 0;

    ef[fn].serial = 0;

    used_effects--;
}

// get first effect in used list
int getfirst_effect(void) {
    if (!ueffect) return 0;
    return ueffect - ef;
}

// get next effect in used list
// (following a getfirst)
int getnext_effect(int fn) {
    struct effect *tmp;

    if (!(tmp = ef[fn].next)) return 0;

    return tmp - ef;
}

// *********** basic effect helpers ***********************

// add a marker on the map on x,y which links back to effect fn
int set_effect_map(int fn, int x, int y) {
    int m, n;

    if (fn < 1 || fn >= MAXEFFECT) return 0;
    if (!ef[fn].type) return 0;

    if (x < 1 || x >= MAXMAP - 1 || y < 1 || y >= MAXMAP - 1) return 0;

    m = x + y * MAXMAP;

    if (ef[fn].field_cnt >= MAXFIELD) return 0;

    for (n = 0; n < 4; n++) {
        if (!map[m].ef[n]) break;
    }
    if (n == 4) return 0;

    map[m].ef[n] = fn;

    if (ef[fn].light) add_effect_light(x, y, ef[fn].light);

    ef[fn].m[ef[fn].field_cnt] = m;
    ef[fn].field_cnt++;

    set_sector(x, y);

    return 1;
}

// remove link to effect fn from map tile m
int remove_effect_map(int fn, int m) {
    int n;

    if (m < 0 || m >= MAXMAP * MAXMAP) return 0;

    for (n = 0; n < 4; n++) {
        if (map[m].ef[n] == fn) break;
    }
    if (n == 4) return 0;

    if (ef[fn].light) remove_effect_light(m % MAXMAP, m / MAXMAP, ef[fn].light);

    map[m].ef[n] = 0;

    set_sector(m % MAXMAP, m / MAXMAP);

    return 1;
}

int add_effect_char(int fn, int cn) {
    int n;

    if (fn < 1 || fn >= MAXEFFECT) return 0;
    if (!ef[fn].type) return 0;

    if (cn < 1 || cn >= MAXCHARS) return 0;

    for (n = 0; n < 4; n++) {
        if (!ch[cn].ef[n]) break;
    }
    if (n == 4) return 0;

    ch[cn].ef[n] = fn;
    ef[fn].efcn = cn;

    if (ef[fn].light) update_char(cn);
    else set_sector(ch[cn].x, ch[cn].y);

    return 1;
}

int remove_effect_char(int fn) {
    int n, cn;

    cn = ef[fn].efcn;

    if (cn < 1 || cn >= MAXCHARS) return 0;

    for (n = 0; n < 4; n++) {
        if (ch[cn].ef[n] == fn) break;
    }
    if (n == 4) return 0;

    ch[cn].ef[n] = 0;
    ef[fn].efcn = 0;

    if (ef[fn].light) update_char(cn);
    else set_sector(ch[cn].x, ch[cn].y);

    return 1;
}

int destroy_effect_type(int cn, int type) {
    int n, fn = 0, flag = 0;

    if (cn < 1 || cn >= MAXCHARS) return 0;

    for (n = 0; n < 4; n++) {
        if ((fn = ch[cn].ef[n]) && ef[fn].type == type) {
            flag = 1;
            ch[cn].ef[n] = 0;
            ef[fn].efcn = 0;
            free_effect(fn);
        }
    }
    if (flag) {
        if (ef[fn].light) update_char(cn);
        else set_sector(ch[cn].x, ch[cn].y);
        return 1;
    }

    return 0;
}

// completely remove effect fn from
// the map
int remove_effect(int fn) {
    int n;

    if (fn < 1 || fn >= MAXEFFECT) return 0;
    if (!ef[fn].type) return 0;

    for (n = 0; n < ef[fn].field_cnt; n++) {
        remove_effect_map(fn, ef[fn].m[n]);
    }

    ef[fn].field_cnt = 0;

    if (ef[fn].efcn) remove_effect_char(fn);

    return 1;
}

int effect_changed(int fn) {
    int n;

    if (fn < 1 || fn >= MAXEFFECT) return 0;
    if (!ef[fn].type) return 0;

    for (n = 0; n < ef[fn].field_cnt; n++) {
        set_sector(ef[fn].m[n] % MAXMAP, ef[fn].m[n] / MAXMAP);
    }

    return 1;
}

// ****** Fireball and helpers ***********

// Short post-burn reapplication guard for the cosmetic Fireball experiment.
// This is runtime state only; it is intentionally not persisted as character data.
static int fireball_burn_cooldown[TOTAL_MAXCHARS];
#define PLAGUE_MAX_JUMPS 3 // contagion hops from the directly hit target (owner 2026-10-15; may raise later)

static void ef_fireball_set(int fn, int x, int y) {
    int m, co, cn, dam;

    if (fn < 1 || fn >= MAXEFFECT) return;
    if (x < 1 || x >= MAXMAP - 1 || y < 1 || y >= MAXMAP - 1) return;

    m = x + y * MAXMAP;

    co = map[m].ch;
    // A walking character reserves its destination tile (MF_TMOVEBLOCK, which stops
    // projectiles) while map.ch still holds the tile it is leaving. Fireball's 3x3 blast
    // covers that; Plague's center+sides missed approaching enemies. If the burst tile
    // is empty but reserved, hit the character walking into it.
    if (!co && ef[fn].variant && (map[m].flags & MF_TMOVEBLOCK)) {
        int dx, dy, c2;

        for (dy = -1; dy <= 1 && !co; dy++)
            for (dx = -1; dx <= 1 && !co; dx++)
                if ((dx || dy) && (c2 = map[m + dx + dy * MAXMAP].ch) && ch[c2].tox == x && ch[c2].toy == y) co = c2;
    }

    if (co) {

        cn = ef[fn].cn;

        // Keep the blast harmless to dialogue/other non-combat characters.
        // The explosion visits every tile, but only legal attack targets may
        // receive damage.
        if (!can_attack(cn, co)) {
            return;
        }

        if (ch[co].flags & CF_EDEMON) { // earth demons shoot back
            create_fireball(co, ch[co].x, ch[co].y, ch[cn].x, ch[cn].y, ef[fn].strength - 1);
        }
        if (ef[fn].variant) {
            plague_apply_contact(fn, ch[co].x, ch[co].y); // co may be walking into (x,y)
        }

        dam = fireball_damage(cn, co, ef[fn].strength);
        hurt(co, dam, cn, 10, 50, 70);
    }
}

int checkhit_fireball(int x, int y) {
    int m;

    m = x + y * MAXMAP;

    if (map[m].ch) return map[m].ch;

    return 0;
}

static int ef_fireball_reflect(int cn, int str, int fn) {
    int in, n, co;

    if (!(co = ef[fn].cn) || !ch[co].flags || ch[co].serial != ef[fn].sercn) return 0;

    for (n = 0; n < 12; n++) {
        if ((in = ch[cn].item[n]) && it[in].ID == IID_REFLECT_FIREBALL) {
            if (*(unsigned int *)(it[in].drdata) <= str) {
                log_char(cn, LOG_SYSTEM, 0, "Your %s was destroyed.", it[in].name);
                if (ch[cn].flags & CF_PLAYER) dlog(cn, in, "dropped because it was used up");
                remove_item_char(in);
                destroy_item(in);
            }
            *(unsigned int *)(it[in].drdata) -= str;
            sprintf(it[in].description, "%d units left.", *(unsigned int *)(it[in].drdata));
            return 1;
        }
    }
    return 0;
}

/* Each projectile applies pDOT once per character identity, including impact. */
static void plague_apply_contact(int fn, int x, int y) {
    int co, cn = ef[fn].cn;
    struct plague_contact *p;
    if (!ef[fn].variant || !plague_contacts || x < 1 || y < 1 || x >= MAXMAP - 1 || y >= MAXMAP - 1) return;
    co = map[x + y * MAXMAP].ch;
    if (!co || !cn || !ch[cn].flags || ch[cn].serial != ef[fn].sercn || !can_attack(cn, co)) return;
    if (!los_can_see(0, ef[fn].lx, ef[fn].ly, x, y, 5)) return;
    for (p = plague_contacts[fn]; p; p = p->next) if (p->cn == co && p->serial == ch[co].serial) return;
    p = xmalloc(sizeof(*p), IM_BASE);
    if (!p) return;
    if (!create_plague_dot(cn, co, ticker + plague_duration(cn), ef[fn].strength, 1, 19, ef[fn].spell_color)) { xfree(p); return; }
    p->cn = co; p->serial = ch[co].serial; p->next = plague_contacts[fn]; plague_contacts[fn] = p;
}

/* Circular overlap: projectile radius .75 + target radius .5 = 1.25 tiles. */
static void plague_edge_contacts(int fn) {
    int x, y, cx = ef[fn].x / 1024, cy = ef[fn].y / 1024;
    for (y = cy - 1; y <= cy + 1; y++) for (x = cx - 1; x <= cx + 1; x++) {
        long long dx = (long long)x * 1024 + 512 - ef[fn].x;
        long long dy = (long long)y * 1024 + 512 - ef[fn].y;
        if (x == cx && y == cy) continue;
        if (dx * dx + dy * dy <= 1280LL * 1280) plague_apply_contact(fn, x, y);
    }
}

static void plague_impact_sides(int fn, int x, int y) {
    int dx = ef[fn].tox - ef[fn].frx, dy = ef[fn].toy - ef[fn].fry;
    int sx = (dx > 0) - (dx < 0), sy = (dy > 0) - (dy < 0);
    if (!sx && !sy) return;
    if (abs(dx) > 2 * abs(dy)) sy = 0;
    else if (abs(dy) > 2 * abs(dx)) sx = 0;
    // Owner 2026-10-05: the two side tiles follow Fireball mechanics - full impact
    // burst plus pDOT, same as the center - but only these two, none behind/beyond.
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x - sy, y + sx, 5)) ef_fireball_set(fn, x - sy, y + sx);
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x + sy, y - sx, 5)) ef_fireball_set(fn, x + sy, y - sx);
}

static void ef_fireball_explode(int fn, int x, int y) {
    int cn, co, fn2;

    // would we hit someone we're not allowed to hit?
    cn = ef[fn].cn;

    remove_effect(fn);

    if ((co = checkhit_fireball(x, y))) {
        if (can_attack(cn, co) && ef_fireball_reflect(co, ef[fn].strength, fn)) {
            create_fireball(co, ch[co].x, ch[co].y, ch[cn].x, ch[cn].y, ef[fn].strength - 1);
            free_effect(fn);
            return;
        }
    }
    if (ef[fn].variant) {
        plague_impact_sides(fn, x, y);
        ef_fireball_set(fn, x, y);
        goto blast_visual;
    }
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x + 1, y, 5) && (co = checkhit_fireball(x + 1, y))) {
        if (can_attack(cn, co) && ef_fireball_reflect(co, ef[fn].strength, fn)) {
            create_fireball(co, ch[co].x, ch[co].y, ch[cn].x, ch[cn].y, ef[fn].strength - 1);
            free_effect(fn);
            return;
        }
    }
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x - 1, y, 5) && (co = checkhit_fireball(x - 1, y))) {
        if (can_attack(cn, co) && ef_fireball_reflect(co, ef[fn].strength, fn)) {
            create_fireball(co, ch[co].x, ch[co].y, ch[cn].x, ch[cn].y, ef[fn].strength - 1);
            free_effect(fn);
            return;
        }
    }
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x, y + 1, 5) && (co = checkhit_fireball(x, y + 1))) {
        if (can_attack(cn, co) && ef_fireball_reflect(co, ef[fn].strength, fn)) {
            create_fireball(co, ch[co].x, ch[co].y, ch[cn].x, ch[cn].y, ef[fn].strength - 1);
            free_effect(fn);
            return;
        }
    }
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x, y - 1, 5) && (co = checkhit_fireball(x, y - 1))) {
        if (can_attack(cn, co) && ef_fireball_reflect(co, ef[fn].strength, fn)) {
            create_fireball(co, ch[co].x, ch[co].y, ch[cn].x, ch[cn].y, ef[fn].strength - 1);
            free_effect(fn);
            return;
        }
    }

    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x + 1, y + 1, 5) && (co = checkhit_fireball(x + 1, y + 1))) {
        if (can_attack(cn, co) && ef_fireball_reflect(co, ef[fn].strength, fn)) {
            create_fireball(co, ch[co].x, ch[co].y, ch[cn].x, ch[cn].y, ef[fn].strength - 1);
            free_effect(fn);
            return;
        }
    }
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x + 1, y - 1, 5) && (co = checkhit_fireball(x + 1, y - 1))) {
        if (can_attack(cn, co) && ef_fireball_reflect(co, ef[fn].strength, fn)) {
            create_fireball(co, ch[co].x, ch[co].y, ch[cn].x, ch[cn].y, ef[fn].strength - 1);
            free_effect(fn);
            return;
        }
    }
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x - 1, y + 1, 5) && (co = checkhit_fireball(x - 1, y + 1))) {
        if (can_attack(cn, co) && ef_fireball_reflect(co, ef[fn].strength, fn)) {
            create_fireball(co, ch[co].x, ch[co].y, ch[cn].x, ch[cn].y, ef[fn].strength - 1);
            free_effect(fn);
            return;
        }
    }
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x - 1, y - 1, 5) && (co = checkhit_fireball(x - 1, y - 1))) {
        if (can_attack(cn, co) && ef_fireball_reflect(co, ef[fn].strength, fn)) {
            create_fireball(co, ch[co].x, ch[co].y, ch[cn].x, ch[cn].y, ef[fn].strength - 1);
            free_effect(fn);
            return;
        }
    }

    // nope, blast away!
    ef_fireball_set(fn, x, y);
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x + 1, y, 5)) ef_fireball_set(fn, x + 1, y);
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x - 1, y, 5)) ef_fireball_set(fn, x - 1, y);
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x, y + 1, 5)) ef_fireball_set(fn, x, y + 1);
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x, y - 1, 5)) ef_fireball_set(fn, x, y - 1);

    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x + 1, y + 1, 5)) ef_fireball_set(fn, x + 1, y + 1);
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x + 1, y - 1, 5)) ef_fireball_set(fn, x + 1, y - 1);
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x - 1, y + 1, 5)) ef_fireball_set(fn, x - 1, y + 1);
    if (los_can_see(0, ef[fn].lx, ef[fn].ly, x - 1, y - 1, 5)) ef_fireball_set(fn, x - 1, y - 1);

blast_visual:
    {
        /* Preserve coordinates before freeing: the allocator may reuse fn. */
        int blast_x = ef[fn].lx;
        int blast_y = ef[fn].ly;
        unsigned short spell_color = ef[fn].spell_color;
        uint32_t spell_color32 = ef[fn].spell_color32;

        free_effect(fn);
        fn2 = create_explosion_color32(8, 50050, spell_color, spell_color32);
        if (fn2) add_explosion(fn2, blast_x, blast_y);
        sound_area(blast_x, blast_y, 6);
    }
}

int fire_map_block(int m) {
    if (map[m].flags & MF_TMOVEBLOCK) return 1;
    if (map[m].flags & MF_FIRETHRU) return 0;
    if (map[m].flags & MF_MOVEBLOCK) return 1;

    return 0;
}

int ball_map_block(int m) {
    if (map[m].flags & MF_TMOVEBLOCK) return 1;
    if (map[m].flags & MF_FIRETHRU) return 0;
    if (map[m].flags & MF_MOVEBLOCK) return 1;

    return 0;
}

static int fire_char(int m, int cn) {
    if (map[m].ch == cn) return 1;
    if (ch[cn].tox + ch[cn].toy * MAXMAP == m) return 1;

    return 0;
}
static void ef_fireball(int fn) {
    int m, dx, dy, n, x, y, cn, remo = 0;

    cn = ef[fn].cn;
    if (cn && (!ch[cn].flags || ch[cn].serial != ef[fn].sercn)) remo = 1; // different char

    // maximum age for a fireball is TICKS, which makes its range 24 tiles
    if (remo || ticker >= ef[fn].stop) {
        remove_effect(fn);
        free_effect(fn);
        return;
    }

    while (ef[fn].field_cnt) {
        ef[fn].field_cnt--;
        remove_effect_map(fn, ef[fn].m[ef[fn].field_cnt]);
    }

    dx = (ef[fn].tox - ef[fn].frx);
    dy = (ef[fn].toy - ef[fn].fry);

    if (dx == 0 && dy == 0) {
        ef_fireball_explode(fn, ef[fn].x / 1024, ef[fn].y / 1024);
        return;
    }

    // line algorithm with a step of 0.5 tiles
    // this is needed to avoid jumping over obstacles
    if (abs(dx) > abs(dy)) {
        dy = dy * 512 / abs(dx);
        dx = dx * 512 / abs(dx);
    } else {
        dx = dx * 512 / abs(dy);
        dy = dy * 512 / abs(dy);
    }

    // Plague uses one half-tile step per tick; Fireball keeps its normal speed.
    for (n = 0; n < (ef[fn].variant ? 1 : 2); n++) {
        ef[fn].lx = ef[fn].x / 1024;
        ef[fn].ly = ef[fn].y / 1024;

        ef[fn].x += dx;
        ef[fn].y += dy;

        m = (ef[fn].x / 1024) + (ef[fn].y / 1024) * MAXMAP;

        // Plague flies through a character that is already dying (CF_DEAD, death animation) so it
        // reaches the live enemy behind; Fireball keeps stopping there (owner bug 2026-10-10)
        if ((fire_map_block(m)) && (ef[fn].cn == 0 || !fire_char(m, ef[fn].cn)) &&
            !(ef[fn].variant && map[m].ch && (ch[map[m].ch].flags & CF_DEAD))) {
            ef_fireball_explode(fn, ef[fn].x / 1024, ef[fn].y / 1024);
            return;
        }
        if (ef[fn].variant) plague_edge_contacts(fn);
    }

    x = ef[fn].x / 1024;
    y = ef[fn].y / 1024;

    set_effect_map(fn, x, y);
}

// helper for NCPs: would we hit using fireball?
int ishit_fireball(int cn, int frx, int fry, int tox, int toy, int (*isenemy)(int, int)) {
    int dx, dy, x, y, co, n, cx, cy, m, enemy = 0;

    x = frx * 1024 + 512;
    y = fry * 1024 + 512;

    dx = (tox - frx);
    dy = (toy - fry);

    if (abs(dx) < 2 && abs(dy) < 2) return 0;

    // line algorithm with a step of 0.5 tiles
    if (abs(dx) > abs(dy)) {
        dy = dy * 512 / abs(dx);
        dx = dx * 512 / abs(dx);
    } else {
        dx = dx * 512 / abs(dy);
        dy = dy * 512 / abs(dy);
    }

    // 48 steps is whole range
    for (n = 0; n < 48; n++) {

        cx = x / 1024;
        cy = y / 1024;

        m = cx + cy * MAXMAP;

        if ((fire_map_block(m)) && map[m].ch != cn) {
            if ((co = checkhit_fireball(cx, cy))) {
                if (isenemy(cn, co)) enemy++;
                else return 0;
            }
            if ((co = checkhit_fireball(cx + 1, cy))) {
                if (isenemy(cn, co)) enemy++;
                else return 0;
            }
            if ((co = checkhit_fireball(cx, cy + 1))) {
                if (isenemy(cn, co)) enemy++;
                else return 0;
            }
            if ((co = checkhit_fireball(cx - 1, cy))) {
                if (isenemy(cn, co)) enemy++;
                else return 0;
            }
            if ((co = checkhit_fireball(cx, cy - 1))) {
                if (isenemy(cn, co)) enemy++;
                else return 0;
            }
            if ((co = checkhit_fireball(cx + 1, cy + 1))) {
                if (isenemy(cn, co)) enemy++;
                else return 0;
            }
            if ((co = checkhit_fireball(cx - 1, cy + 1))) {
                if (isenemy(cn, co)) enemy++;
                else return 0;
            }
            if ((co = checkhit_fireball(cx + 1, cy - 1))) {
                if (isenemy(cn, co)) enemy++;
                else return 0;
            }
            if ((co = checkhit_fireball(cx - 1, cy - 1))) {
                if (isenemy(cn, co)) enemy++;
                else return 0;
            }

            return enemy;
        }

        x += dx;
        y += dy;
    }

    return 0;
}

int create_fireball(int cn, int frx, int fry, int tox, int toy, int str) {
    int n;

    n = alloc_effect(EF_FIREBALL);
    if (!n) return 0;

    // Fireball is the vanilla projectile. Clear recycled-slot state.
    ef[n].base_sprite = 0;
    ef[n].variant = 0;
    ef[n].strength = str;
    ef[n].light = 200;
    ef[n].frx = frx;
    ef[n].fry = fry;
    ef[n].tox = tox;
    ef[n].toy = toy;
    ef[n].cn = cn;
    if (cn) ef[n].sercn = ch[cn].serial;
    ef[n].field_cnt = 0;
    ef[n].stop = ticker + TICKS;
    ef[n].number_of_enemies = 0;
    ef[n].spell_color = 0; // client default
    ef[n].spell_color32 = 0;

    ef[n].x = ef[n].frx * 1024 + 512;
    ef[n].y = ef[n].fry * 1024 + 512;

    return n;
}

int create_plagueball(int cn, int frx, int fry, int tox, int toy, int str) {
    int n = create_fireball(cn, frx, fry, tox, toy, str);
    if (n) {
        ef[n].variant = 1;
        ef[n].stop = ticker + TICKS * 2;
        // Fixed basic green poison color (packed 5:5:5 = 20, 28, 4).
        ef[n].spell_color = (unsigned short)((20 << 10) | (28 << 5) | 4);
        ef[n].spell_color32 = effect_color32_expand(ef[n].spell_color); // 0xA4E620
    }
    return n;
}

// --------------- lightning ball and helpers ----------------------

static void ef_strike(int fn) {
    if (ticker >= ef[fn].stop) {
        remove_effect(fn);
        free_effect(fn);
        return;
    }
}

static void add_strike(int xc, int yc, int x, int y, int str, int cn, int cc, int cnt) {
    int n, dam, cnt_multi, fn;

    for (n = 0; n < 4; n++) {
        if ((fn = ch[cn].ef[n]) &&
            ef[fn].type == EF_STRIKE &&
            ef[fn].x == xc &&
            ef[fn].y == yc &&
            ef[fn].strength == str) break;
    }
    if (n == 4) {
        fn = alloc_effect(EF_STRIKE);
        if (!fn) return;

        ef[fn].strength = str;
        ef[fn].light = 50;

        ef[fn].field_cnt = 0;

        ef[fn].x = xc;
        ef[fn].y = yc;

        add_effect_char(fn, cn);
    }

    ef[fn].stop = ticker + 2;

    if (cnt < 1) cnt = 1;
    if (cnt > 10) cnt = 10;

    switch (cnt) {
    case 1:
        cnt_multi = 100;
        break;
    case 2:
        cnt_multi = 95;
        break;
    case 3:
        cnt_multi = 90;
        break;
    case 4:
        cnt_multi = 85;
        break;
    case 5:
        cnt_multi = 80;
        break;
    case 6:
        cnt_multi = 75;
        break;
    case 7:
        cnt_multi = 70;
        break;
    case 8:
        cnt_multi = 65;
        break;
    case 9:
        cnt_multi = 60;
        break;
    default:
        cnt_multi = 55;
        break;
    }

    if ((ticker & 3) == 0) {
        dam = strike_damage(cc, cn, str) * cnt_multi / (25 * TICKS * 2);
        if (ch[cn].flags & CF_EDEMON) { // earth demons dont suffer as much damage
            dam -= min(dam / 4, (dam * max(0, ch[cn].value[1][V_DEMON] - ch[cc].value[1][V_DEMON])) / 10);
        }
        hurt(cn, dam, cc, TICKS * 2 * 10 / 4, 30, 85);
        //say(cn,"hit by strike, str=%d (%d), cnt=%d, damage=%d (%.2f)",str,immunity_reduction(cc,cn,str),cnt,dam,(double)dam/1000*TICKS*2);
    }
}

static void check_strike_near(int fn, int xc, int yc) {
    int x, y, xs, ys, xe, ye, co, cn, tmp, cnt = 0;

    xs = max(1, xc - 5);
    ys = max(1, yc - 5);
    xe = min(MAXMAP - 2, xc + 5);
    ye = min(MAXMAP - 2, yc + 5);

    for (y = ys; y < ye; y++) {
        for (x = xs; x < xe; x++) {
            if ((co = map[x + y * MAXMAP].ch)) {
                cn = ef[fn].cn;

                if (!can_attack(cn, co)) continue;
                if (!is_visible(cn, co)) continue;

                // not through walls
                if (!(tmp = los_can_see(0, xc, yc, x, y, 5))) continue;

                add_strike(xc, yc, x, y, ef[fn].strength, co, ef[fn].cn, ef[fn].number_of_enemies);
                if (cnt == 0 && (ticker & 7) == 0) {
                    sound_area(ch[cn].x, ch[cn].y, 30);
                }
                cnt++;
            }
        }
    }
    ef[fn].number_of_enemies = cnt;
}

static void ef_ball(int fn) {
    int m, dx, dy, x, y, cn, remo = 0, oldx, oldy;

    cn = ef[fn].cn;
    if (cn && (!ch[cn].flags || ch[cn].serial != ef[fn].sercn)) remo = 1; // different char

    // maximum age for a lightning ball is TICKS*5
    if (remo || ticker >= ef[fn].stop) {
        remove_effect(fn);
        free_effect(fn);
        return;
    }

    oldx = ef[fn].x / 1024;
    oldy = ef[fn].y / 1024;

    dx = (ef[fn].tox - ef[fn].frx);
    dy = (ef[fn].toy - ef[fn].fry);

    if (dx == 0 && dy == 0) {
        remove_effect(fn);
        free_effect(fn);
        return;
    }

    // line algorithm with a step of 0.125 tiles
    if (abs(dx) > abs(dy)) {
        dy = dy * 128 / abs(dx);
        dx = dx * 128 / abs(dx);
    } else {
        dx = dx * 128 / abs(dy);
        dy = dy * 128 / abs(dy);
    }

    ef[fn].x += dx;
    ef[fn].y += dy;

    m = (ef[fn].x / 1024) + (ef[fn].y / 1024) * MAXMAP;

    //(map[m].flags&(MF_TMOVEBLOCK|MF_MOVEBLOCK))
    if (ball_map_block(m) && (ef[fn].cn == 0 || map[m].ch != ef[fn].cn)) {
        remove_effect(fn);
        free_effect(fn);
        return;
    }

    x = ef[fn].x / 1024;
    y = ef[fn].y / 1024;

    if (oldx != x || oldy != y) {
        remove_effect(fn);
        set_effect_map(fn, x, y);
    } else set_sector(x, y);

    check_strike_near(fn, x, y);
}

int calc_steps_ball(int cn, int frx, int fry, int tox, int toy) {
    int m, dx, dy, x, y, n;

    dx = (tox - frx);
    dy = (toy - fry);

    x = frx * 1024 + 512;
    y = fry * 1024 + 512;

    if (dx == 0 && dy == 0) return 0;

    // line algorithm with a step of 0.5 tiles
    if (abs(dx) > abs(dy)) {
        dy = dy * 512 / abs(dx);
        dx = dx * 512 / abs(dx);
    } else {
        dx = dx * 512 / abs(dy);
        dy = dy * 512 / abs(dy);
    }

    for (n = 0; n < TICKS * 5 / 4; n++) {
        x += dx;
        y += dy;

        m = (x / 1024) + (y / 1024) * MAXMAP;

        //(map[m].flags&(MF_TMOVEBLOCK|MF_MOVEBLOCK))
        if (ball_map_block(m) && map[m].ch != cn) {
            return n;
        }
    }

    return n;
}

int create_ball(int cn, int frx, int fry, int tox, int toy, int str) {
    int n;

    n = alloc_effect(EF_BALL);
    if (!n) return 0;

    ef[n].strength = str;
    ef[n].light = 80;
    ef[n].frx = frx;
    ef[n].fry = fry;
    ef[n].tox = tox;
    ef[n].toy = toy;
    ef[n].cn = cn;
    if (cn) ef[n].sercn = ch[cn].serial;
    ef[n].field_cnt = 0;
    ef[n].stop = ticker + TICKS * 5;
    ef[n].number_of_enemies = 0;

    ef[n].x = ef[n].frx * 1024 + 512;
    ef[n].y = ef[n].fry * 1024 + 512;

    return n;
}

//---------------- lightning flash ---------------------------------

static void ef_flash(int fn) {
    int cn;

    cn = ef[fn].cn;
    if (ch[cn].serial != ef[fn].sercn) cn = 0; // different char
    else if (ch[cn].flags & (CF_DEAD)) cn = 0; // dead or unconscious char

    // maximum age for a flash is TICKS*2
    if (!cn || ticker >= ef[fn].stop) {
        remove_effect(fn);
        free_effect(fn);
        return;
    }

    /*while (ef[fn].field_cnt) {
    ef[fn].field_cnt--;
    remove_effect_map(fn,ef[fn].m[ef[fn].field_cnt]);
}

set_effect_map(fn,ch[cn].x,ch[cn].y);*/
    check_strike_near(fn, ch[cn].x, ch[cn].y);
    //set_sector(ch[cn].x,ch[cn].y);
}

int create_flash(int cn, int str, int duration) {
    int fn;

    fn = alloc_effect(EF_FLASH);
    if (!fn) return 0;

    ef[fn].strength = str;
    ef[fn].light = 50;
    ef[fn].cn = cn;
    ef[fn].sercn = ch[cn].serial;
    ef[fn].field_cnt = 0;
    ef[fn].start = ticker;
    ef[fn].stop = ticker + duration;
    ef[fn].number_of_enemies = 0;

    add_effect_char(fn, cn);

    return fn;
}

//---------------- explosion ---------------------------------

static void ef_explode(int fn) {
    if (ticker >= ef[fn].stop) {
        remove_effect(fn);
        free_effect(fn);
        return;
    }
}

int add_explosion(int fn, int x, int y) {
    return set_effect_map(fn, x, y);
}

int create_explosion(int maxage, int base) {
    int n;

    n = alloc_effect(EF_EXPLODE);
    if (!n) return 0;

    ef[n].strength = maxage;
    ef[n].light = 200;
    ef[n].cn = 0;
    ef[n].sercn = 0;
    ef[n].field_cnt = 0;
    ef[n].stop = ticker + maxage;
    ef[n].number_of_enemies = 0;
    ef[n].base_sprite = base;

    return n;
}

int create_explosion_color(int maxage, int base, unsigned short color) {
    int fn = create_explosion(maxage, base);
    if (fn) ef[fn].spell_color = color;
    return fn;
}

int create_explosion_color32(int maxage, int base, unsigned short color, uint32_t color32) {
    int fn = create_explosion_color(maxage, base, color);
    if (fn) ef[fn].spell_color32 = color32 ? color32 : effect_color32_expand(color);
    return fn;
}

int create_mist(int x, int y) {
    int n;

    n = alloc_effect(EF_MIST);
    if (!n) return 0;

    ef[n].light = 0;
    ef[n].cn = 0;
    ef[n].sercn = 0;
    ef[n].field_cnt = 0;
    ef[n].stop = ticker + 24;
    ef[n].number_of_enemies = 0;

    return set_effect_map(n, x, y);
}

static void ef_show(int fn) {
    if (ticker >= ef[fn].stop) {
        remove_effect(fn);
        free_effect(fn);
        return;
    }
}

static void ef_earthrain(int fn) {
    int n, cn, m, dam, per;

    for (n = 0; n < ef[fn].field_cnt; n++) {
        m = ef[fn].m[n];
        if (!(cn = map[m].ch)) continue;
        //if (!can_attack(ef[fn].cn,cn)) continue;
        if (!(ch[cn].flags & CF_PLAYER)) continue;

        dam = edemon_reduction(cn, ef[fn].strength) * 150;
        if (!dam) continue;

        per = 50 - min(50, edemon_reduction(cn, ef[fn].strength));

        if (!RANDOM(10)) hurt(cn, dam, 0, 8, per, per + 25); //hurt(cn,dam,ef[fn].cn,8,per,per+25);
    }
    if (ticker >= ef[fn].stop) {
        remove_effect(fn);
        free_effect(fn);
        return;
    }
}

static int add_earthrain_map(int fn, int x, int y) {
    int n, fn2, m;

    m = x + y * MAXMAP;

    for (n = 0; n < 4; n++) {
        if ((fn2 = map[m].ef[n]) && ef[fn2].type == EF_EARTHRAIN) return 0;
    }
    return set_effect_map(fn, x, y);
}

int create_earthrain(int cn, int x, int y, int strength) {
    int n;

    n = alloc_effect(EF_EARTHRAIN);
    if (!n) return 0;

    ef[n].light = 10;
    ef[n].cn = 0; //ef[n].cn=cn;
    ef[n].sercn = 0;
    ef[n].field_cnt = 0;
    ef[n].stop = ticker + TICKS * 60;
    ef[n].number_of_enemies = 0;
    ef[n].strength = strength;

    set_effect_map(n, x, y);

    if (!(map[x + y * MAXMAP + 1].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthrain_map(n, x + 1, y);
    if (!(map[x + y * MAXMAP - 1].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthrain_map(n, x - 1, y);
    if (!(map[x + y * MAXMAP + MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthrain_map(n, x, y + 1);
    if (!(map[x + y * MAXMAP - MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthrain_map(n, x, y - 1);

    if (!(map[x + y * MAXMAP + 1 + MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthrain_map(n, x + 1, y + 1);
    if (!(map[x + y * MAXMAP - 1 + MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthrain_map(n, x - 1, y + 1);
    if (!(map[x + y * MAXMAP + 1 - MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthrain_map(n, x + 1, y - 1);
    if (!(map[x + y * MAXMAP - 1 - MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthrain_map(n, x - 1, y - 1);

    return 1;
}

static void ef_earthmud(int fn) {
    if (ticker >= ef[fn].stop) {
        remove_effect(fn);
        free_effect(fn);
        return;
    }
}

static int add_earthmud_map(int fn, int x, int y) {
    int n, fn2, m;

    m = x + y * MAXMAP;

    for (n = 0; n < 4; n++) {
        if ((fn2 = map[m].ef[n]) && ef[fn2].type == EF_EARTHMUD) return 0;
    }
    return set_effect_map(fn, x, y);
}

int create_earthmud(int cn, int x, int y, int strength) {
    int n;

    n = alloc_effect(EF_EARTHMUD);
    if (!n) return 0;

    ef[n].light = 0;
    ef[n].cn = cn;
    ef[n].sercn = 0;
    ef[n].field_cnt = 0;
    ef[n].stop = ticker + TICKS * 60;
    ef[n].number_of_enemies = 0;
    ef[n].strength = strength;

    set_effect_map(n, x, y);

    if (!(map[x + y * MAXMAP + 1].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthmud_map(n, x + 1, y);
    if (!(map[x + y * MAXMAP - 1].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthmud_map(n, x - 1, y);
    if (!(map[x + y * MAXMAP + MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthmud_map(n, x, y + 1);
    if (!(map[x + y * MAXMAP - MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthmud_map(n, x, y - 1);

    if (!(map[x + y * MAXMAP + 1 + MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthmud_map(n, x + 1, y + 1);
    if (!(map[x + y * MAXMAP - 1 + MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthmud_map(n, x - 1, y + 1);
    if (!(map[x + y * MAXMAP + 1 - MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthmud_map(n, x + 1, y - 1);
    if (!(map[x + y * MAXMAP - 1 - MAXMAP].flags & (MF_SIGHTBLOCK | MF_TSIGHTBLOCK))) add_earthmud_map(n, x - 1, y - 1);

    return 1;
}

static int fireball_burn_path_clear(int x, int y, int tx, int ty) {
    int steps, n, px, py, m;

    steps = abs(tx - x) > abs(ty - y) ? abs(tx - x) : abs(ty - y);
    if (!steps) return 0;

    // The destination is occupied by the candidate character, so its own
    // movement flag may describe the creature's spawn tile. Only intervening
    // tiles must be traversable; the destination remains subject to LOS and
    // combat checks below.
    for (n = 1; n < steps; n++) {
        px = x + (tx - x) * n / steps;
        py = y + (ty - y) * n / steps;
        m = px + py * MAXMAP;
        if (map[m].flags & (MF_MOVEBLOCK | MF_TMOVEBLOCK | MF_SIGHTBLOCK | MF_TSIGHTBLOCK)) return 0;
    }

    return los_can_see(0, x, y, tx, ty, 5);
}

static void ef_burn(int fn) {
    int cn, co, x, y, dx, dy, n, already_burning;

    cn = ef[fn].cn;
    if (!cn || !(ch[cn].flags) || ch[cn].serial != ef[fn].sercn || ticker >= ef[fn].stop) {
        if (cn > 0 && cn < MAXCHARS && (ef[fn].base_sprite >= 1 && ef[fn].base_sprite <= 4)) {
            fireball_burn_cooldown[cn] = ticker + 3;
        }
        remove_effect(fn);
        free_effect(fn);
        return;
    }

    // Plague pDOT uses the Lightning Strike damage formula, but remains a
    // separate burn branch and credits its current owner (a stronger ball takes over).
    // It ticks for its whole lifetime (start..stop), so a visible infection always damages.
    // a caster who left and whose slot was reused is not the caster any more: the
    // infection then neither damages nor spreads in that stranger's name
    if (ef[fn].burn_kind == BURN_KIND_PLAGUE && ef[fn].source_cn && !dps_effect_source_alive(fn))
        ef[fn].source_cn = 0;

    if (ef[fn].burn_kind == BURN_KIND_PLAGUE && (ticker & 3) == 0) {
        int attacker = ef[fn].source_cn;
        int dam = 0;
        if (attacker >= 1 && attacker < MAXCHARS && ch[attacker].flags) {
            dam = strike_damage(attacker, cn, ef[fn].strength) * 100 / (25 * TICKS * 2);
            dps_hurt_effect(fn, cn, dam, attacker, TICKS * 2 * 10 / 4, 30, 85);
        }
    } else if (ef[fn].burn_kind != BURN_KIND_PLAGUE && ef[fn].strength) {
        int attacker = ef[fn].source_cn;
        if (attacker < 1 || attacker >= MAXCHARS || !ch[attacker].flags) attacker = 0;
        dps_hurt_effect(fn, cn, POWERSCALE / 6 + ef[fn].strength, attacker, 30, 50, 75);
    }

    // The burn marker can jump up to three times to an
    // attackable target within two tiles. The internal marker tracks depth,
    // distinguishes this experiment from ordinary burns, and prevents loops.
    if (ef[fn].base_sprite >= 1 &&
        ef[fn].base_sprite <= (ef[fn].burn_kind == BURN_KIND_PLAGUE ? PLAGUE_MAX_JUMPS : 3)) {
        x = ch[cn].x;
        y = ch[cn].y;

        for (dy = -3; dy <= 3; dy++) {
            for (dx = -3; dx <= 3; dx++) {
                if (!dx && !dy) continue;

                co = 0;
                if (x + dx >= 1 && x + dx < MAXMAP - 1 && y + dy >= 1 && y + dy < MAXMAP - 1) {
                    co = map[x + dx + (y + dy) * MAXMAP].ch;
                }
                if (!co || co == ef[fn].source_cn || ticker < fireball_burn_cooldown[co] ||
                    (ch[co].flags & (CF_DEAD | CF_NOATTACK)) ||
                    !ef[fn].source_cn || !ch[ef[fn].source_cn].flags ||
                    !can_attack(ef[fn].source_cn, co) ||
                    !fireball_burn_path_clear(x, y, x + dx, y + dy)) continue;

                already_burning = 0;
                for (n = 0; n < 4; n++) {
                    // Fire and Plague are independent: only a burn of the same kind counts.
                    if (ch[co].ef[n] && ef[ch[co].ef[n]].type == EF_BURN &&
                        ef[ch[co].ef[n]].burn_kind == ef[fn].burn_kind) {
                        // Plague contagion never refreshes or extends an existing infection.
                        if (ef[fn].burn_kind != BURN_KIND_PLAGUE && ef[ch[co].ef[n]].base_sprite >= 1) {
                            ef[ch[co].ef[n]].burn_hits = 2;
                            ef[ch[co].ef[n]].source_cn = ef[fn].source_cn;
                            dps_copy_effect_owner(ch[co].ef[n], fn);
                            ef[ch[co].ef[n]].stop = ef[fn].stop;
                            update_char(co);
                        }
                        already_burning = 1;
                        break;
                    }
                }
                if (already_burning) continue;

                // Spread keeps the parent's remaining duration (stop). Plague spreads at
                // full strength and ticks normally; Fire spread stays cosmetic.
                if (ef[fn].burn_kind == BURN_KIND_PLAGUE) {
                    int added = create_plague_dot(ef[fn].source_cn, co, ef[fn].stop, ef[fn].strength,
                        ef[fn].base_sprite + 1, 19, ef[fn].spell_color);
                    if (added) dps_copy_effect_owner(added, fn);
                } else {
                    int added = create_fireball_burn(ef[fn].source_cn, co, ef[fn].stop, ef[fn].base_sprite + 1, 19);
                    if (added) dps_copy_effect_owner(added, fn);
                }
            }
        }
    }
}

int create_show_effect(int type, int cn, int start, int stop, int light, int strength) {
    int fn;

    fn = alloc_effect(type);
    if (!fn) return 0;

    ef[fn].light = light;
    ef[fn].cn = cn;
    ef[fn].efcn = cn;
    ef[fn].sercn = ch[cn].serial;
    ef[fn].field_cnt = 0;
    ef[fn].start = start;
    ef[fn].stop = stop;
    ef[fn].strength = strength;
    ef[fn].base_sprite = 0;
    ef[fn].burn_kind = 0;
    ef[fn].source_cn = 0;
    ef[fn].burn_hits = 0;
    ef[fn].spell_color = 0; // client default
    ef[fn].spell_color32 = 0;

    add_effect_char(fn, cn);

    return fn;
}

int create_fireball_burn(int source_cn, int target, int stop, int depth, int light) {
    int n, fn;

    if (target < 1 || target >= MAXCHARS || !(ch[target].flags)) return 0;

    for (n = 0; n < 4; n++) {
        fn = ch[target].ef[n];
        // Only an existing Fire burn is refreshed; a Plague infection is left alone.
        if (fn && ef[fn].type == EF_BURN && ef[fn].burn_kind == BURN_KIND_FIRE && ef[fn].base_sprite >= 1) {
            ef[fn].burn_hits = 2;
            ef[fn].source_cn = source_cn;
            dps_bind_effect(fn, source_cn);
            ef[fn].stop = stop;
            update_char(target);
            return fn;
        }
    }

    fn = create_show_effect(EF_BURN, target, ticker, stop, light, 0);
    if (!fn) return 0;

    ef[fn].base_sprite = depth;
    ef[fn].burn_kind = BURN_KIND_FIRE;
    ef[fn].source_cn = source_cn;
    dps_bind_effect(fn, source_cn);
    ef[fn].burn_hits = 1;
    update_char(target);
    return fn;
}

/* Plague infection lasts as long as the caster's Lightning Flash (act.c act_flash):
   2s plus the Duration bonus. Mirrors act.c's local DURATION divisor (35). */
#define PLAGUE_FLASH_DURATION_DIV 35
int plague_duration(int cn) {
    int duration = FLASHDURATION;
    if (cn < 1 || cn >= MAXCHARS || !ch[cn].flags) return duration;
    if (ch[cn].value[1][V_DURATION]) duration += FLASHDURATION * ch[cn].value[0][V_DURATION] / PLAGUE_FLASH_DURATION_DIV;
    else if (ch[cn].flags & CF_ARCH) duration += FLASHDURATION * ch[cn].level / PLAGUE_FLASH_DURATION_DIV / 2;
    return duration;
}

static int plague_dot_on(int target) {
    int n, fn;
    for (n = 0; n < 4; n++)
        if ((fn = ch[target].ef[n]) && ef[fn].type == EF_BURN && ef[fn].burn_kind == BURN_KIND_PLAGUE) return fn;
    return 0;
}

/* Plague pDOT is its own EF_BURN (kind PLAGUE), independent of a Fire burn on the
   same target. It damages for exactly as long as it exists (visible == ticking).
   Direct contact (depth 1) against an existing infection follows the owner's
   residual rules: equal or weaker leaves it untouched (no timer reset), stronger
   replaces strength/owner/color and renews. Contagion (depth > 1) never
   touches an existing infection; it inherits the parent's stop unchanged. */
int create_plague_dot(int source_cn, int target, int stop, int strength, int depth, int light,
    unsigned short color) {
    int fn;

    if (target < 1 || target >= MAXCHARS || !(ch[target].flags)) return 0;

    if ((fn = plague_dot_on(target))) {
        // contagion, equal or weaker: unchanged (owner 2026-10-15: only a stronger ball resets)
        if (depth > 1 || strength <= ef[fn].strength) return fn;
        ef[fn].strength = strength;
        ef[fn].source_cn = source_cn;
        dps_bind_effect(fn, source_cn);
        ef[fn].spell_color = color;
        ef[fn].spell_color32 = effect_color32_expand(color);
        // stronger: takes over and renews the damage window; a fresh direct hit restarts the lineage
        ef[fn].burn_hits = 2;
        ef[fn].start = ticker;
        ef[fn].stop = stop;
        ef[fn].base_sprite = 1;
        update_char(target);
        return fn;
    }

    fn = create_show_effect(EF_BURN, target, ticker, stop, light, strength);
    if (!fn) return 0;

    ef[fn].base_sprite = depth;
    ef[fn].burn_kind = BURN_KIND_PLAGUE;
    ef[fn].source_cn = source_cn;
    dps_bind_effect(fn, source_cn);
    ef[fn].burn_hits = 1;
    ef[fn].spell_color = color;
    ef[fn].spell_color32 = effect_color32_expand(color);
    update_char(target);
    return fn;
}

int create_map_effect(int type, int x, int y, int start, int stop, int light, int strength) {
    int fn;

    fn = alloc_effect(type);
    if (!fn) return 0;

    ef[fn].light = light;
    ef[fn].field_cnt = 0;
    ef[fn].start = start;
    ef[fn].stop = stop;
    ef[fn].strength = strength;

    set_effect_map(fn, x, y);

    return fn;
}

// ------------- end of magic shield -----------------

void destroy_chareffects(int cn) {
    int n, fn;

    for (n = 0; n < 4; n++) {
        if ((fn = ch[cn].ef[n])) {
            remove_effect(fn);
            free_effect(fn);
        }
    }
}

// ------ edemon ball --------------

int edemonball_map_block(int m) {
    if (map[m].ch || (map[m].it && (map[m].flags & MF_TMOVEBLOCK))) return 1;
    if (map[m].flags & MF_FIRETHRU) return 0;
    if (map[m].flags & MF_MOVEBLOCK) return 1;

    return 0;
}

static void ef_edemonball_explode(int fn, int x, int y) {
    int m, co, cn, dam, n, in, sprite, base;

    if (fn < 1 || fn >= MAXEFFECT) return;
    if (x < 1 || x >= MAXMAP - 1 || y < 1 || y >= MAXMAP - 1) return;

    // would we hit someone we're not allowed to hit?
    cn = ef[fn].cn;
    base = ef[fn].base_sprite;

    remove_effect(fn);
    free_effect(fn);

    m = x + y * MAXMAP;

    if ((co = map[m].ch) && can_attack(cn, co) && !(base == 2 && (ch[co].flags & (CF_PLAYER | CF_PLAYERLIKE)))) {

        cn = ef[fn].cn;
        dam = ef[fn].strength;

        for (n = 29; n < INVENTORYSIZE; n++) {

            if (n > 29) in = ch[co].item[n];
            else in = ch[co].citem;

            if (in && base == 0 && it[in].ID == IID_AREA6_GREENCRYSTAL) {

                if (dam > it[in].drdata[0]) {

                    dam -= it[in].drdata[0];

                    if (n > 29) ch[co].item[n] = 0;
                    else ch[co].citem = 0;

                    ch[co].flags |= CF_ITEMS;

                    if (ch[cn].flags & CF_PLAYER) dlog(cn, in, "dropped as shield against edemonball");
                    log_char(co, LOG_SYSTEM, 0, "Your %s was destroyed.", it[in].name);

                    destroy_item(in);
                    continue;
                }

                it[in].drdata[0] -= dam;
                dam = 0;

                sprite = 50318 + 5 - (it[in].drdata[0] / 42);

                if (sprite != it[in].sprite) {
                    it[in].sprite = sprite;
                    ch[co].flags |= CF_ITEMS;
                }
                break;
            }
        }

        hurt(co, dam * POWERSCALE, 0, 6, 75, 50);
    }

    fn = create_explosion(8, 50450 + base);
    add_explosion(fn, x, y);
    sound_area(x, y, 6);
}

static void ef_edemonball(int fn) {
    int m, dx, dy, x, y, cn, remo = 0;

    cn = ef[fn].cn;
    if (cn && ch[cn].serial != ef[fn].sercn) remo = 1; // different char

    // maximum age for a fireball is TICKS, which makes its range 24 tiles
    if (remo || ticker >= ef[fn].stop) {
        remove_effect(fn);
        free_effect(fn);
        return;
    }

    while (ef[fn].field_cnt) {
        ef[fn].field_cnt--;
        remove_effect_map(fn, ef[fn].m[ef[fn].field_cnt]);
    }

    dx = (ef[fn].tox - ef[fn].frx);
    dy = (ef[fn].toy - ef[fn].fry);

    if (dx == 0 && dy == 0) {
        ef_edemonball_explode(fn, ef[fn].x / 1024, ef[fn].y / 1024);
        return;
    }

    // line algorithm with a step of 0.5 tiles
    // this is needed to avoid jumping over obstacles
    if (abs(dx) > abs(dy)) {
        dy = dy * 256 / abs(dx);
        dx = dx * 256 / abs(dx);
    } else {
        dx = dx * 256 / abs(dy);
        dy = dy * 256 / abs(dy);
    }

    ef[fn].lx = ef[fn].x / 1024;
    ef[fn].ly = ef[fn].y / 1024;

    x = ef[fn].x + dx;
    y = ef[fn].y + dy;

    m = (x / 1024) + (y / 1024) * MAXMAP;

    if ((edemonball_map_block(m)) && (ef[fn].cn == 0 || map[m].ch != ef[fn].cn)) {
        if (map[m].ch) ef_edemonball_explode(fn, x / 1024, y / 1024);
        else ef_edemonball_explode(fn, ef[fn].x / 1024, ef[fn].y / 1024);
        return;
    }

    ef[fn].x = x;
    ef[fn].y = y;

    x = ef[fn].x / 1024;
    y = ef[fn].y / 1024;

    set_effect_map(fn, x, y);
}

int create_edemonball(int cn, int frx, int fry, int tox, int toy, int str, int base) {
    int n;

    n = alloc_effect(EF_EDEMONBALL);
    if (!n) return 0;

    ef[n].strength = str;
    ef[n].light = 0;
    ef[n].frx = frx;
    ef[n].fry = fry;
    ef[n].tox = tox;
    ef[n].toy = toy;
    ef[n].cn = cn;
    if (cn) ef[n].sercn = ch[cn].serial;
    ef[n].field_cnt = 0;
    ef[n].stop = ticker + TICKS * 4;
    ef[n].number_of_enemies = 0;
    ef[n].base_sprite = base;

    ef[n].x = ef[n].frx * 1024 + 512;
    ef[n].y = ef[n].fry * 1024 + 512;

    return n;
}

void create_pulse(int x, int y, int str) {
    int n;

    n = alloc_effect(EF_PULSE);
    if (!n) return;

    ef[n].strength = str;
    ef[n].field_cnt = 0;
    ef[n].stop = ticker + 6;

    set_effect_map(n, x, y);

    return;
}

void tick_effect(void) {
    int n, next;
    /*static int cnt=0;

    if (++cnt<TICKS) return;
    cnt=0;*/

    for (n = getfirst_effect(); n; n = next) {

        next = getnext_effect(n);

        switch (ef[n].type) {
        case EF_NONE:
            break;
        case EF_FIREBALL:
            ef_fireball(n);
            break;
        case EF_MAGICSHIELD:
            ef_show(n);
            break;
        case EF_BALL:
            ef_ball(n);
            break;
        case EF_STRIKE:
            ef_strike(n);
            break;
        case EF_FLASH:
            ef_flash(n);
            break;
        case EF_EXPLODE:
            ef_explode(n);
            break;
        case EF_WARCRY:
            ef_show(n);
            break;
        case EF_BLESS:
            ef_show(n);
            break;
        case EF_FREEZE:
            ef_show(n);
            break;
        case EF_HEAL:
            ef_show(n);
            break;
        case EF_BURN:
            ef_burn(n);
            break;
        case EF_MIST:
            ef_show(n);
            break;
        case EF_POTION:
            ef_show(n);
            break;
        case EF_EARTHRAIN:
            ef_earthrain(n);
            break;
        case EF_EARTHMUD:
            ef_earthmud(n);
            break;
        case EF_EDEMONBALL:
            ef_edemonball(n);
            break;
        case EF_CURSE:
            ef_show(n);
            break;
        case EF_CAP:
            ef_show(n);
            break;
        case EF_LAG:
            ef_show(n);
            break;
        case EF_PULSE:
            ef_show(n);
            break;
        case EF_PULSEBACK:
            ef_show(n);
            break;
        case EF_FIRERING:
            ef_show(n);
            break;
        case EF_BUBBLE:
            ef_show(n);
            break;

        default:
            elog("unknown effect type %d for ef %d", ef[n].type, n);
        }
    }
}

int init_effect(void) {
    int n;

    plague_contacts = xcalloc(sizeof(*plague_contacts) * MAXEFFECT, IM_BASE);
    if (!plague_contacts) return 0;
    ef = xcalloc(sizeof(struct effect) * MAXEFFECT, IM_BASE);
    if (!ef) return 0;
    xlog("Allocated effects: %.2fM (%zu*%d)", sizeof(struct effect) * MAXEFFECT / 1024.0 / 1024.0, sizeof(struct effect), MAXEFFECT);
    mem_usage += sizeof(struct effect) * MAXEFFECT;

    for (n = 1; n < MAXEFFECT; n++) {
        ef[n].type = 42;
        free_effect(n);
    }

    used_effects = 0;

    return 1;
}
