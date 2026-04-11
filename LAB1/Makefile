CC ?= cc
CXX ?= g++
# mac osx clang doesn't support -D_POSIX_C_SOURCE=200809L
CFLAGS = -Wall -Wextra -pedantic -std=c11 -D_POSIX_C_SOURCE=200809L
CXXFLAGS = -Wall -Wextra -pedantic -std=c++17

COMMAND_SRCS := $(wildcard commands/*.cpp)
COMMAND_BINS := $(patsubst commands/%.cpp,bin/%,$(COMMAND_SRCS))

ROOT := $(CURDIR)
PART1_DIR := $(ROOT)/project-1-demo-sample-grantyehh/part-1
PART2_DIR := $(ROOT)/project-1-demo-sample-grantyehh/part-2
PART2_PORT ?= 12345

.PHONY: all build test test1 test2 clean

all: build test

build: bin npshell np_simple $(COMMAND_BINS)

test: test1 test2

test1: build
	cd $(PART1_DIR) && ./demo.sh $(ROOT)/npshell

test2: build
	cd $(PART2_DIR) && ./demo.sh $(ROOT)/np_simple $(PART2_PORT)

npshell: npshell.c
	$(CC) $(CFLAGS) $< -o $@

np_simple: np_simple.c
	$(CC) $(CFLAGS) $< -o $@

bin:
	mkdir -p $@

bin/%: commands/%.cpp | bin
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f npshell np_simple $(COMMAND_BINS)
	rm -rf bin
