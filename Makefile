CFLAGS = -O2 -Wall $(shell pkg-config --cflags libevdev raylib)
LIBS   = $(shell pkg-config --libs libevdev raylib) -lm

gcc-notch-ui: engine.c ui.c engine.h raygui.h
	gcc $(CFLAGS) -o $@ engine.c ui.c $(LIBS)

