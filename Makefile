CC	:= gcc
CFLAGS	:= -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -g2
LDFLAGS := -lrt

all: flashbench vm

flashbench: flashbench.c

vm: vm.c

clean:
	rm flashbench vm
