# Builds the mini C compiler into <repo>/build/minic.
CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O1 -g
BUILD   := ../../build
OBJDIR  := $(BUILD)/minic-obj
SRCS    := $(wildcard src/*.c)
OBJS    := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRCS))

.PHONY: all test clean

all: $(BUILD)/minic

$(BUILD)/minic: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(OBJDIR)/%.o: src/%.c include/minic.h | $(OBJDIR)
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

test: all
	bash tests/run_tests.sh

clean:
	rm -rf $(OBJDIR) $(BUILD)/minic
