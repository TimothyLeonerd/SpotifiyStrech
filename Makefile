CC ?= cc
CXX ?= c++
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -O2
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)
PULSE_CFLAGS := $(shell pkg-config --cflags libpulse-simple libpulse)
PULSE_LIBS := $(shell pkg-config --libs libpulse-simple libpulse)
RUBBERBAND_DIR := third_party/rubberband
RUBBERBAND_BUILD := $(RUBBERBAND_DIR)/build
RUBBERBAND_CFLAGS := -I$(RUBBERBAND_DIR)
RUBBERBAND_LIB := $(RUBBERBAND_BUILD)/librubberband.a

TARGET := spotify-recorder

all: $(TARGET)

$(RUBBERBAND_BUILD)/build.ninja: $(RUBBERBAND_DIR)/meson.build $(RUBBERBAND_DIR)/meson_options.txt
	meson setup "$(RUBBERBAND_BUILD)" "$(RUBBERBAND_DIR)" -Ddefault_library=static -Dfft=builtin -Dresampler=builtin -Dtests=disabled -Dcmdline=disabled -Dladspa=disabled -Dlv2=disabled -Dvamp=disabled -Djni=disabled

$(RUBBERBAND_LIB): $(RUBBERBAND_BUILD)/build.ninja
	meson compile -C "$(RUBBERBAND_BUILD)"

core.o: core.c core.h
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(PULSE_CFLAGS) -c -o $@ $<

app.o: app.c core.h $(RUBBERBAND_LIB)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(PULSE_CFLAGS) $(RUBBERBAND_CFLAGS) -c -o $@ $<

$(TARGET): core.o app.o $(RUBBERBAND_LIB)
	$(CXX) -o $@ core.o app.o $(RUBBERBAND_LIB) $(GTK_LIBS) $(PULSE_LIBS) -lm -pthread

clean:
	rm -f $(TARGET) *.o *.wav
	rm -rf $(RUBBERBAND_BUILD)

.PHONY: all clean
