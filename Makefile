CC	:= gcc
CFLAGS	:= -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -g2
LDFLAGS := -lrt

all: flashbench vm

flashbench: flashbench.o dev.o

vm: vm.o dev.o

clean:
	rm flashbench vm
