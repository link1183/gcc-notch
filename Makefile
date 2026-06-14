CFLAGS = -O2 -Wall -Wno-format-truncation $(shell pkg-config --cflags libevdev raylib libxml-2.0)
LIBS   = $(shell pkg-config --libs libevdev raylib libxml-2.0) -lm

gcc-notch-ui: engine.c ui.c skin.c engine.h skin.h raygui.h
	gcc $(CFLAGS) -o $@ engine.c ui.c skin.c $(LIBS)

