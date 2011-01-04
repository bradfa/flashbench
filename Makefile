CC	:= gcc
CFLAGS	:= -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -g2
LDFLAGS := -lrt

all: flashbench

dev.o: dev.c dev.h
vm.o: vm.c vm.h dev.h
flashbench.o: flashbench.c vm.h dev.h

flashbench: flashbench.o dev.o vm.o

clean:
	rm flashbench flashbench.o dev.o vm.o
