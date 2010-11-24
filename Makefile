CC	:= gcc
CFLAGS	:= -O2 -Wall -Wextra
LDFLAGS := -lrt

flashbench: flashbench.c

clean:
	rm flashbench
