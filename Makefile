# CTTS - Concatenative Text-to-Speech Engine
# Makefile

CC = gcc
CFLAGS = -O3 -Wall -Wextra -std=c99 -pedantic
LDFLAGS = -lm

# Target executable
TARGET = ctts

# Source files
SRCS = ctts.c
OBJS = $(SRCS:.c=.o)

# Default target
all: $(TARGET)

# Link
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile
%.o: %.c ctts.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Build database
database: $(TARGET)
	./$(TARGET) build ./dataset voice.db

# Test synthesis
test: $(TARGET) database
	./$(TARGET) synth voice.db "hello world" test_output.wav 1.0
	@echo "Output written to test_output.wav"

# Clean
clean:
	rm -f $(TARGET) $(OBJS) voice.db test_output.wav

# Debug build
debug: CFLAGS = -g -O0 -Wall -Wextra -std=c99 -pedantic -DDEBUG
debug: clean $(TARGET)

# Install (to /usr/local/bin)
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

.PHONY: all clean debug database test install
