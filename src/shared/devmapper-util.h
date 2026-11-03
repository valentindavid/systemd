/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

int swap_verity_devices(const char *name, const char *new_dev, const char *new_hash_dev);
int dm_is_verity(const char *name, char **ret_dev, char **ret_hash_dev);
