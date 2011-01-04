#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <string.h>
#include <getopt.h>
#include <stdbool.h>

#include "dev.h"

static inline long long time_to_ns(struct timespec *ts)
{
	return ts->tv_sec * 1000 * 1000 * 1000 + ts->tv_nsec;
}

static long long get_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return time_to_ns(&ts);
}

long long time_read(struct device *dev, off_t pos, size_t size)
{
	static char readbuf[64 * 1024 * 1024] __attribute__((aligned(4096)));
	long long now = get_ns();
	ssize_t ret;

	if (size > sizeof(readbuf))
		return -ENOMEM;

	do {
		ret = pread(dev->fd, readbuf, size, pos % dev->size);
		if (ret > 0) {
			size -= ret;
			pos += ret;
		}
	} while (ret > 0 || errno == -EAGAIN);

	if (ret)
		return -errno;

	return get_ns() - now;
}

long long time_write(struct device *dev, off_t pos, size_t size, enum writebuf which)
{
	static char writebuf[64 * 1024 * 1024] __attribute__((aligned(4096)));
	long long now = get_ns();
	ssize_t ret;

	if (size > sizeof(writebuf))
		return -ENOMEM;

	do {
		ret = pwrite(dev->fd, writebuf, size, pos % dev->size);
		if (ret > 0) {
			size -= ret;
			pos += ret;
		}
	} while (ret > 0 || errno == -EAGAIN);

	if (ret)
		return -errno;

	return get_ns() - now;
}

static void set_rtprio(void)
{
	int ret;
	struct sched_param p = {
		.sched_priority = 10,
	};
	ret = sched_setscheduler(0, SCHED_FIFO, &p);
	if (ret)
		perror("sched_setscheduler");
}


int setup_dev(struct device *dev, const char *filename)
{
	set_rtprio();

	dev->fd = open(filename, O_RDWR | O_DIRECT | O_SYNC | O_NOATIME);
	if (dev->fd < 0) {
		perror("open");
		return -errno;
	}

	dev->size = lseek(dev->fd, 0, SEEK_END);
	if (dev->size < 0) {
		perror("seek");
		return -errno;
	}

	return 0;
}



