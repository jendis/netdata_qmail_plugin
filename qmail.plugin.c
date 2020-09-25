/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "callbacks.h"
#include "err.h"
#include "flush.h"
#include "signal.h"
#include "timer.h"
#include "vector.h"

#include "fs.h"
#include "queue.h"
#include "send.h"
#include "smtp.h"
#include "ratelimitspp.h"

#define DEFAULT_PATH "/var/log/qmail"

enum poll {
	POLL_SIGNAL = 0,
	POLL_TIMER,
	POLL_FS_EVENT,
	POLL_LENGTH
};

#define LEN(x) ( sizeof x / sizeof * x )

static
void
usage(const char * name) {
	fprintf(stderr, "usage: %s <timout> [path]\n", name);
}

static
enum nd_err
prepare_watcher(struct fs_watch * watch, const int fd, const struct stat_func * func) {
	char file_name[PATH_MAX];

	watch->file_name = "current";
	watch->type = WATCH_LOG_FILE;
	sprintf(file_name, "%s/%s", watch->dir_name, watch->file_name);
	watch->watch_dir = inotify_add_watch(fd, watch->dir_name, IN_CREATE);
	if (watch->watch_dir == -1) {
		perror("inotify_add_watch");
		return ND_INOTIFY;
	}
	watch->fd = open(file_name, O_RDONLY);
	lseek(watch->fd, 0, SEEK_END);
	watch->func = func;
	watch->data = func->init();
	if (watch->data == NULL) {
		return ND_ALLOC;
	}

	return ND_SUCCESS;
}

static
enum nd_err
append_queue_watcher(struct vector * v) {
	struct fs_watch watch;

	memset(&watch, 0, sizeof watch);
	watch.type = WATCH_QUEUE;
	watch.watch_dir = -1;
	watch.fd = -1;
	watch.func = queue_func;
	watch.data = watch.func->init();

	if (watch.data == NULL) {
		return ND_ALLOC;
	}

	vector_add(v, &watch);

	return ND_SUCCESS;
}

static
void
detect_log_dirs(const int fd, struct vector * v) {
	struct dirent * dir_entry;
	const char * dir_name;
	struct fs_watch watch;
	DIR * dir;

	dir = opendir(".");
	if (dir == NULL) {
		perror("opendir");
		exit(1);
	}

	while ((dir_entry = readdir(dir))) {
		dir_name = dir_entry->d_name;

		if (dir_name[0] == '.')
			continue;

		if (is_directory(dir_name) == 1) {
			memset(&watch, 0, sizeof watch);
			if (strstr(dir_name, "send")) {
				fprintf(stderr, "send log directory detected: %s\n", dir_name);
				watch.dir_name = strdup(dir_name);

				if (prepare_watcher(&watch, fd, send_func) == ND_SUCCESS)
					vector_add(v, &watch);

			} else if (strstr(dir_name, "smtp")) {
				fprintf(stderr, "smtp log directory detected: %s\n", dir_name);
				watch.dir_name = strdup(dir_name);

				if (prepare_watcher(&watch, fd, smtp_func) == ND_SUCCESS)
					vector_add(v, &watch);

			}
		}
	}

	closedir(dir);
}

static
enum nd_err
append_ratelimit_aggregator(struct vector * v) {
	struct fs_watch_aggregator aggregator;

	memset(&aggregator, 0, sizeof aggregator);
	aggregator.type = WATCH_LOG_FILE;
	aggregator.func = ratelimitspp_func;
	aggregator.data = aggregator.func->init();

	if (aggregator.data == NULL) {
		return ND_ALLOC;
	}

	vector_add(v, &aggregator);

	return ND_SUCCESS;
}

int
main(int argc, const char * argv[]) {
	struct pollfd pfd[POLL_LENGTH];
	struct vector vector_watchers = VECTOR_EMPTY;
	struct vector vector_aggregators = VECTOR_EMPTY;
	unsigned long last_update;
	struct fs_watch * watch;
	struct fs_watch_aggregator * aggregator;
	const char * argv0;
	const char * path;
	int timeout = 1;
	int fs_event_fd;
	int signal_fd;
	int timer_fd;
	int run;
	int i;

	path = DEFAULT_PATH;
	argv0 = *argv; argv++; argc--;

	if (argc > 0) {
		timeout = atoi(*argv);
		argv++; argc--;
	} else
		usage(argv0);

	if (argc > 0) {
		path = *argv;
		argv++; argc--;
	}

	if (chdir(path) == -1) {
		fprintf(stderr, "Cannot change directory to '%s': %s\n", path, strerror(errno));
		exit(1);
	}

	vector_init(&vector_watchers, sizeof * watch);
	vector_init(&vector_aggregators, sizeof * aggregator);

	timer_fd = prepare_timer_fd(timeout);
	pfd[POLL_TIMER].fd = timer_fd;
	pfd[POLL_TIMER].events = POLLIN;

	signal_fd = prepare_signal_fd();
	pfd[POLL_SIGNAL].fd = signal_fd;
	pfd[POLL_SIGNAL].events = POLLIN;

	fs_event_fd = prepare_fs_event_fd();
	pfd[POLL_FS_EVENT].fd = fs_event_fd;
	pfd[POLL_FS_EVENT].events = POLLIN;

	detect_log_dirs(fs_event_fd, &vector_watchers);
	append_queue_watcher(&vector_watchers);

	if (vector_is_empty(&vector_watchers)) {
		fprintf(stderr, "Nothing to log for qmail\n");
		exit(1);
	}

	append_ratelimit_aggregator(&vector_aggregators);

	for (i = 0; i < vector_watchers.len; i++) {
		watch = vector_item(&vector_watchers, i);
		watch->func->print_hdr(watch->dir_name);
		clock_gettime(CLOCK_REALTIME, &watch->time);
	}

	for (i = 0; i < vector_aggregators.len; i++) {
		aggregator = vector_item(&vector_aggregators, i);
		aggregator->func->print_hdr();
		clock_gettime(CLOCK_REALTIME, &aggregator->time);
	}

	for (run = 1; run;) {
		switch (poll(pfd, LEN(pfd), -1)) {
		case -1:
			perror("poll");
			break;
		case 0:
			fputs("timeout\n", stderr);
			continue;
		default:
			if (pfd[POLL_SIGNAL].revents & POLLIN) {
				flush_read_fd(signal_fd);
				run = 0;
				continue;
			}
			if (pfd[POLL_FS_EVENT].revents & POLLIN) {
				process_fs_event_queue(fs_event_fd, vector_watchers.data, vector_watchers.len);
			}
			if (pfd[POLL_TIMER].revents & POLLIN) {
				flush_read_fd(timer_fd);
				for (i = 0; i < vector_watchers.len; i++) {
					watch = vector_item(&vector_watchers, i);

					if (watch->type == WATCH_LOG_FILE)
						read_log_file(watch);
					else if (watch->type == WATCH_QUEUE)
						watch->func->process(NULL, watch->data);

					for (i = 0; i < vector_aggregators.len; i++) {
						aggregator = vector_item(&vector_aggregators, i);
						if (aggregator->type == watch->type) {
							aggregator->func->aggregate(aggregator->data, watch->data);
						}
					}

					if (watch->func->postprocess)
						watch->func->postprocess(watch->data);

					last_update = update_timestamp(&watch->time);
					if (watch->func->print(watch->dir_name, watch->data, last_update)) {
						run = 0;
						fprintf(stderr, "Cannot write to stdout: %s\n", strerror(errno));
						break;
					}
					watch->func->clear(watch->data);
				}
				for (i = 0; i < vector_aggregators.len; i++) {
					aggregator = vector_item(&vector_aggregators, i);
					if (aggregator->func->postprocess)
						aggregator->func->postprocess(aggregator->data);

					last_update = update_timestamp(&aggregator->time);
                                        if (aggregator->func->print(aggregator->data, last_update)) {
                                                run = 0;
                                                fprintf(stderr, "Cannot write to stdout: %s\n", strerror(errno));
                                                break;
                                        }
                                        aggregator->func->clear(aggregator->data);
				}
			}
		}
	}

	for (i = 0; i < vector_watchers.len; i++) {
		watch = vector_item(&vector_watchers, i);
		free((void *)watch->dir_name);
		watch->func->fini(watch->data);
		close(watch->fd);
	}
        for (i = 0; i < vector_aggregators.len; i++) {
                aggregator = vector_item(&vector_aggregators, i);
                aggregator->func->fini(aggregator->data);
        }

	close(fs_event_fd);
	close(timer_fd);
	close(signal_fd);

	return 0;
}
