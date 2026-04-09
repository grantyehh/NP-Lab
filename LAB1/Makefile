CC = clang
CFLAGS = -Wall -Wextra -pedantic -std=c11

ROOT := $(CURDIR)
PART1_DIR := $(ROOT)/project-1-demo-sample-grantyehh/part-1
PART2_DIR := $(ROOT)/project-1-demo-sample-grantyehh/part-2
PART2_PORT ?= 12345

.PHONY: all build test test1 test2 clean

all: build test

build: npshell np_simple

test: test1 test2

test1: build
	cd $(PART1_DIR) && ./demo.sh $(ROOT)/npshell

test2: build
	cd $(PART2_DIR) && ./demo.sh $(ROOT)/np_simple $(PART2_PORT)

npshell: npshell.c
	$(CC) $(CFLAGS) $< -o $@

np_simple: np_simple.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f npshell np_simple
