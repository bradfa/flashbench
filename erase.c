#define _GNU_SOURCE

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
	int fd = open(argv[1], O_RDWR | O_DIRECT);
	int ret;
	unsigned long long range[2];

	if (argc != 4) {
		fprintf(stderr, "usage: %s <device> <start> <length>\n",
			argv[0]);
	}

	if (fd < 0) {
		perror("open");
		return errno;
	}

	range[0] = atoll(argv[2]);
	range[1] = atoll(argv[3]);

	printf("erasing %lld to %lld on %s\n", range[0], range[0] + range[1], argv[1]);

	ret = ioctl(fd, BLKDISCARD, range);
	if (ret)
		perror("ioctl");

	return errno;
}
