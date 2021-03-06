/* SPDX-License-Identifier: GPL-2.0+ */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "sd-event.h"

#include "alloc-util.h"
#include "device-private.h"
#include "fd-util.h"
#include "format-util.h"
#include "libudev-device-internal.h"
#include "netlink-util.h"
#include "path-util.h"
#include "process-util.h"
#include "signal-util.h"
#include "string-util.h"
#include "udev-builtin.h"
#include "udev-node.h"
#include "udev-watch.h"
#include "udev.h"

typedef struct Spawn {
        const char *cmd;
        pid_t pid;
        usec_t timeout_warn_usec;
        usec_t timeout_usec;
        usec_t event_birth_usec;
        bool accept_failure;
        int fd_stdout;
        int fd_stderr;
        char *result;
        size_t result_size;
        size_t result_len;
} Spawn;

struct udev_event *udev_event_new(struct udev_device *dev) {
        struct udev_event *event;

        event = new0(struct udev_event, 1);
        if (event == NULL)
                return NULL;
        event->dev = dev;
        event->birth_usec = now(CLOCK_MONOTONIC);
        return event;
}

void udev_event_unref(struct udev_event *event) {
        void *p;

        if (event == NULL)
                return;
        sd_netlink_unref(event->rtnl);
        while ((p = hashmap_steal_first_key(event->run_list)))
                free(p);
        hashmap_free_free_free(event->seclabel_list);
        free(event->program_result);
        free(event->name);
        free(event);
}

enum subst_type {
        SUBST_UNKNOWN,
        SUBST_DEVNODE,
        SUBST_ATTR,
        SUBST_ENV,
        SUBST_KERNEL,
        SUBST_KERNEL_NUMBER,
        SUBST_DRIVER,
        SUBST_DEVPATH,
        SUBST_ID,
        SUBST_MAJOR,
        SUBST_MINOR,
        SUBST_RESULT,
        SUBST_PARENT,
        SUBST_NAME,
        SUBST_LINKS,
        SUBST_ROOT,
        SUBST_SYS,
};

static size_t subst_format_var(struct udev_event *event, struct udev_device *dev,
                               enum subst_type type, char *attr,
                               char *dest, size_t l) {
        char *s = dest;

        switch (type) {
        case SUBST_DEVPATH:
                l = strpcpy(&s, l, udev_device_get_devpath(dev));
                break;
        case SUBST_KERNEL:
                l = strpcpy(&s, l, udev_device_get_sysname(dev));
                break;
        case SUBST_KERNEL_NUMBER:
                if (udev_device_get_sysnum(dev) == NULL)
                        break;
                l = strpcpy(&s, l, udev_device_get_sysnum(dev));
                break;
        case SUBST_ID:
                if (event->dev_parent == NULL)
                        break;
                l = strpcpy(&s, l, udev_device_get_sysname(event->dev_parent));
                break;
        case SUBST_DRIVER: {
                const char *driver;

                if (event->dev_parent == NULL)
                        break;

                driver = udev_device_get_driver(event->dev_parent);
                if (driver == NULL)
                        break;
                l = strpcpy(&s, l, driver);
                break;
        }
        case SUBST_MAJOR: {
                char num[UTIL_PATH_SIZE];

                sprintf(num, "%u", major(udev_device_get_devnum(dev)));
                l = strpcpy(&s, l, num);
                break;
        }
        case SUBST_MINOR: {
                char num[UTIL_PATH_SIZE];

                sprintf(num, "%u", minor(udev_device_get_devnum(dev)));
                l = strpcpy(&s, l, num);
                break;
        }
        case SUBST_RESULT: {
                char *rest;
                int i;

                if (event->program_result == NULL)
                        break;
                /* get part of the result string */
                i = 0;
                if (attr != NULL)
                        i = strtoul(attr, &rest, 10);
                if (i > 0) {
                        char result[UTIL_PATH_SIZE];
                        char tmp[UTIL_PATH_SIZE];
                        char *cpos;

                        strscpy(result, sizeof(result), event->program_result);
                        cpos = result;
                        while (--i) {
                                while (cpos[0] != '\0' && !isspace(cpos[0]))
                                        cpos++;
                                while (isspace(cpos[0]))
                                        cpos++;
                                if (cpos[0] == '\0')
                                        break;
                        }
                        if (i > 0) {
                                log_error("requested part of result string not found");
                                break;
                        }
                        strscpy(tmp, sizeof(tmp), cpos);
                        /* %{2+}c copies the whole string from the second part on */
                        if (rest[0] != '+') {
                                cpos = strchr(tmp, ' ');
                                if (cpos)
                                        cpos[0] = '\0';
                        }
                        l = strpcpy(&s, l, tmp);
                } else {
                        l = strpcpy(&s, l, event->program_result);
                }
                break;
        }
        case SUBST_ATTR: {
                const char *value = NULL;
                char vbuf[UTIL_NAME_SIZE];
                size_t len;
                int count;

                if (attr == NULL) {
                        log_error("missing file parameter for attr");
                        break;
                }

                /* try to read the value specified by "[dmi/id]product_name" */
                if (util_resolve_subsys_kernel(attr, vbuf, sizeof(vbuf), 1) == 0)
                        value = vbuf;

                /* try to read the attribute the device */
                if (value == NULL)
                        value = udev_device_get_sysattr_value(event->dev, attr);

                /* try to read the attribute of the parent device, other matches have selected */
                if (value == NULL && event->dev_parent != NULL && event->dev_parent != event->dev)
                        value = udev_device_get_sysattr_value(event->dev_parent, attr);

                if (value == NULL)
                        break;

                /* strip trailing whitespace, and replace unwanted characters */
                if (value != vbuf)
                        strscpy(vbuf, sizeof(vbuf), value);
                len = strlen(vbuf);
                while (len > 0 && isspace(vbuf[--len]))
                        vbuf[len] = '\0';
                count = util_replace_chars(vbuf, UDEV_ALLOWED_CHARS_INPUT);
                if (count > 0)
                        log_debug("%i character(s) replaced" , count);
                l = strpcpy(&s, l, vbuf);
                break;
        }
        case SUBST_PARENT: {
                struct udev_device *dev_parent;
                const char *devnode;

                dev_parent = udev_device_get_parent(event->dev);
                if (dev_parent == NULL)
                        break;
                devnode = udev_device_get_devnode(dev_parent);
                if (devnode != NULL)
                        l = strpcpy(&s, l, devnode + STRLEN("/dev/"));
                break;
        }
        case SUBST_DEVNODE:
                if (udev_device_get_devnode(dev) != NULL)
                        l = strpcpy(&s, l, udev_device_get_devnode(dev));
                break;
        case SUBST_NAME:
                if (event->name != NULL)
                        l = strpcpy(&s, l, event->name);
                else if (udev_device_get_devnode(dev) != NULL)
                        l = strpcpy(&s, l,
                                    udev_device_get_devnode(dev) + STRLEN("/dev/"));
                else
                        l = strpcpy(&s, l, udev_device_get_sysname(dev));
                break;
        case SUBST_LINKS: {
                struct udev_list_entry *list_entry;

                list_entry = udev_device_get_devlinks_list_entry(dev);
                if (list_entry == NULL)
                        break;
                l = strpcpy(&s, l,
                            udev_list_entry_get_name(list_entry) + STRLEN("/dev/"));
                udev_list_entry_foreach(list_entry, udev_list_entry_get_next(list_entry))
                        l = strpcpyl(&s, l, " ",
                                     udev_list_entry_get_name(list_entry) + STRLEN("/dev/"),
                                     NULL);
                break;
        }
        case SUBST_ROOT:
                l = strpcpy(&s, l, "/dev");
                break;
        case SUBST_SYS:
                l = strpcpy(&s, l, "/sys");
                break;
        case SUBST_ENV:
                if (attr == NULL) {
                        break;
                } else {
                        const char *value;

                        value = udev_device_get_property_value(event->dev, attr);
                        if (value == NULL)
                                break;
                        l = strpcpy(&s, l, value);
                        break;
                }
        default:
                log_error("unknown substitution type=%i", type);
                break;
        }

        return s - dest;
}

size_t udev_event_apply_format(struct udev_event *event,
                               const char *src, char *dest, size_t size,
                               bool replace_whitespace) {
        struct udev_device *dev = event->dev;
        static const struct subst_map {
                const char *name;
                const char fmt;
                enum subst_type type;
        } map[] = {
                { .name = "devnode",  .fmt = 'N', .type = SUBST_DEVNODE },
                { .name = "tempnode", .fmt = 'N', .type = SUBST_DEVNODE },
                { .name = "attr",     .fmt = 's', .type = SUBST_ATTR },
                { .name = "sysfs",    .fmt = 's', .type = SUBST_ATTR },
                { .name = "env",      .fmt = 'E', .type = SUBST_ENV },
                { .name = "kernel",   .fmt = 'k', .type = SUBST_KERNEL },
                { .name = "number",   .fmt = 'n', .type = SUBST_KERNEL_NUMBER },
                { .name = "driver",   .fmt = 'd', .type = SUBST_DRIVER },
                { .name = "devpath",  .fmt = 'p', .type = SUBST_DEVPATH },
                { .name = "id",       .fmt = 'b', .type = SUBST_ID },
                { .name = "major",    .fmt = 'M', .type = SUBST_MAJOR },
                { .name = "minor",    .fmt = 'm', .type = SUBST_MINOR },
                { .name = "result",   .fmt = 'c', .type = SUBST_RESULT },
                { .name = "parent",   .fmt = 'P', .type = SUBST_PARENT },
                { .name = "name",     .fmt = 'D', .type = SUBST_NAME },
                { .name = "links",    .fmt = 'L', .type = SUBST_LINKS },
                { .name = "root",     .fmt = 'r', .type = SUBST_ROOT },
                { .name = "sys",      .fmt = 'S', .type = SUBST_SYS },
        };
        const char *from;
        char *s;
        size_t l;

        assert(dev);

        from = src;
        s = dest;
        l = size;

        for (;;) {
                enum subst_type type = SUBST_UNKNOWN;
                char attrbuf[UTIL_PATH_SIZE];
                char *attr = NULL;
                size_t subst_len;

                while (from[0] != '\0') {
                        if (from[0] == '$') {
                                /* substitute named variable */
                                unsigned i;

                                if (from[1] == '$') {
                                        from++;
                                        goto copy;
                                }

                                for (i = 0; i < ELEMENTSOF(map); i++) {
                                        if (startswith(&from[1], map[i].name)) {
                                                type = map[i].type;
                                                from += strlen(map[i].name)+1;
                                                goto subst;
                                        }
                                }
                        } else if (from[0] == '%') {
                                /* substitute format char */
                                unsigned i;

                                if (from[1] == '%') {
                                        from++;
                                        goto copy;
                                }

                                for (i = 0; i < ELEMENTSOF(map); i++) {
                                        if (from[1] == map[i].fmt) {
                                                type = map[i].type;
                                                from += 2;
                                                goto subst;
                                        }
                                }
                        }
copy:
                        /* copy char */
                        if (l < 2) /* need space for this char and the terminating NUL */
                                goto out;
                        s[0] = from[0];
                        from++;
                        s++;
                        l--;
                }

                goto out;
subst:
                /* extract possible $format{attr} */
                if (from[0] == '{') {
                        unsigned i;

                        from++;
                        for (i = 0; from[i] != '}'; i++)
                                if (from[i] == '\0') {
                                        log_error("missing closing brace for format '%s'", src);
                                        goto out;
                                }

                        if (i >= sizeof(attrbuf))
                                goto out;
                        memcpy(attrbuf, from, i);
                        attrbuf[i] = '\0';
                        from += i+1;
                        attr = attrbuf;
                } else {
                        attr = NULL;
                }

                subst_len = subst_format_var(event, dev, type, attr, s, l);

                /* SUBST_RESULT handles spaces itself */
                if (replace_whitespace && type != SUBST_RESULT)
                        /* util_replace_whitespace can replace in-place,
                         * and does nothing if subst_len == 0
                         */
                        subst_len = util_replace_whitespace(s, s, subst_len);

                s += subst_len;
                l -= subst_len;
        }

out:
        assert(l >= 1);
        s[0] = '\0';
        return l;
}

static int on_spawn_io(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        Spawn *spawn = userdata;
        char buf[4096], *p;
        size_t size;
        ssize_t l;

        assert(spawn);
        assert(fd == spawn->fd_stdout || fd == spawn->fd_stderr);
        assert(!spawn->result || spawn->result_len < spawn->result_size);

        if (fd == spawn->fd_stdout && spawn->result) {
                p = spawn->result + spawn->result_len;
                size = spawn->result_size - spawn->result_len;
        } else {
                p = buf;
                size = sizeof(buf);
        }

        l = read(fd, p, size - 1);
        if (l < 0) {
                if (errno != EAGAIN)
                        log_error_errno(errno, "Failed to read stdout of '%s': %m", spawn->cmd);

                return 0;
        }

        p[l] = '\0';
        if (fd == spawn->fd_stdout && spawn->result)
                spawn->result_len += l;

        /* Log output only if we watch stderr. */
        if (l > 0 && spawn->fd_stderr >= 0) {
                _cleanup_strv_free_ char **v = NULL;
                char **q;

                v = strv_split_newlines(p);
                if (!v)
                        return 0;

                STRV_FOREACH(q, v)
                        log_debug("'%s'(%s) '%s'", spawn->cmd,
                                  fd == spawn->fd_stdout ? "out" : "err", *q);
        }

        return 0;
}

static int on_spawn_timeout(sd_event_source *s, uint64_t usec, void *userdata) {
        Spawn *spawn = userdata;
        char timeout[FORMAT_TIMESTAMP_RELATIVE_MAX];

        assert(spawn);

        kill_and_sigcont(spawn->pid, SIGKILL);

        log_error("Spawned process '%s' ["PID_FMT"] timed out after %s, killing", spawn->cmd, spawn->pid,
                  format_timestamp_relative(timeout, sizeof(timeout), spawn->timeout_usec));

        return 1;
}

static int on_spawn_timeout_warning(sd_event_source *s, uint64_t usec, void *userdata) {
        Spawn *spawn = userdata;
        char timeout[FORMAT_TIMESTAMP_RELATIVE_MAX];

        assert(spawn);

        log_warning("Spawned process '%s' ["PID_FMT"] is taking longer than %s to complete", spawn->cmd, spawn->pid,
                    format_timestamp_relative(timeout, sizeof(timeout), spawn->timeout_warn_usec));

        return 1;
}

static int on_spawn_sigchld(sd_event_source *s, const siginfo_t *si, void *userdata) {
        Spawn *spawn = userdata;

        assert(spawn);

        switch (si->si_code) {
        case CLD_EXITED:
                if (si->si_status == 0) {
                        log_debug("Process '%s' succeeded.", spawn->cmd);
                        sd_event_exit(sd_event_source_get_event(s), 0);

                        return 1;
                }

                log_full(spawn->accept_failure ? LOG_DEBUG : LOG_WARNING,
                         "Process '%s' failed with exit code %i.", spawn->cmd, si->si_status);
                break;
        case CLD_KILLED:
        case CLD_DUMPED:
                log_warning("Process '%s' terminated by signal %s.", spawn->cmd, signal_to_string(si->si_status));

                break;
        default:
                log_error("Process '%s' failed due to unknown reason.", spawn->cmd);
        }

        sd_event_exit(sd_event_source_get_event(s), -EIO);

        return 1;
}

static int spawn_wait(Spawn *spawn) {
        _cleanup_(sd_event_unrefp) sd_event *e = NULL;
        int r, ret;

        assert(spawn);

        r = sd_event_new(&e);
        if (r < 0)
                return r;

        if (spawn->timeout_usec > 0) {
                usec_t usec, age_usec;

                usec = now(CLOCK_MONOTONIC);
                age_usec = usec - spawn->event_birth_usec;
                if (age_usec < spawn->timeout_usec) {
                        if (spawn->timeout_warn_usec > 0 &&
                            spawn->timeout_warn_usec < spawn->timeout_usec &&
                            spawn->timeout_warn_usec > age_usec) {
                                spawn->timeout_warn_usec -= age_usec;

                                r = sd_event_add_time(e, NULL, CLOCK_MONOTONIC,
                                                      usec + spawn->timeout_warn_usec, USEC_PER_SEC,
                                                      on_spawn_timeout_warning, spawn);
                                if (r < 0)
                                        return r;
                        }

                        spawn->timeout_usec -= age_usec;

                        r = sd_event_add_time(e, NULL, CLOCK_MONOTONIC,
                                              usec + spawn->timeout_usec, USEC_PER_SEC, on_spawn_timeout, spawn);
                        if (r < 0)
                                return r;
                }
        }

        r = sd_event_add_io(e, NULL, spawn->fd_stdout, EPOLLIN, on_spawn_io, spawn);
        if (r < 0)
                return r;

        r = sd_event_add_io(e, NULL, spawn->fd_stderr, EPOLLIN, on_spawn_io, spawn);
        if (r < 0)
                return r;

        r = sd_event_add_child(e, NULL, spawn->pid, WEXITED, on_spawn_sigchld, spawn);
        if (r < 0)
                return r;

        r = sd_event_loop(e);
        if (r < 0)
                return r;

        r = sd_event_get_exit_code(e, &ret);
        if (r < 0)
                return r;

        return ret;
}

int udev_event_spawn(struct udev_event *event,
                     usec_t timeout_usec,
                     usec_t timeout_warn_usec,
                     bool accept_failure,
                     const char *cmd,
                     char *result, size_t ressize) {
        _cleanup_close_pair_ int outpipe[2] = {-1, -1}, errpipe[2] = {-1, -1};
        _cleanup_strv_free_ char **argv = NULL;
        char **envp = NULL;
        Spawn spawn;
        pid_t pid;
        int r;

        assert(result || ressize == 0);

        /* pipes from child to parent */
        if (result || log_get_max_level() >= LOG_INFO)
                if (pipe2(outpipe, O_NONBLOCK|O_CLOEXEC) != 0)
                        return log_error_errno(errno, "Failed to create pipe for command '%s': %m", cmd);

        if (log_get_max_level() >= LOG_INFO)
                if (pipe2(errpipe, O_NONBLOCK|O_CLOEXEC) != 0)
                        return log_error_errno(errno, "Failed to create pipe for command '%s': %m", cmd);

        argv = strv_split_full(cmd, NULL, SPLIT_QUOTES|SPLIT_RELAX);
        if (!argv)
                return log_oom();

        /* allow programs in /usr/lib/udev/ to be called without the path */
        if (!path_is_absolute(argv[0])) {
                char *program;

                program = path_join(NULL, UDEVLIBEXECDIR, argv[0]);
                if (!program)
                        return log_oom();

                free_and_replace(argv[0], program);
        }

        r = device_get_properties_strv(event->dev->device, &envp);
        if (r < 0)
                return log_error_errno(r, "Failed to get device properties");

        log_debug("Starting '%s'", cmd);

        r = safe_fork("(spawn)", FORK_RESET_SIGNALS|FORK_DEATHSIG|FORK_LOG, &pid);
        if (r < 0)
                return log_error_errno(r, "Failed to fork() to execute command '%s': %m", cmd);
        if (r == 0) {
                if (rearrange_stdio(-1, outpipe[WRITE_END], errpipe[WRITE_END]) < 0)
                        _exit(EXIT_FAILURE);

                (void) close_all_fds(NULL, 0);

                execve(argv[0], argv, envp);
                _exit(EXIT_FAILURE);
        }

        /* parent closed child's ends of pipes */
        outpipe[WRITE_END] = safe_close(outpipe[WRITE_END]);
        errpipe[WRITE_END] = safe_close(errpipe[WRITE_END]);

        spawn = (Spawn) {
                .cmd = cmd,
                .pid = pid,
                .accept_failure = accept_failure,
                .timeout_warn_usec = timeout_warn_usec,
                .timeout_usec = timeout_usec,
                .event_birth_usec = event->birth_usec,
                .fd_stdout = outpipe[READ_END],
                .fd_stderr = errpipe[READ_END],
                .result = result,
                .result_size = ressize,
        };
        r = spawn_wait(&spawn);
        if (r < 0)
                return log_error_errno(r, "Failed to wait spawned command '%s': %m", cmd);

        if (result)
                result[spawn.result_len] = '\0';

        return r;
}

static int rename_netif(struct udev_event *event) {
        struct udev_device *dev = event->dev;
        char name[IFNAMSIZ];
        const char *oldname;
        int r;

        oldname = udev_device_get_sysname(dev);

        strscpy(name, IFNAMSIZ, event->name);

        r = rtnl_set_link_name(&event->rtnl, udev_device_get_ifindex(dev), name);
        if (r < 0)
                return log_error_errno(r, "Error changing net interface name '%s' to '%s': %m", oldname, name);

        log_debug("renamed network interface '%s' to '%s'", oldname, name);

        return 0;
}

void udev_event_execute_rules(struct udev_event *event,
                              usec_t timeout_usec, usec_t timeout_warn_usec,
                              Hashmap *properties_list,
                              struct udev_rules *rules) {
        struct udev_device *dev = event->dev;

        if (udev_device_get_subsystem(dev) == NULL)
                return;

        if (streq(udev_device_get_action(dev), "remove")) {
                udev_device_read_db(dev);
                udev_device_tag_index(dev, NULL, false);
                udev_device_delete_db(dev);

                if (major(udev_device_get_devnum(dev)) != 0)
                        udev_watch_end(dev->device);

                udev_rules_apply_to_event(rules, event,
                                          timeout_usec, timeout_warn_usec,
                                          properties_list);

                if (major(udev_device_get_devnum(dev)) != 0)
                        udev_node_remove(dev->device);
        } else {
                event->dev_db = udev_device_clone_with_db(dev);
                if (event->dev_db != NULL) {
                        /* disable watch during event processing */
                        if (major(udev_device_get_devnum(dev)) != 0)
                                udev_watch_end(event->dev_db->device);

                        if (major(udev_device_get_devnum(dev)) == 0 &&
                            streq(udev_device_get_action(dev), "move"))
                                udev_device_copy_properties(dev, event->dev_db);
                }

                udev_rules_apply_to_event(rules, event,
                                          timeout_usec, timeout_warn_usec,
                                          properties_list);

                /* rename a new network interface, if needed */
                if (udev_device_get_ifindex(dev) > 0 && streq(udev_device_get_action(dev), "add") &&
                    event->name != NULL && !streq(event->name, udev_device_get_sysname(dev))) {
                        int r;

                        r = rename_netif(event);
                        if (r < 0)
                                log_warning_errno(r, "could not rename interface '%d' from '%s' to '%s': %m", udev_device_get_ifindex(dev),
                                                  udev_device_get_sysname(dev), event->name);
                        else {
                                r = udev_device_rename(dev, event->name);
                                if (r < 0)
                                        log_warning_errno(r, "renamed interface '%d' from '%s' to '%s', but could not update udev_device: %m",
                                                          udev_device_get_ifindex(dev), udev_device_get_sysname(dev), event->name);
                                else
                                        log_debug("changed devpath to '%s'", udev_device_get_devpath(dev));
                        }
                }

                if (major(udev_device_get_devnum(dev)) > 0) {
                        bool apply;

                        /* remove/update possible left-over symlinks from old database entry */
                        if (event->dev_db != NULL)
                                udev_node_update_old_links(dev->device, event->dev_db->device);

                        if (!event->owner_set)
                                event->uid = udev_device_get_devnode_uid(dev);

                        if (!event->group_set)
                                event->gid = udev_device_get_devnode_gid(dev);

                        if (!event->mode_set) {
                                if (udev_device_get_devnode_mode(dev) > 0) {
                                        /* kernel supplied value */
                                        event->mode = udev_device_get_devnode_mode(dev);
                                } else if (event->gid > 0) {
                                        /* default 0660 if a group is assigned */
                                        event->mode = 0660;
                                } else {
                                        /* default 0600 */
                                        event->mode = 0600;
                                }
                        }

                        apply = streq(udev_device_get_action(dev), "add") || event->owner_set || event->group_set || event->mode_set;
                        udev_node_add(dev->device, apply, event->mode, event->uid, event->gid, event->seclabel_list);
                }

                /* preserve old, or get new initialization timestamp */
                udev_device_ensure_usec_initialized(event->dev, event->dev_db);

                /* (re)write database file */
                udev_device_tag_index(dev, event->dev_db, true);
                udev_device_update_db(dev);
                udev_device_set_is_initialized(dev);

                event->dev_db = udev_device_unref(event->dev_db);
        }
}

void udev_event_execute_run(struct udev_event *event, usec_t timeout_usec, usec_t timeout_warn_usec) {
        const char *cmd;
        void *val;
        Iterator i;

        HASHMAP_FOREACH_KEY(val, cmd, event->run_list, i) {
                enum udev_builtin_cmd builtin_cmd = PTR_TO_INT(val);
                char command[UTIL_PATH_SIZE];

                udev_event_apply_format(event, cmd, command, sizeof(command), false);

                if (builtin_cmd >= 0 && builtin_cmd < _UDEV_BUILTIN_MAX)
                        udev_builtin_run(event->dev->device, builtin_cmd, command, false);
                else {
                        if (event->exec_delay > 0) {
                                log_debug("delay execution of '%s'", command);
                                sleep(event->exec_delay);
                        }

                        udev_event_spawn(event, timeout_usec, timeout_warn_usec, false, command, NULL, 0);
                }
        }
}
