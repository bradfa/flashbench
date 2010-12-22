CC	:= gcc
CFLAGS	:= -O2 -Wall -Wextra -g2
LDFLAGS := -lrt

flashbench: flashbench.c

clean:
	rm flashbench
