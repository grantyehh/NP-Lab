CC = clang
CFLAGS = -Wall -Wextra -pedantic -std=c11

.PHONY: all clean

all: npshell np_simple

npshell: npshell.c
	$(CC) $(CFLAGS) $< -o $@

np_simple: np_simple.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f npshell np_simple
