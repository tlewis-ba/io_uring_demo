# Sample usage:
# ./write_io_uring foo 1024 8192 1024 0
# ./write_io_uring foo 1024 8192 1024 1

# Compiler and flags
CC = gcc
CFLAGS = -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes -Wredundant-decls -Wuninitialized -Wformat=2 -ggdb
LDFLAGS = -luring

# Define target and source file variables
TARGETS = write_io_uring
SOURCES = $(TARGETS:=.c)

# Build all targets
all: $(TARGETS)

# Generic rule to build each target from its corresponding source
%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Clean up build artifacts
clean:
	rm -f $(TARGETS) *.o foo

.PHONY: all clean

