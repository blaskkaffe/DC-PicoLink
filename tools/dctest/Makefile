# KOS build: source the KOS environment (environ.sh), then `make`; run with
#   dc-tool-ser -t <port> -b 115200 -x dctest.elf   (then quit dc-tool before starting dctest.py)
TARGET = dctest.elf
OBJS = dctest.o

DT_BUILD ?= $(shell date +'%b %d %Y %H:%M:%S')
KOS_CFLAGS += -DDT_BUILD='"$(DT_BUILD)"'

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS)

clean:
	-rm -f $(TARGET) $(OBJS)

# host build for the selftest (no KOS needed)
dctest-host: dctest.c proto.h
	gcc -O1 -Wall -DDCTEST_HOST dctest.c -o $@
