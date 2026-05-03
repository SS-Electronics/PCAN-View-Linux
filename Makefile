# PCAN-View Linux – Makefile
#
# Build:  make
# Run:    ./pcan-view
# Clean:  make clean
# Install:make install
#
# Dependencies: libgtk-3-dev, libglib2.0-dev, can-utils (optional)
# See scripts/install_dependencies.sh

TARGET  := pcan-view

CC      := gcc
CFLAGS  := -Wall -Wextra -Wpedantic -std=c11 \
           -D_GNU_SOURCE \
           $(shell pkg-config --cflags gtk+-3.0)
LDFLAGS := $(shell pkg-config --libs gtk+-3.0) -lpthread

# Optimised release build by default; override with DEBUG=1
ifeq ($(DEBUG),1)
CFLAGS  += -g -O0 -DDEBUG
else
CFLAGS  += -O2
endif

# Source files
SRCS := main.c \
        driver/drv_can.c \
        driver/socketcan.c \
        gui/threads.c \
        gui/message_view.c \
        gui/main_window.c \
        gui/settings_dialog.c \
        gui/transmit_dialog.c

OBJS := $(SRCS:.c=.o)

# Include paths
CFLAGS += -Iinc

# ------------------------------------------------------------------ #

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "Build successful → $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)
	@echo "Installed to $(DESTDIR)/usr/local/bin/$(TARGET)"

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(TARGET)

# Rebuild if any header changes
$(OBJS): $(wildcard inc/*.h)
