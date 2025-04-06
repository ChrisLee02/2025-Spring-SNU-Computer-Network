CC = gcc
CFLAGS = -Wall -Wextra -O2

TARGETS = test_simple test_persistent

all: $(TARGETS)

test_simple: test_simple.c
	$(CC) $(CFLAGS) -o $@ $<

test_persistent: test_persistent.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)
