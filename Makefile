CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wshadow -Wno-format-truncation -Wno-unused-parameter -pthread \
          $(shell pkg-config --cflags libevdev raylib libxml-2.0)
LIBS    = $(shell pkg-config --libs libevdev raylib libxml-2.0) -lm
PREFIX ?= /usr/local

OBJ = engine.o ui.o skin.o
BIN = gcc-notch-ui

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LIBS)

engine.o: engine.c engine.h
ui.o:     ui.c engine.h skin.h raygui.h
skin.o:   skin.c skin.h engine.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

.PHONY: clean install
