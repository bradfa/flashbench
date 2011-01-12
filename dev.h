#ifndef FLASHBENCH_DEV_H
#define FLASHBENCH_DEV_H

#include <unistd.h>

struct device {
	void *readbuf;
	void *writebuf[3];
	int fd;
	off_t size;
};

enum writebuf {
	WBUF_ZERO,
	WBUF_ONE,
	WBUF_RAND,
};

extern int setup_dev(struct device *dev, const char *filename);

long long time_write(struct device *dev, off_t pos, size_t size, enum writebuf which);

long long time_read(struct device *dev, off_t pos, size_t size);

long long time_erase(struct device *dev, off_t pos, size_t size);

#endif /* FLASHBENCH_DEV_H */
