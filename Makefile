CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -pthread

.PHONY: run test clean

build/nexcent: server/server.c
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $< -lsqlite3 -lpthread

run: build/nexcent
	./build/nexcent

test: build/nexcent
	sh tests/api_test.sh

clean:
	rm -rf build
