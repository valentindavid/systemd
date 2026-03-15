/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#if HAVE_LIBDEVMAPPER
#include "dlfcn-util.h"

#include <libdevmapper.h>

extern DLSYM_PROTOTYPE(dm_get_next_target);
extern DLSYM_PROTOTYPE(dm_task_add_target);
extern DLSYM_PROTOTYPE(dm_task_create);
extern DLSYM_PROTOTYPE(dm_task_destroy);
extern DLSYM_PROTOTYPE(dm_task_get_errno);
extern DLSYM_PROTOTYPE(dm_task_get_info);
extern DLSYM_PROTOTYPE(dm_task_run);
extern DLSYM_PROTOTYPE(dm_task_set_name);
extern DLSYM_PROTOTYPE(dm_task_set_ro);

DEFINE_TRIVIAL_CLEANUP_FUNC_FULL(struct dm_task *, sym_dm_task_destroy, NULL);

int swap_verity_devices(const char *name, const char *new_dev, const char *new_hash_dev);

#endif

int dlopen_devmapper(void);
