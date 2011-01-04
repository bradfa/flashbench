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

#define MAX_BUFSIZE (64 * 1024 * 1024)

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
	long long now = get_ns();
	ssize_t ret;

	if (size > MAX_BUFSIZE)
		return -ENOMEM;

	do {
		ret = pread(dev->fd, dev->readbuf, size, pos % dev->size);
		if (ret > 0) {
			size -= ret;
			pos += ret;
		}
	} while (ret > 0 || errno == -EAGAIN);

	if (ret) {
		fprintf(stderr, "fd %d buf %p size %ld pos %ld\n", dev->fd, dev->readbuf, size, pos % dev->size);
		perror("time_read");
		return 0;
	}

	return get_ns() - now;
}

long long time_write(struct device *dev, off_t pos, size_t size, enum writebuf which)
{
	long long now = get_ns();
	ssize_t ret;

	if (size > MAX_BUFSIZE)
		return -ENOMEM;

	do {
		ret = pwrite(dev->fd, dev->writebuf[which], size, pos % dev->size);
		if (ret > 0) {
			size -= ret;
			pos += ret;
		}
	} while (ret > 0 || errno == -EAGAIN);

	if (ret) {
		perror("time_write");
		return 0;
	}

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
	int i, err;
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

	err = posix_memalign(&dev->readbuf,		4096, MAX_BUFSIZE);
	if (err) return -err;
	err = posix_memalign(&dev->writebuf[WBUF_ZERO], 4096, MAX_BUFSIZE);
	if (err) return -err;
	err = posix_memalign(&dev->writebuf[WBUF_ONE],  4096, MAX_BUFSIZE);
	if (err) return -err;
	err = posix_memalign(&dev->writebuf[WBUF_RAND], 4096, MAX_BUFSIZE);
	if (err) return -err;

	memset(dev->writebuf[WBUF_ZERO], 0, MAX_BUFSIZE);
	memset(dev->writebuf[WBUF_ONE], 0xff, MAX_BUFSIZE);
	for (i = 0; i < MAX_BUFSIZE; i+=256)
		memset(dev->writebuf[WBUF_RAND] + i, (i & 0xff), 256);

	return 0;
}
