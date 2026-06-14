CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wshadow -Wno-format-truncation -Wno-unused-parameter -pthread \
          $(shell pkg-config --cflags libevdev raylib libxml-2.0)
LIBS    = $(shell pkg-config --libs libevdev raylib libxml-2.0) -lm
PREFIX ?= /usr/local

BUILD = build
OBJ   = $(addprefix $(BUILD)/,engine.o ui.o skin.o livesplit.o)
BIN   = $(BUILD)/gcc-notch-ui

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LIBS)

$(BUILD)/engine.o:    engine.c engine.h
$(BUILD)/ui.o:        ui.c engine.h skin.h raygui.h livesplit.h
$(BUILD)/skin.o:      skin.c skin.h engine.h
$(BUILD)/livesplit.o: livesplit.c livesplit.h

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/gcc-notch-ui
	install -Dm644 gcc-notch-ui.desktop $(DESTDIR)$(PREFIX)/share/applications/gcc-notch-ui.desktop
	@if [ -z "$(DESTDIR)" ] && command -v update-desktop-database >/dev/null 2>&1; then \
		update-desktop-database $(PREFIX)/share/applications; \
	fi

.PHONY: clean install
