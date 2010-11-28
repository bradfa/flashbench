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

typedef long long ns_t;
struct device {
	int fd;
	ssize_t size;
};

static inline ns_t time_to_ns(struct timespec *ts)
{
	return ts->tv_sec * 1000 * 1000 * 1000 + ts->tv_nsec;
}

static ns_t get_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return time_to_ns(&ts);
}

static ns_t ns_min(int count, ns_t data[])
{
	int i;
	ns_t min = LLONG_MAX;

	for (i=0; i<count; i++) {
		if (data[i] < min)
			min = data[i];
	}

	return min;
}

static ns_t ns_max(int count, ns_t data[])
{
	int i;
	ns_t max = 0;

	for (i=0; i<count; i++) {
		if (data[i] > max)
			max = data[i];
	}

	return max;
}

static ns_t ns_avg(int count, ns_t data[])
{
	int i;
	ns_t sum = 0;

	for (i=0; i<count; i++) {
		sum += data[i];
	}

	return sum / i;
}

static void format_ns(char *out, ns_t ns)
{
	if (ns < 1000)
		snprintf(out, 8, "%lldns", ns);
	else if (ns < 1000 * 1000)
		snprintf(out, 8, "%.3gµs", ns / 1000.0);
	else if (ns < 1000 * 1000 * 1000)
		snprintf(out, 8, "%.3gms", ns / 1000000.0);
	else {
		snprintf(out, 8, "%.4gs", ns / 1000000000.0);
	}
}

static void print_ns(ns_t ns)
{
	char buf[8];
	format_ns(buf, ns);
	puts(buf);
}

static ns_t time_read(struct device *dev, off_t pos, size_t size)
{
	static char readbuf[64 * 1024 * 1024] __attribute__((aligned(4096)));
	ns_t now = get_ns();
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

static ns_t time_write(struct device *dev, off_t pos, size_t size)
{
	static char writebuf[64 * 1024 * 1024] __attribute__((aligned(4096)));
	ns_t now = get_ns();
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

static int time_read_interval(struct device *dev, int count, size_t size, ns_t results[])
{
	int i;
	off_t pos;

	for (i=0; i < count; i++) {
		pos = i * size * 2; /* every other block */
		results[i] = time_read(dev, pos, size);
		if (results[i] < 0)
			return results[i];
	}

	return 0;
}

static int time_read_linear(struct device *dev, int count, size_t size, ns_t results[])
{
	int i;
	off_t pos;

	for (i=0; i < count; i++) {
		pos = (count - i) * size; /* every other block */
		results[i] = time_read(dev, pos, size);
		if (results[i] < 0)
			return results[i];
	}

	return 0;
}

static int try_read_cache(struct device *dev)
{
	const int rounds = 18;
	const int tries = 8;
	ns_t times[rounds];
	int i;

	for (i = 0; i < rounds; i++) {
		long blocksize = 512l << i;
		char min[8];
		int j;

		times[i] = LLONG_MAX;

		for (j = 0; j < tries; j++) {
			ns_t ns, max;
			ns = time_read(dev, blocksize, blocksize);

			if (ns < 0)
				return ns;

			if (ns < times[i])
				times[i] = ns;
		}

		format_ns(min, times[i]);

		printf("%ld bytes: %s, %g MB/s\n", blocksize, min, 
					blocksize / (times[i] / 1000.0));
	}

	return 0;
}

static int try_intervals(struct device *dev)
{
	const int count = 128;
	const off_t rounds = 12;
	ns_t times[count];
	int i;

	for (i=0; i<rounds; i++) {
		char min[8], avg[8], max[8];
		long blocksize = 512l << i;
		int ret;

		ret = time_read_interval(dev, count, blocksize, times);
		if (ret < 0)
			return ret;

		format_ns(min, ns_min(count, times));
		format_ns(avg, ns_avg(count, times));
		format_ns(max, ns_max(count, times));

		printf("%ld bytes: min %s avg %s max %s: %g MB/s\n", blocksize,
			 min, avg, max, blocksize / (ns_min(count, times) / 1000.0));

		ret = time_read_linear(dev, count, blocksize, times);
		if (ret < 0)
			return ret;

		format_ns(min, ns_min(count, times));
		format_ns(avg, ns_avg(count, times));
		format_ns(max, ns_max(count, times));

		printf("%ld bytes: min %s avg %s max %s: %g MB/s\n", blocksize,
			 min, avg, max, blocksize / (ns_min(count, times) / 1000.0));
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct device dev;

	if (argc < 2) {
		fprintf(stderr, "%s: need arguments\n", argv[0]);
		return -EINVAL;
	}
	
	dev.fd = open(argv[1], O_RDWR | O_DIRECT | O_SYNC | O_NOATIME);
	if (dev.fd < 0) {
		perror("open");
		return -errno;
	}

	dev.size = lseek(dev.fd, 0, SEEK_END);
	if (dev.size < 0) {
		perror("seek");
		return -errno;
	}

	printf("filename: \"%s\"\n", argv[1]);
	printf("filesize: 0x%llx\n", (unsigned long long)dev.size);

	{
		int ret;

		ret = try_read_cache(&dev);
		if (ret < 0) {
			errno = -ret;
			perror("try_read_cache");
			return ret;
		}

		ret = try_intervals(&dev);
		if (ret < 0) {
			errno = -ret;
			perror("try_intervals");
			return ret;
		}

	}

	return 0;
}
