obj-m += monitor.o
CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -D_GNU_SOURCE -O2 -g
LDFLAGS = -lpthread

KDIR := /lib/modules/$(shell uname -r)/build
# no need for PWD

WORKLOAD_LDFLAGS ?= -static

USER_TARGETS := engine memory_hog cpu_hog io_pulse

.PHONY: all clean module

all: $(USER_TARGETS) module

# ---------------- ENGINE ----------------
engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o engine engine.c $(LDFLAGS)

# ---------------- WORKLOADS ----------------
memory_hog: memory_hog.c
	$(CC) -O2 -Wall $(WORKLOAD_LDFLAGS) -o memory_hog memory_hog.c

cpu_hog: cpu_hog.c
	$(CC) -O2 -Wall $(WORKLOAD_LDFLAGS) -o cpu_hog cpu_hog.c

io_pulse: io_pulse.c
	$(CC) -O2 -Wall $(WORKLOAD_LDFLAGS) -o io_pulse io_pulse.c

# ---------------- KERNEL MODULE ----------------
module: monitor.ko

monitor.ko: monitor.c monitor_ioctl.h
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

# ---------------- CLEAN ----------------
clean:
	-@sudo rm -rf logs 2>/dev/null || true
	-@rm -f $(USER_TARGETS)
	-@rm -f *.o *.mod *.mod.c *.symvers *.order
	-@rm -f *.log
	-@rm -f /tmp/mini_runtime.sock
