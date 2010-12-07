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

typedef long long ns_t;
struct device {
	int fd;
	ssize_t size;
};

#define returnif(x) do { int __x = (x); if (__x < 0) return (__x); } while (0)

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

static inline void print_ns(ns_t ns)
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

#if 0
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
#endif

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
		returnif (results[i] < 0);
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
		returnif (results[i] < 0);
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
		returnif (results[i] < 0);
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

			returnif (ns);

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
	returnif (ret);

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
		returnif (ret);

		aligned[i] = ns_min(count, times);
		avg_a = ns_avg(count, times);

		ret = time_read_interval_unaligned(dev, count, blocksize, times);
		returnif (ret);

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
#if 0
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
#endif

/*
 * Linear feedback shift register
 *
 * We use this to randomize the block positions for random-access
 * tests. Unlike real random data, we know that within 2^bits
 * accesses, every possible value up to 2^bits will be seen
 * exactly once, with the exception of zero, for which we have
 * a special treatment.
 */
static int lfsr(unsigned short v, unsigned int bits)
{
	unsigned short bit;

	if (v >= (1 << bits)) {
		fprintf(stderr, "flashbench: internal error\n");
		exit(-EINVAL);
	}

	if (v == 0)
		v = ((1 << bits) - 1) & 0xace1;

	switch (bits) {
	case 12:
		bit = ((v >> 0) ^ (v >> 1) ^ (v >> 2) ^ (v >> 8) ) & 1;
		break;
	case 13: /* x^13 + x^12 + x^11 + x^8 + 1 */
		bit = ((v >> 0) ^ (v >> 1) ^ (v >> 2) ^ (v >> 5) ) & 1;
		break;
	case 14: /* x^14 + x^13 + x^12 + x^2 + 1 */
		bit = ((v >> 0) ^ (v >> 1) ^ (v >> 2) ^ (v >> 12) ) & 1;
		break;
	case 15: /* x^15 + x^14 + 1 */
		bit = ((v >> 0) ^ (v >> 1) ) & 1;
		break;
	case 16: /* x^16 + x^14 + x^13 + x^11 + 1 */
		bit = ((v >> 0) ^ (v >> 2) ^ (v >> 3) ^ (v >> 5) ) & 1;
		break;
	default:
		fprintf(stderr, "flashbench: internal error\n");
		exit(-EINVAL);
	}

	return v >> 1 | bit << (bits - 1);
}

static int try_scatter_io(struct device *dev, int tries, int scatter_order, int blocksize, FILE *out)
{
	int i, j;
	const int count = 1 << scatter_order;
	ns_t time;
	ns_t min[count];
	unsigned long pos;

	memset(min, 0, sizeof(min));
	for (i = 0; i < tries; i++) {
		pos = 0;
		for (j = 0; j < count; j++) {
			pos = lfsr(pos, scatter_order);
			time = time_read(dev, (pos * blocksize), 2 * blocksize);
			returnif (time);

			if (i == 0 || time < min[pos])
				min[pos] = time;
		}
	}

	for (j = 0; j < count; j++) {
		fprintf(out, "%d	%lld\n", j * blocksize, min[j]);
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

static void print_help(const char *name)
{
	printf("%s [OPTION]... [DEVICE]\n", name);
	printf("run tests on DEVICE, pointing to a flash storage medium.\n\n");
	printf("-o, --out=FILE	write output to FILE instead of stdout\n");
	printf("-s, --scatter	run scatter read test\n");
	printf("    --scatter-order=N scatter across 2^N blocks\n");
	printf("-r, --rcache	determine read cache size\n");
	printf("-v, --verbose	increase verbosity of output\n");
	printf("-c, --count=N	run each test N times (default: 8\n");	
}

struct arguments {
	const char *dev;
	const char *out;
	bool scatter, rcache, align, interval;
	int verbosity;
	int count;
	int blocksize;
	int scatter_order;
};

static int parse_arguments(int argc, char **argv, struct arguments *args)
{
	static const struct option long_options[] = {
		{ "out", 1, NULL, 'o' },
		{ "scatter", 0, NULL, 's' },
		{ "scatter-order", 1, NULL, 'S' },
		{ "rcache", 0, NULL, 'r' },
		{ "align", 0, NULL, 'a' },
		{ "interval", 0, NULL, 'i' },
		{ "verbose", 0, NULL, 'v' },
		{ "count", 1, NULL, 'c' },
		{ NULL, 0, NULL, 0 },
	};

	memset(args, 0, sizeof(*args));
	args->count = 8;
	args->scatter_order = 14;
	args->blocksize = 8192;

	while (1) {
		int c;

		c = getopt_long(argc, argv, "o:srai", long_options, &optind);

		if (c == -1)
			break;

		switch (c) {
		case 'o':
			args->out = optarg;
			break;

		case 's':
			args->scatter = 1;
			break;

		case 'S':
			args->scatter_order = atoi(optarg);
			break;

		case 'r':
			args->rcache = 1;
			break;

		case 'a':
			args->align = 1;
			break;

		case 'i':
			args->interval = 1;
			break;

		case 'v':
			args->verbosity++;
			break;

		case 'c':
			args->count = atoi(optarg);
			break;

		case '?':
			print_help(argv[0]);
			return -EINVAL;
			break;
		}
	}

	if (optind != (argc - 1))  {
		fprintf(stderr, "%s: invalid arguments\n", argv[0]);
		return -EINVAL;
	}

	args->dev = argv[optind];

	if (!(args->scatter || args->rcache ||
	      args->align || args->interval)) {
		fprintf(stderr, "%s: need at least one action\n", argv[0]);
		return -EINVAL;
	}

	if (args->scatter && (args->scatter_order > 16)) {
		fprintf(stderr, "%s: scatter_order must be at most 16\n", argv[0]);
		return -EINVAL;
	}

	return 0;
}

static int setup_dev(struct device *dev, struct arguments *args)
{
	try_set_rtprio();

	dev->fd = open(args->dev, O_RDWR | O_DIRECT | O_SYNC | O_NOATIME);
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

static FILE *open_output(const char *filename)
{
	if (!filename || !strcmp(filename, "-"))
		return fdopen(0, "w"); /* write to stdout */

	return fopen(filename, "w+");
}

int main(int argc, char **argv)
{
	struct device dev;
	struct arguments args;
	FILE *output;
	int ret;

	returnif(parse_arguments(argc, argv, &args));

	returnif(setup_dev(&dev, &args));

	output = open_output(args.out);
	if (!output) {
		perror(args.out);
		return -errno;
	}

	if (args.verbosity) {
		printf("filename: \"%s\"\n", argv[1]);
		printf("filesize: 0x%llx\n", (unsigned long long)dev.size);
	}

	if (args.scatter) {
		ret = try_scatter_io(&dev, args.count, args.scatter_order, args.blocksize, output);
		if (ret < 0) {
			errno = -ret;
			perror("try_scatter_io");
			return ret;
		}
	}

	if (args.align) {
		ret = try_align(&dev);
		if (ret < 0) {
			errno = -ret;
			perror("try_align");
			return ret;
		}
	}

	if (args.rcache) {
		ret = try_read_cache(&dev);
		if (ret < 0) {
			errno = -ret;
			perror("try_read_cache");
			return ret;
		}
	}

	if (args.interval) {
		ret = try_intervals(&dev);
		if (ret < 0) {
			errno = -ret;
			perror("try_intervals");
			return ret;
		}
	}

	return 0;
}
