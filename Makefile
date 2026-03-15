CC      = gcc
CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags gtk+-3.0)
LDFLAGS = $(shell pkg-config --libs gtk+-3.0)
TARGET  = kkueinput

$(TARGET): kkueinput.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean
