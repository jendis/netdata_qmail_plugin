/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "callbacks.h"
#include "err.h"
#include "fs.h"

int
is_directory(const char * name) {
	struct stat st;
	int ret;

	ret = stat(name, &st);

	if (ret == -1)
		return ret;

	return S_ISDIR(st.st_mode);
}

int
prepare_fs_event_fd() {
	int fd;

	fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

	if (fd == -1) {
		perror("inotify_init1");
		exit(1);
	}

	return fd;
}

enum nd_err
read_log_file(struct fs_watch * watch) {
	ssize_t  max_line_length;
	const char * line;
	ssize_t ret;
	char * end;

	if (watch->fd == -1)
		return ND_FILE;

	while ((ret = read(watch->fd, watch->buf + watch->buffered, sizeof watch->buf - watch->buffered)) > 0) {
		line = watch->buf;
		ret += watch->buffered;
		watch->buffered = 0;
		for (;;) {
			max_line_length = ret - (line - watch->buf);
			end = memchr(line, '\n', max_line_length);
			if (end) {
				*end = '\0';

				if (watch->skip == DO_NOT_SKIP)
					watch->func->process(line, watch->data);
				else
					watch->skip = DO_NOT_SKIP;

				line = end + 1;
			} else {
				if (max_line_length > 0 && max_line_length < sizeof watch->buf) {
					watch->buffered = max_line_length;
					memmove(watch->buf, line, watch->buffered);
				} else if (max_line_length == sizeof watch->buf) {
					watch->buf[sizeof watch->buf - 1] = '\0';

					if (watch->skip == DO_NOT_SKIP)
						watch->func->process(line, watch->data);

					watch->skip = SKIP_THE_REST;
				}
				break;
			}
		}
	}

	return ND_SUCCESS;
}

static
void
reopen_log_file(struct fs_watch * watch) {
	char file_name[BUFSIZ];

	sprintf(file_name, "%s/%s", watch->dir_name, watch->file_name);
	close(watch->fd);
	watch->fd = open(file_name, O_RDONLY);
}

static
void
process_fs_event(const struct inotify_event * event, struct fs_watch * watchers, size_t watchers_length) {
	struct fs_watch * item;
	int i;

	for (i = 0; i < watchers_length; i++) {
		item = watchers + i;
		if (event->wd == item->watch_dir) {
			if (event->len) {
				if (!strcmp(event->name, item->file_name)) {
					read_log_file(item);
					reopen_log_file(item);
				}
			}
		}
	}
}

void
process_fs_event_queue(const int fd, struct fs_watch * watchers, size_t watchers_length) {
	const struct inotify_event * event;
	char buf[BUFSIZ];
	ssize_t len;
	char * ptr;

	for (;;) {
		len = read(fd, buf, sizeof buf);
		if (len == -1 && errno != EAGAIN) {
			perror("E: Cannot read fs_event fd");
			exit(1);
		}

		if (len <= 0)
			break;

		for (ptr = buf; ptr < buf + len; ptr += sizeof * event + event->len) {
			event = (const struct inotify_event *)ptr;
			process_fs_event(event, watchers, watchers_length);
		}
	}
}
