CC ?= cc
CXX ?= g++
# mac osx clang doesn't support -D_POSIX_C_SOURCE=200809L
CFLAGS = -Wall -Wextra -pedantic -std=c11
CXXFLAGS = -Wall -Wextra -pedantic -std=c++17

COMMAND_SRCS := $(wildcard commands/*.cpp)
COMMAND_BINS := $(patsubst commands/%.cpp,bin/%,$(COMMAND_SRCS))

ROOT := $(CURDIR)
PART1_DIR := $(ROOT)/project-1-demo-sample-grantyehh/part-1
PART2_DIR := $(ROOT)/project-1-demo-sample-grantyehh/part-2
PART2_PORT ?= 12345

.PHONY: default all build test test1 test2 clean

default: npshell np_simple

all: build test

build: npshell np_simple bin $(COMMAND_BINS)

test: build test1 test2

test1:
	cd $(PART1_DIR) && ./demo.sh $(ROOT)/npshell

test2:
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
