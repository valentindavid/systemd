/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/sysmacros.h>

#include "alloc-util.h"
#include "devmapper-util.h"
#include "log.h"
#include "sd-dlopen.h"

#if HAVE_LIBDEVMAPPER
static void *devmapper_dl = NULL;

DLSYM_PROTOTYPE(dm_get_next_target) = NULL;
DLSYM_PROTOTYPE(dm_task_add_target) = NULL;
DLSYM_PROTOTYPE(dm_task_create) = NULL;
DLSYM_PROTOTYPE(dm_task_destroy) = NULL;
DLSYM_PROTOTYPE(dm_task_get_errno) = NULL;
DLSYM_PROTOTYPE(dm_task_get_info) = NULL;
DLSYM_PROTOTYPE(dm_task_run) = NULL;
DLSYM_PROTOTYPE(dm_task_set_name) = NULL;
DLSYM_PROTOTYPE(dm_task_set_ro) = NULL;
#endif

int dlopen_devmapper(void) {
#if HAVE_LIBDEVMAPPER
        int r;

        SD_ELF_NOTE_DLOPEN("devmapper",
                        "Support for devmapper",
                        SD_ELF_NOTE_DLOPEN_PRIORITY_SUGGESTED,
                        "libdevmapper.so.1.02");

        r = dlopen_many_sym_or_warn(
                        &devmapper_dl, "libdevmapper.so.1.02", LOG_DEBUG,
                        DLSYM_ARG(dm_get_next_target),
                        DLSYM_ARG(dm_task_add_target),
                        DLSYM_ARG(dm_task_create),
                        DLSYM_ARG(dm_task_destroy),
                        DLSYM_ARG(dm_task_get_errno),
                        DLSYM_ARG(dm_task_get_info),
                        DLSYM_ARG(dm_task_run),
                        DLSYM_ARG(dm_task_set_name),
                        DLSYM_ARG(dm_task_set_ro));
        if (r <= 0)
                return r;

        return 1;
#else
        return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "cryptsetup support is not compiled in.");
#endif
}

#if HAVE_LIBDEVMAPPER

static int resume(const char *name) {
        _cleanup_(sym_dm_task_destroyp) struct dm_task *task = NULL;

        task = sym_dm_task_create(DM_DEVICE_RESUME);
        if (!task)
                return log_oom();
        if (!sym_dm_task_set_name(task, name))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Could not set name %s to devicemapper task", name);
        if (!sym_dm_task_run(task))
                return log_error_errno(sym_dm_task_get_errno(task), "Could not run resume task");

        return 0;
}

static int get_params(const char *name, uint64_t *start, uint64_t *size, char **ret) {
        _cleanup_(sym_dm_task_destroyp) struct dm_task *task = NULL;
        struct dm_info info;
        char *params = NULL;
        void *next = NULL;
        char *target_type = NULL;

        assert(ret);

        task = sym_dm_task_create(DM_DEVICE_TABLE);
        if (!task)
                return log_oom();
        if (!sym_dm_task_set_name(task, name))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Could not set name %s to devicemapper task", name);
        if (!sym_dm_task_run(task))
                return log_error_errno(sym_dm_task_get_errno(task), "Could not get the device mapper table");
        if (!sym_dm_task_get_info(task, &info))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Could not get retrieve device mapper table");

        next = sym_dm_get_next_target(task, next, start, size, &target_type, &params);
        if (!target_type || (strcmp(target_type, "verity") != 0) || next)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Expected a dm-verity");

        *ret = strdup(params);
        if (!ret)
                        return log_oom();

        return 0;
}

static int reload_params(const char *name, const char *params, uint64_t start, uint64_t size) {
        _cleanup_(sym_dm_task_destroyp) struct dm_task *task = NULL;

        task = sym_dm_task_create(DM_DEVICE_RELOAD);
        if (!task)
                return log_oom();
        if (!sym_dm_task_set_name(task, name))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Could not set name %s to devicemapper task", name);
        if (!sym_dm_task_set_ro(task))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Could not set task read only");

        if (!sym_dm_task_add_target(task, start, size, "verity", params))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Could not add target to task");

        if (!sym_dm_task_run(task))
                return log_error_errno(sym_dm_task_get_errno(task), "Could not add target to devicemapper");

        return 0;
}

static int device_name(const char *node_path, char **ret) {
        struct stat st;

        assert(ret);
        assert(!*ret);

        if (stat(node_path, &st) < 0)
                return -errno;

        if (asprintf(ret, "%u:%u", major(st.st_rdev), minor(st.st_rdev)) < 0)
                return -ENOMEM;

        return 0;
}

static int update_params(const char *src_params, const char *new_dev, const char *new_hash_dev, char **ret) {
        _cleanup_free_ char *params = NULL;
        _cleanup_free_ char *new_dev_formatted = NULL;
        _cleanup_free_ char *new_hash_dev_formatted = NULL;
        int r;

        assert(ret);
        assert(!*ret);

        params = strdup(src_params);
        if (!params)
                return log_oom();

        const char* params_comps[10];
        size_t index = 0;
        params_comps[index++] = params;
        for (size_t i = 0; params[i] && index < 10; ++i) {
                if (params[i] == ' ') {
                        params_comps[index++] = params + i + 1;
                        params[i] = '\0';
                }
        }

        if (index != 10)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Not enough parameters");

        if (new_dev) {
                r = device_name(new_dev, &new_dev_formatted);
                if (r < 0)
                        return r;
        }

        if (new_hash_dev) {
                r = device_name(new_hash_dev, &new_hash_dev_formatted);
                if (r < 0)
                        return r;
        }

        if (asprintf(ret, "%s %s %s %s %s %s %s %s %s %s",
                     params_comps[0],
                     new_dev_formatted?new_dev_formatted:params_comps[1],
                     new_hash_dev_formatted?new_hash_dev_formatted:params_comps[2],
                     params_comps[3],
                     params_comps[4],
                     params_comps[5],
                     params_comps[6],
                     params_comps[7],
                     params_comps[8],
                     params_comps[9]) < 0)
                return log_oom();

        return 0;
}

int swap_verity_devices(const char *name, const char *new_dev, const char *new_hash_dev) {
        _cleanup_free_ char* src_params = NULL;
        _cleanup_free_ char* new_params = NULL;
        uint64_t start, size;
        int r;

        assert(name);
        assert(new_dev || new_hash_dev);

        r = dlopen_devmapper();
        if (r < 0)
                return r;

        r = get_params(name, &start, &size, &src_params);
        if (r < 0)
                return r;

        r = update_params(src_params, new_dev, new_hash_dev, &new_params);
        if (r < 0)
                return r;

        r = reload_params(name, new_params, start, size);
        if (r < 0)
                return r;

        r = resume(name);
        if (r < 0)
                return r;

        return 0;
}

#endif
