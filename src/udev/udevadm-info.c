/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sd-device.h"

#include "alloc-util.h"
#include "device-enumerator-private.h"
#include "device-private.h"
#include "device-util.h"
#include "dirent-util.h"
#include "fd-util.h"
#include "sort-util.h"
#include "static-destruct.h"
#include "string-table.h"
#include "string-util.h"
#include "udev-util.h"
#include "udevadm-util.h"
#include "udevadm.h"

typedef enum ActionType {
        ACTION_QUERY,
        ACTION_ATTRIBUTE_WALK,
        ACTION_DEVICE_ID_FILE,
} ActionType;

typedef enum QueryType {
        QUERY_NAME,
        QUERY_PATH,
        QUERY_SYMLINK,
        QUERY_PROPERTY,
        QUERY_ALL,
} QueryType;

static char **arg_properties = NULL;
static bool arg_root = false;
static bool arg_export = false;
static bool arg_value = false;
static const char *arg_export_prefix = NULL;
static usec_t arg_wait_for_initialization_timeout = 0;

static bool skip_attribute(const char *name) {
        /* Those are either displayed separately or should not be shown at all. */
        return STR_IN_SET(name,
                          "uevent",
                          "dev",
                          "modalias",
                          "resource",
                          "driver",
                          "subsystem",
                          "module");
}

typedef struct SysAttr {
        const char *name;
        const char *value;
} SysAttr;

STATIC_DESTRUCTOR_REGISTER(arg_properties, strv_freep);

static int sysattr_compare(const SysAttr *a, const SysAttr *b) {
        return strcmp(a->name, b->name);
}

static int print_all_attributes(sd_device *device, bool is_parent) {
        _cleanup_free_ SysAttr *sysattrs = NULL;
        const char *name, *value;
        size_t n_items = 0;
        int r;

        value = NULL;
        (void) sd_device_get_devpath(device, &value);
        printf("  looking at %sdevice '%s':\n", is_parent ? "parent " : "", strempty(value));

        value = NULL;
        (void) sd_device_get_sysname(device, &value);
        printf("    %s==\"%s\"\n", is_parent ? "KERNELS" : "KERNEL", strempty(value));

        value = NULL;
        (void) sd_device_get_subsystem(device, &value);
        printf("    %s==\"%s\"\n", is_parent ? "SUBSYSTEMS" : "SUBSYSTEM", strempty(value));

        value = NULL;
        (void) sd_device_get_driver(device, &value);
        printf("    %s==\"%s\"\n", is_parent ? "DRIVERS" : "DRIVER", strempty(value));

        FOREACH_DEVICE_SYSATTR(device, name) {
                size_t len;

                if (skip_attribute(name))
                        continue;

                r = sd_device_get_sysattr_value(device, name, &value);
                if (r >= 0) {
                        /* skip any values that look like a path */
                        if (value[0] == '/')
                                continue;

                        /* skip nonprintable attributes */
                        len = strlen(value);
                        while (len > 0 && isprint((unsigned char) value[len-1]))
                                len--;
                        if (len > 0)
                                continue;

                } else if (r == -EPERM)
                        value = "(write-only)";
                else
                        continue;

                if (!GREEDY_REALLOC(sysattrs, n_items + 1))
                        return log_oom();

                sysattrs[n_items] = (SysAttr) {
                        .name = name,
                        .value = value,
                };
                n_items++;
        }

        typesafe_qsort(sysattrs, n_items, sysattr_compare);

        for (size_t i = 0; i < n_items; i++)
                printf("    %s{%s}==\"%s\"\n", is_parent ? "ATTRS" : "ATTR", sysattrs[i].name, sysattrs[i].value);

        puts("");

        return 0;
}

static int print_device_chain(sd_device *device) {
        sd_device *child, *parent;
        int r;

        printf("\n"
               "Udevadm info starts with the device specified by the devpath and then\n"
               "walks up the chain of parent devices. It prints for every device\n"
               "found, all possible attributes in the udev rules key format.\n"
               "A rule to match, can be composed by the attributes of the device\n"
               "and the attributes from one single parent device.\n"
               "\n");

        r = print_all_attributes(device, false);
        if (r < 0)
                return r;

        for (child = device; sd_device_get_parent(child, &parent) >= 0; child = parent) {
                r = print_all_attributes(parent, true);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int print_record(sd_device *device) {
        const char *str, *val;
        int i;

        (void) sd_device_get_devpath(device, &str);
        printf("P: %s\n", str);

        if (sd_device_get_devname(device, &str) >= 0) {
                assert_se(val = path_startswith(str, "/dev/"));
                printf("N: %s\n", val);
        }

        if (device_get_devlink_priority(device, &i) >= 0)
                printf("L: %i\n", i);

        FOREACH_DEVICE_DEVLINK(device, str) {
                assert_se(val = path_startswith(str, "/dev/"));
                printf("S: %s\n", val);
        }

        FOREACH_DEVICE_PROPERTY(device, str, val)
                printf("E: %s=%s\n", str, val);

        puts("");
        return 0;
}

static int stat_device(const char *name, bool export, const char *prefix) {
        struct stat statbuf;

        if (stat(name, &statbuf) != 0)
                return -errno;

        if (export) {
                if (!prefix)
                        prefix = "INFO_";
                printf("%sMAJOR=%u\n"
                       "%sMINOR=%u\n",
                       prefix, major(statbuf.st_dev),
                       prefix, minor(statbuf.st_dev));
        } else
                printf("%u:%u\n", major(statbuf.st_dev), minor(statbuf.st_dev));
        return 0;
}

static int export_devices(void) {
        _cleanup_(sd_device_enumerator_unrefp) sd_device_enumerator *e = NULL;
        sd_device *d;
        int r;

        r = sd_device_enumerator_new(&e);
        if (r < 0)
                return log_oom();

        r = sd_device_enumerator_allow_uninitialized(e);
        if (r < 0)
                return log_error_errno(r, "Failed to set allowing uninitialized flag: %m");

        r = device_enumerator_scan_devices(e);
        if (r < 0)
                return log_error_errno(r, "Failed to scan devices: %m");

        FOREACH_DEVICE_AND_SUBSYSTEM(e, d)
                (void) print_record(d);

        return 0;
}

static void cleanup_dir(DIR *dir, mode_t mask, int depth) {
        struct dirent *dent;

        if (depth <= 0)
                return;

        FOREACH_DIRENT_ALL(dent, dir, break) {
                struct stat stats;

                if (dent->d_name[0] == '.')
                        continue;
                if (fstatat(dirfd(dir), dent->d_name, &stats, AT_SYMLINK_NOFOLLOW) != 0)
                        continue;
                if ((stats.st_mode & mask) != 0)
                        continue;
                if (S_ISDIR(stats.st_mode)) {
                        _cleanup_closedir_ DIR *dir2 = NULL;

                        dir2 = fdopendir(openat(dirfd(dir), dent->d_name, O_RDONLY|O_NONBLOCK|O_DIRECTORY|O_CLOEXEC));
                        if (dir2)
                                cleanup_dir(dir2, mask, depth-1);

                        (void) unlinkat(dirfd(dir), dent->d_name, AT_REMOVEDIR);
                } else
                        (void) unlinkat(dirfd(dir), dent->d_name, 0);
        }
}

static void cleanup_db(void) {
        _cleanup_closedir_ DIR *dir1 = NULL, *dir2 = NULL, *dir3 = NULL, *dir4 = NULL, *dir5 = NULL;

        dir1 = opendir("/run/udev/data");
        if (dir1)
                cleanup_dir(dir1, S_ISVTX, 1);

        dir2 = opendir("/run/udev/links");
        if (dir2)
                cleanup_dir(dir2, 0, 2);

        dir3 = opendir("/run/udev/tags");
        if (dir3)
                cleanup_dir(dir3, 0, 2);

        dir4 = opendir("/run/udev/static_node-tags");
        if (dir4)
                cleanup_dir(dir4, 0, 2);

        dir5 = opendir("/run/udev/watch");
        if (dir5)
                cleanup_dir(dir5, 0, 1);
}

static int query_device(QueryType query, sd_device* device) {
        int r;

        assert(device);

        switch(query) {
        case QUERY_NAME: {
                const char *node;

                r = sd_device_get_devname(device, &node);
                if (r < 0)
                        return log_error_errno(r, "No device node found: %m");

                if (!arg_root)
                        assert_se(node = path_startswith(node, "/dev/"));
                printf("%s\n", node);
                return 0;
        }

        case QUERY_SYMLINK: {
                const char *devlink, *prefix = "";

                FOREACH_DEVICE_DEVLINK(device, devlink) {
                        if (!arg_root)
                                assert_se(devlink = path_startswith(devlink, "/dev/"));
                        printf("%s%s", prefix, devlink);
                        prefix = " ";
                }
                puts("");
                return 0;
        }

        case QUERY_PATH: {
                const char *devpath;

                r = sd_device_get_devpath(device, &devpath);
                if (r < 0)
                        return log_error_errno(r, "Failed to get device path: %m");

                printf("%s\n", devpath);
                return 0;
        }

        case QUERY_PROPERTY: {
                const char *key, *value;

                FOREACH_DEVICE_PROPERTY(device, key, value) {
                        if (arg_properties && !strv_contains(arg_properties, key))
                                continue;

                        if (arg_export)
                                printf("%s%s='%s'\n", strempty(arg_export_prefix), key, value);
                        else if (arg_value)
                                printf("%s\n", value);
                        else
                                printf("%s=%s\n", key, value);
                }

                return 0;
        }

        case QUERY_ALL:
                return print_record(device);
        }

        assert_not_reached();
        return 0;
}

static int help(void) {
        printf("%s info [OPTIONS] [DEVPATH|FILE]\n\n"
               "Query sysfs or the udev database.\n\n"
               "  -h --help                   Print this message\n"
               "  -V --version                Print version of the program\n"
               "  -q --query=TYPE             Query device information:\n"
               "       name                     Name of device node\n"
               "       symlink                  Pointing to node\n"
               "       path                     sysfs device path\n"
               "       property                 The device properties\n"
               "       all                      All values\n"
               "     --property=NAME          Show only properties by this name\n"
               "     --value                  When showing properties, print only their values\n"
               "  -p --path=SYSPATH           sysfs device path used for query or attribute walk\n"
               "  -n --name=NAME              Node or symlink name used for query or attribute walk\n"
               "  -r --root                   Prepend dev directory to path names\n"
               "  -a --attribute-walk         Print all key matches walking along the chain\n"
               "                              of parent devices\n"
               "  -d --device-id-of-file=FILE Print major:minor of device containing this file\n"
               "  -x --export                 Export key/value pairs\n"
               "  -P --export-prefix          Export the key name with a prefix\n"
               "  -e --export-db              Export the content of the udev database\n"
               "  -c --cleanup-db             Clean up the udev database\n"
               "  -w --wait-for-initialization[=SECONDS]\n"
               "                              Wait for device to be initialized\n",
               program_invocation_short_name);

        return 0;
}

int info_main(int argc, char *argv[], void *userdata) {
        _cleanup_strv_free_ char **devices = NULL;
        _cleanup_free_ char *name = NULL;
        int c, r;

        enum {
                ARG_PROPERTY = 0x100,
                ARG_VALUE,
        };

        static const struct option options[] = {
                { "attribute-walk",          no_argument,       NULL, 'a'          },
                { "cleanup-db",              no_argument,       NULL, 'c'          },
                { "device-id-of-file",       required_argument, NULL, 'd'          },
                { "export",                  no_argument,       NULL, 'x'          },
                { "export-db",               no_argument,       NULL, 'e'          },
                { "export-prefix",           required_argument, NULL, 'P'          },
                { "help",                    no_argument,       NULL, 'h'          },
                { "name",                    required_argument, NULL, 'n'          },
                { "path",                    required_argument, NULL, 'p'          },
                { "property",                required_argument, NULL, ARG_PROPERTY },
                { "query",                   required_argument, NULL, 'q'          },
                { "root",                    no_argument,       NULL, 'r'          },
                { "value",                   no_argument,       NULL, ARG_VALUE    },
                { "version",                 no_argument,       NULL, 'V'          },
                { "wait-for-initialization", optional_argument, NULL, 'w'          },
                {}
        };

        ActionType action = ACTION_QUERY;
        QueryType query = QUERY_ALL;

        while ((c = getopt_long(argc, argv, "aced:n:p:q:rxP:w::Vh", options, NULL)) >= 0)
                switch (c) {
                case ARG_PROPERTY:
                        /* Make sure that if the empty property list was specified, we won't show any
                           properties. */
                        if (isempty(optarg) && !arg_properties) {
                                arg_properties = new0(char*, 1);
                                if (!arg_properties)
                                        return log_oom();
                        } else {
                                r = strv_split_and_extend(&arg_properties, optarg, ",", true);
                                if (r < 0)
                                        return log_oom();
                        }
                        break;
                case ARG_VALUE:
                        arg_value = true;
                        break;
                case 'n':
                case 'p': {
                        const char *prefix = c == 'n' ? "/dev/" : "/sys/";
                        char *path;

                        path = path_join(path_startswith(optarg, prefix) ? NULL : prefix, optarg);
                        if (!path)
                                return log_oom();

                        r = strv_consume(&devices, path);
                        if (r < 0)
                                return log_oom();
                        break;
                }

                case 'q':
                        action = ACTION_QUERY;
                        if (streq(optarg, "property") || streq(optarg, "env"))
                                query = QUERY_PROPERTY;
                        else if (streq(optarg, "name"))
                                query = QUERY_NAME;
                        else if (streq(optarg, "symlink"))
                                query = QUERY_SYMLINK;
                        else if (streq(optarg, "path"))
                                query = QUERY_PATH;
                        else if (streq(optarg, "all"))
                                query = QUERY_ALL;
                        else
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "unknown query type");
                        break;
                case 'r':
                        arg_root = true;
                        break;
                case 'd':
                        action = ACTION_DEVICE_ID_FILE;
                        r = free_and_strdup(&name, optarg);
                        if (r < 0)
                                return log_oom();
                        break;
                case 'a':
                        action = ACTION_ATTRIBUTE_WALK;
                        break;
                case 'e':
                        return export_devices();
                case 'c':
                        cleanup_db();
                        return 0;
                case 'x':
                        arg_export = true;
                        break;
                case 'P':
                        arg_export = true;
                        arg_export_prefix = optarg;
                        break;
                case 'w':
                        if (optarg) {
                                r = parse_sec(optarg, &arg_wait_for_initialization_timeout);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to parse timeout value: %m");
                        } else
                                arg_wait_for_initialization_timeout = USEC_INFINITY;
                        break;
                case 'V':
                        return print_version();
                case 'h':
                        return help();
                case '?':
                        return -EINVAL;
                default:
                        assert_not_reached();
                }

        if (action == ACTION_DEVICE_ID_FILE) {
                if (argv[optind])
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Positional arguments are not allowed with -d/--device-id-of-file.");
                assert(name);
                return stat_device(name, arg_export, arg_export_prefix);
        }

        r = strv_extend_strv(&devices, argv + optind, false);
        if (r < 0)
                return log_error_errno(r, "Failed to build argument list: %m");

        if (strv_isempty(devices))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "A device name or path is required");
        if (action == ACTION_ATTRIBUTE_WALK && strv_length(devices) > 1)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Only one device may be specified with -a/--attribute-walk");

        if (arg_export && arg_value)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "-x/--export or -P/--export-prefix cannot be used with --value");

        char **p;
        STRV_FOREACH(p, devices) {
                _cleanup_(sd_device_unrefp) sd_device *device = NULL;

                r = find_device(*p, NULL, &device);
                if (r == -EINVAL)
                        return log_error_errno(r, "Bad argument \"%s\", expected an absolute path in /dev/ or /sys or a unit name: %m", *p);
                if (r < 0)
                        return log_error_errno(r, "Unknown device \"%s\": %m",  *p);

                if (arg_wait_for_initialization_timeout > 0) {
                        sd_device *d;

                        r = device_wait_for_initialization(
                                        device,
                                        NULL,
                                        usec_add(now(CLOCK_MONOTONIC), arg_wait_for_initialization_timeout),
                                        &d);
                        if (r < 0)
                                return r;

                        sd_device_unref(device);
                        device = d;
                }

                if (action == ACTION_QUERY)
                        r = query_device(query, device);
                else if (action == ACTION_ATTRIBUTE_WALK)
                        r = print_device_chain(device);
                else
                        assert_not_reached();
                if (r < 0)
                        return r;
        }

        return 0;
}
