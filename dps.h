#ifndef UNCHARTED_DPS_H
#define UNCHARTED_DPS_H
#include <stdint.h>
#define DPS_WINDOW_TICKS (5 * TICKS)
#define DPS_PACKET_SIZE 13
#define DPS_CLIENT_VERSION 8
int dps_damage_source_serial(int cn);
void dps_record_damage(int cn, int serial, int target, int hp_damage, int shield_damage);
void dps_reset(int cn);
int dps_build_packet(int cn, int version, unsigned char *buf);
void dps_bind_effect(int fn, int source);
void dps_forget_effect(int fn);
void dps_copy_effect_owner(int to, int from);
int dps_effect_source_alive(int fn);
int dps_hurt_effect(int fn, int target, int damage, int source, int armordiv, int armorper, int shieldper);
#endif
