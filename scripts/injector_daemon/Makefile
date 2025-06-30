CC=gcc
CFLAGS=-std=c11 -Wall -Wextra -g
LDFLAGS=-lmicrohttpd -lpthread -lrt

TARGET=injector_daemon
SRCS=injector_daemon.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean