/*
 * Part of Astonia Server (c) Daniel Brockhaus. Please read license.txt.
 */

#define __USE_BSD_SIGNAL
#define _GNU_SOURCE // dladdr

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <dlfcn.h>

#include "server.h"
#include "tool.h"
#include "log.h"
#include "notify.h"
#include "player.h"
#include "mem.h"
#include "libload.h"
#include "drvlib.h"
#include "talk.h"

#define MAXLIB 100
#define MAXDRV 200

struct lib {
    int used;
    char name[80];
    time_t time;

    void *handle;
    int (*driver)(int type, int nr, int obj, intptr_t ret, int last_action);
};

static struct lib *libs = NULL;

static int *fast_chdrv;
static int *fast_itdrv;

static int abi_failed; // set when any driver fails the ABI check; init_lib then aborts startup

// Returns 1 if the driver's own au_abi_info matches the server's exactly.
// A missing record (driver predates the check), one resolved from another
// object, or any differing size/offset is rejected and logged in full.
static int abi_verify(struct lib *lib) {
    Dl_info a, d;
    const struct au_abi *abi = dlsym(lib->handle, "au_abi_info");
    const char *why = NULL;

    if (!abi) why = "no au_abi_info (driver built before the ABI check)";
    else if (!dladdr((void *)abi, &a) || !dladdr((void *)lib->driver, &d) || a.dli_fbase != d.dli_fbase)
        why = "au_abi_info not provided by the driver itself";
    else if (memcmp(abi, &au_abi_info, sizeof(au_abi_info))) why = "shared struct layout differs from server";
    if (!why) return 1;

    elog("FATAL ABI MISMATCH in %s: %s. Rebuild server and ALL drivers from one source snapshot.", lib->name, why);
    elog("  server: ver %u ptr %u map %u item %u char %u effect %u (field_cnt@%u m@%u source_cn@%u burn_hits@%u burn_kind@%u) maxfield %u",
        au_abi_info.version, au_abi_info.ptr_size, au_abi_info.sz_map, au_abi_info.sz_item, au_abi_info.sz_character,
        au_abi_info.sz_effect, au_abi_info.off_effect_field_cnt, au_abi_info.off_effect_m, au_abi_info.off_effect_source_cn,
        au_abi_info.off_effect_burn_hits, au_abi_info.off_effect_burn_kind, au_abi_info.maxfield);
    if (abi && abi->magic == AU_ABI_MAGIC)
        elog("  driver: ver %u ptr %u map %u item %u char %u effect %u (field_cnt@%u m@%u source_cn@%u burn_hits@%u burn_kind@%u) maxfield %u",
            abi->version, abi->ptr_size, abi->sz_map, abi->sz_item, abi->sz_character, abi->sz_effect,
            abi->off_effect_field_cnt, abi->off_effect_m, abi->off_effect_source_cn, abi->off_effect_burn_hits,
            abi->off_effect_burn_kind, abi->maxfield);
    fprintf(stderr, "FATAL ABI MISMATCH in %s: %s\n", lib->name, why);
    abi_failed = 1;
    return 0;
}

static int load_lib(struct lib *lib) {
    struct stat st;

    if (stat(lib->name, &st)) {
        elog("Error loading library %s, not present.", lib->name);
        if (lib->handle) {
            dlclose(lib->handle);
            //bzero(lib,sizeof(*lib));
            lib->handle = NULL;
            lib->time = 0;
            lib->driver = NULL;
        }
        return 0;
    }
    if (st.st_mtime > lib->time) {
        if (lib->handle) {
            dlclose(lib->handle);
        }

        lib->handle = dlopen(lib->name, RTLD_NOW);
        if (!lib->handle) {
            elog("Error loading library %s.", lib->name);
            //bzero(lib,sizeof(*lib));
            lib->handle = NULL;
            lib->time = 0;
            lib->driver = NULL;
            return 0;
        }

        lib->driver = dlsym(lib->handle, "driver");
        if (!lib->driver) {
            dlclose(lib->handle);
            elog("Error resolving symbol driver in library %s.", lib->name);
            //bzero(lib,sizeof(*lib));
            lib->handle = NULL;
            lib->time = 0;
            lib->driver = NULL;
            return 0;
        }

        if (!abi_verify(lib)) { // never run a driver compiled against a different shared layout
            dlclose(lib->handle);
            lib->handle = NULL;
            lib->time = 0;
            lib->driver = NULL;
            return 0;
        }

        lib->time = st.st_mtime;
    }
    if (lib->handle) return 1;
    else return 0;
}

static void add_lib(char *name) {
    int n;

    for (n = 1; n < MAXLIB; n++) {
        if (!libs[n].used) {
            libs[n].used = 1;
            strcpy(libs[n].name, name);
            load_lib(libs + n);
            return;
        }
    }
}

int init_lib(void) {
    DIR *dir;
    struct dirent *de;
    char dirname[NAME_MAX + 50], name[NAME_MAX * 2 + 200];

    libs = xcalloc(sizeof(struct lib) * MAXLIB, IM_BASE);
    if (!libs) return 0;
    xlog("Allocated libs: %.2fM (%zu*%d)", sizeof(struct lib) * MAXLIB / 1024.0 / 1024.0, sizeof(struct lib), MAXLIB);
    mem_usage += sizeof(struct lib) * MAXLIB;

    fast_chdrv = xcalloc(sizeof(int) * MAXDRV, IM_BASE);
    if (!fast_chdrv) return 0;
    xlog("Allocated fast_chdrvs: %.2fM (%zu*%d)", sizeof(int) * MAXDRV / 1024.0 / 1024.0, sizeof(int), MAXDRV);
    mem_usage += sizeof(int) * MAXDRV;

    fast_itdrv = xcalloc(sizeof(int) * MAXDRV, IM_BASE);
    if (!fast_itdrv) return 0;
    xlog("Allocated fast_itdrvs: %.2fM (%zu*%d)", sizeof(int) * MAXDRV / 1024.0 / 1024.0, sizeof(int), MAXDRV);
    mem_usage += sizeof(int) * MAXDRV;

    dir = opendir("runtime/generic");
    if (dir) {
        while ((de = readdir(dir))) {
            if (!endcmp(de->d_name, ".dll")) continue;

            sprintf(name, "./runtime/generic/%s", de->d_name);

            add_lib(name);
        }
        closedir(dir);
    }

    sprintf(dirname, "runtime/%d", areaID);
    dir = opendir(dirname);
    if (dir) {
        while ((de = readdir(dir))) {
            if (!endcmp(de->d_name, ".dll")) continue;

            sprintf(name, "./%s/%s", dirname, de->d_name);

            add_lib(name);
        }
        closedir(dir);
    }

    if (abi_failed) {
        elog("Startup aborted: one or more drivers failed the ABI check (see FATAL ABI MISMATCH above).");
        return 0; // main() exits(1)
    }
    xlog("ABI check passed for all loaded drivers (au_abi v%u, effect %u bytes).", au_abi_info.version, au_abi_info.sz_effect);

    return 1;
}

void exit_lib(void) {
    xfree(fast_chdrv);
    xfree(fast_itdrv);
    xfree(libs);
}

int char_driver(int nr, int type, int cn, intptr_t ret, int last_action) {
    int n, val = 0;

    if (nr == 0) {
        if (type == 0) player_driver(cn, ret, last_action);
        return 1;
    }

    if (nr < 0 || nr >= MAXDRV) {
        elog("ERROR: Character driver %d out of bounds", nr);
        return 0;
    }

    if ((n = fast_chdrv[nr]) && libs[n].used && load_lib(libs + n) && (val = libs[n].driver(type, nr, cn, ret, last_action))) return val;

    //xlog("Fast lookup failed for character driver %d.",nr);

    for (n = 1; n < MAXLIB; n++)
        if (libs[n].used && load_lib(libs + n) && (val = libs[n].driver(type, nr, cn, ret, last_action))) break;

    if (n == MAXLIB) {
        elog("ERROR: Could not find character driver %d in any library", nr);
        return 0;
    }

    fast_chdrv[nr] = n;

    return val;
}

int item_driver(int nr, int in, int cn) {
    int n, tmp = 0;

    if (nr == 0) {
        if (cn) look_item(cn, it + in, -1);
        return 1;
    }

    if (nr >= 1000) return 1; // identity tag

    if (nr < 0 || nr >= MAXDRV) {
        elog("ERROR: Item driver %d out of bounds", nr);
        return 0;
    }

    switch (nr) {
    case IDR_BONEBRIDGE:
        if (areaID != 18) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_BONEHINT:
        if (areaID != 18) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_NOMADDICE:
        if (areaID != 19) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_STAFFER2:
        if (areaID != 29) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_OXYPOTION:
        if (areaID != 31) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_LIZARDFLOWER:
        if (areaID != 31) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_CALIGAR:
        if (areaID != 36) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    case IDR_ARKHATA:
        if (areaID != 37) {
            if (cn) log_char(cn, LOG_SYSTEM, 0, "This does not work outside its area.");
            return 1;
        }
        break;
    }

    if ((n = fast_itdrv[nr]) && libs[n].used && load_lib(libs + n) && (tmp = libs[n].driver(CDT_ITEM, nr, in, cn, 0))) return tmp;

    //xlog("Fast lookup failed for item driver %d.",nr);

    for (n = 1; n < MAXLIB; n++)
        if (libs[n].used && load_lib(libs + n) && (tmp = libs[n].driver(CDT_ITEM, nr, in, cn, 0))) break;

    if (n == MAXLIB) {
        elog("ERROR: Could not find item driver %d in any library", nr);
        return 0;
    }

    fast_itdrv[nr] = n;

    return tmp;
}
