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

typedef unsigned long long ns_t;

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

static ns_t time_read(int fd, off_t pos, size_t size)
{
	static char readbuf[16 * 1024 * 1024];
	ns_t now = get_ns();
	ssize_t ret;

	do {
		ret = pread(fd, readbuf, size, pos);
		if (ret > 0) {
			size -= ret;
			pos += ret;
		}
	} while (ret > 0 || ret == -EAGAIN);

	return get_ns() - now;
}

static ns_t time_write(int fd, off_t pos, size_t size)
{
	static char writebuf[16 * 1024 * 1024];
	ns_t now = get_ns();
	ssize_t ret;

	do {
		ret = pwrite(fd, writebuf, size, pos);
		if (ret > 0) {
			size -= ret;
			pos += ret;
		}
	} while (ret > 0 || ret == -EAGAIN);

	return get_ns() - now;
}

int main(int argc, char **argv)
{
	int fd;
	off_t file_size;

	if (argc < 2) {
		fprintf(stderr, "%s: need arguments\n", argv[0]);
		return -EINVAL;
	}
	
	fd = open(argv[1], O_RDWR | O_DIRECT | O_SYNC | O_NOATIME);
	if (fd < 0) {
		perror("open");
		return -errno;
	}

	file_size = lseek(fd, 0, SEEK_END);
	if (file_size < 0) {
		perror("seek");
		return -errno;
	}

	printf("%lld\n", time_read(fd, 0, 4095));

	printf("filename: \"%s\"\n", argv[1]);
	printf("filesize: 0x%llx\n", (unsigned long long)file_size);

	return 0;
}
