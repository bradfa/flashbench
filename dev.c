#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
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

#include <linux/fs.h>

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
		perror("time_read");
		return 0;
	}

	return get_ns() - now;
}

long long time_write(struct device *dev, off_t pos, size_t size, enum writebuf which)
{
	long long now = get_ns();
	ssize_t ret;
	unsigned long *p;

	if (size > MAX_BUFSIZE)
		return -ENOMEM;
	p = dev->writebuf[which];

	do {
		ret = pwrite(dev->fd, p, size, pos % dev->size);
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

long long time_erase(struct device *dev, off_t pos, size_t size)
{
	long long now = get_ns();
	ssize_t ret;
	unsigned long long args[2] = { size, pos % dev->size };

	if (size > MAX_BUFSIZE)
		return -ENOMEM;

	ret = ioctl(dev->fd, BLKDISCARD, &args);

	if (ret) {
		perror("time_erase");
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
	int err;
	void *p;
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
	if (err)
		return -err;

	err = posix_memalign(&p, 4096, MAX_BUFSIZE);
	if (err)
		return -err;
	memset(p, 0, MAX_BUFSIZE);
	dev->writebuf[WBUF_ZERO] = p;

	err = posix_memalign(&p,  4096, MAX_BUFSIZE);
	if (err)
		return -err;
	memset(p, 0xff, MAX_BUFSIZE);
	dev->writebuf[WBUF_ONE] = p;

	err = posix_memalign(&p , 4096, MAX_BUFSIZE);
	if (err)
		return -err;
	memset(p, 0x5a, MAX_BUFSIZE);
	dev->writebuf[WBUF_RAND] = p;

	return 0;
}
