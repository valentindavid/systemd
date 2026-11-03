/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

int dm_ioctl_run(const char *name, uint32_t cmd, struct dm_ioctl *data, size_t data_size);
int dm_load_table(const char *name, const char *target_type, uint64_t start, uint64_t size, const char *target_params, uint32_t flags);

int swap_verity_devices(const char *name, const char *new_dev, const char *new_hash_dev);
int dm_is_verity(const char *name, char **ret_dev, char **ret_hash_dev);
