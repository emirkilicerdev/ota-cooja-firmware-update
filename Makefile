CONTIKI_PROJECT = udp-client udp-server
PROJECT_SOURCEFILES += ota-metadata.c
all: $(CONTIKI_PROJECT)

# msp430-gcc 4.7.4 static inline sorunu icin
CFLAGS += -Os

CONTIKI=../..
include $(CONTIKI)/Makefile.include
