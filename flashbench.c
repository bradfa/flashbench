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

static void regression(ns_t ns[], off_t bytes[], int count, ns_t *atime, float *throughput)
{
	int i;
	float sum_x = 0, sum_xx = 0;
	float sum_y = 0, sum_xy = 0;
	float slope, intercept;
	char buf[8];

	for (i = 0; i < count; i++) {
		sum_x	+= (float)bytes[i];
		sum_xx	+= (float)bytes[i] * (float)bytes[i];
		sum_y	+= (float)ns[i];
		sum_xy	+= (float)ns[i] * (float)bytes[i];
	}

	/* standard linear regression method */
	slope = (float)(count * sum_xy - sum_x * sum_y) /
		(float)(count * sum_xx - sum_x * sum_x);
	intercept = (sum_y - slope * sum_x) / count;

	format_ns(buf, intercept);
	printf("%g MB/s, %s access time\n", 1000.0 / slope, buf);

	*atime = intercept;
	*throughput = 1000.0 / slope;
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

static void flush_read_cache(struct device *dev)
{
	off_t cache_size = 1024 * 1024; /* FIXME: use detected size */

	time_read(dev, 2 * cache_size, dev->size - 2 * cache_size);
}

static int time_read_interval(struct device *dev, int count, size_t size, ns_t results[])
{
	int i;
	off_t pos;

	for (i=0; i < count; i++) {
		pos = i * size * 8; /* every other block */
		results[i] = time_read(dev, pos, size);
		if (results[i] < 0)
			return results[i];
	}

	return 0;
}

static int time_read_interval_unaligned(struct device *dev, int count, size_t size, ns_t results[])
{
	int i;
	off_t pos;

	for (i=0; i < count; i++) {
		pos = i * size * 8 + size / 2; /* every other block, half a block off */
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
			ns_t ns;
			ns = time_read(dev, 1024 * 1024 * 1024, blocksize);

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

static void print_one_blocksize(int count, ns_t *times, off_t blocksize)
{
	char min[8], avg[8], max[8];

	format_ns(min, ns_min(count, times));
	format_ns(avg, ns_avg(count, times));
	format_ns(max, ns_max(count, times));

	printf("%ld bytes: min %s avg %s max %s: %g MB/s\n", blocksize,
		 min, avg, max, blocksize / (ns_min(count, times) / 1000.0));
}

static int try_interval(struct device *dev, long blocksize, ns_t *min_time, int count)
{
	int ret;
	ns_t times[count];

	ret = time_read_linear(dev, count, blocksize, times);
	if (ret < 0)
		return ret;

	print_one_blocksize(count, times, blocksize);
	*min_time = ns_min(count, times);

	return 0;
}

static int try_intervals(struct device *dev)
{
	const int count = 32;
	const off_t rounds = 11;
	const int ignore = 3;
	ns_t min[rounds];
	off_t bytes[rounds];
	ns_t atime;
	float throughput;
	int i;

	for (i=0; i<rounds; i++) {
		bytes[i] = 512l << i;
		try_interval(dev, bytes[i], &min[i], count);

	}

	regression(min + ignore, bytes + ignore, rounds - ignore, &atime, &throughput);

	for (i=0; i<rounds; i++) {
		printf("bytes %lld, time %lld overhead %g\n", (long long)bytes[i], min[i],
			min[i] - atime - bytes[i] * 1000 / throughput);
	}

	return 0;
}

static int try_align(struct device *dev)
{
	const int rounds = 11;
	const int count = 16;
	int i;

	ns_t aligned[rounds];
	ns_t unaligned[rounds];

	for (i=0; i<rounds; i++) {
		off_t blocksize = 512l << (i + 1);
		ns_t times[count];
		char buf_a[8], buf_u[8];
		ns_t avg_a, avg_u;
		int ret;

		flush_read_cache(dev);

		ret = time_read_interval(dev, count, blocksize, times);
		if (ret < 0)
			return ret;

		aligned[i] = ns_min(count, times);
		avg_a = ns_avg(count, times);

		ret = time_read_interval_unaligned(dev, count, blocksize, times);
		if (ret < 0)
			return ret;

		unaligned[i] = ns_min(count, times);
		avg_u = ns_avg(count, times);

		format_ns(buf_a, aligned[i]);
		format_ns(buf_u, unaligned[i]);

		printf("%ld bytes: aligned %s unaligned %s diff %lld, %02g%% min %02g%% avg\n",
			blocksize, buf_a, buf_u, unaligned[i] - aligned[i],
			 100.0 * (unaligned[i] - aligned[i]) / aligned[i],
			 100.0 * (avg_u - avg_a) / avg_a);
	}

	return 0;
}

static int lfsr12(unsigned short v)
{
	unsigned short bit = ((v >> 0) ^ (v >> 1) ^ (v >> 2) ^ (v >> 8) ) & 1;
	return v >> 1 | bit << 11;
}

static int lfsr14(unsigned short v)
{
	unsigned short bit = ((v >> 0) ^ (v >> 1) ^ (v >> 2) ^ (v >> 12) ) & 1;
	return v >> 1 | bit << 13;
}

static int lfsr16(unsigned short v)
{
	unsigned short bit = ((v >> 0) ^ (v >> 2) ^ (v >> 3) ^ (v >> 5) ) & 1;
	return v >> 1 | bit << 15;
}

static int try_scatter_io(struct device *dev)
{
	int i, j;
	const int count = 16384;
	const int tries = 4;
	ns_t time;
	ns_t min[count];
	unsigned long pos;

	memset(min, 0, sizeof(min));
	for (i = 0; i < tries; i++) {
		pos = 0xce1;
		for (j = 0; j < count; j++) {
			pos = lfsr14(pos);
			time = time_read(dev, (pos * 4096), 8192);
			if (time < 0)
				return time;

			if (i == 0 || time < min[pos])
				min[pos] = time;
		}
	}

	for (j = 0; j < count; j++) {
		printf("%ld	%lld\n", j * 4096, min[j]);
	}

	return 0;
}

static void try_set_rtprio(void)
{
	int ret;
	struct sched_param p = {
		.sched_priority = 10,
	};
	ret = sched_setscheduler(0, SCHED_FIFO, &p);
	if (ret)
		perror("sched_setscheduler");
}

int main(int argc, char **argv)
{
	struct device dev;

	try_set_rtprio();

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
/*
	printf("filename: \"%s\"\n", argv[1]);
	printf("filesize: 0x%llx\n", (unsigned long long)dev.size);
*/
	{
		int ret;

		ret = try_scatter_io(&dev);
		if (ret < 0) {
			errno = -ret;
			perror("try_scatter_io");
			return ret;
		}
#if 0
		ret = try_align(&dev);
		if (ret < 0) {
			errno = -ret;
			perror("try_align");
			return ret;
		}

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
#endif
	}

	return 0;
}
