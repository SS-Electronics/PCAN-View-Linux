# PCAN-View Linux – Makefile
#
# Build:   make               → build/pcan-view  (release)
#          make DEBUG=1       → build/pcan-view  (debug)
# Run:     ./build/pcan-view
# Clean:   make clean         → removes build/ entirely
# Install: make install

# ------------------------------------------------------------------ #
# Directories                                                          #
# ------------------------------------------------------------------ #
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj

# ------------------------------------------------------------------ #
# Toolchain & flags                                                    #
# ------------------------------------------------------------------ #
TARGET  := $(BUILD_DIR)/pcan-view

CC      := gcc
CFLAGS  := -Wall -Wextra -Wpedantic -std=c11 \
           -D_GNU_SOURCE \
           $(shell pkg-config --cflags gtk+-3.0) \
           -Iinc

LDFLAGS := $(shell pkg-config --libs gtk+-3.0) -lpthread

ifeq ($(DEBUG),1)
CFLAGS  += -g -O0 -DDEBUG
else
CFLAGS  += -O2
endif

# ------------------------------------------------------------------ #
# Sources → objects (mirror source tree under build/obj/)             #
# ------------------------------------------------------------------ #
SRCS := main.c \
        driver/drv_can.c \
        driver/socketcan.c \
        gui/threads.c \
        gui/message_view.c \
        gui/main_window.c \
        gui/settings_dialog.c \
        gui/transmit_dialog.c

OBJS := $(patsubst %.c, $(OBJ_DIR)/%.o, $(SRCS))

# Subdirectories that must exist before compiling
OBJ_SUBDIRS := $(OBJ_DIR) \
               $(OBJ_DIR)/driver \
               $(OBJ_DIR)/gui

# ------------------------------------------------------------------ #
# Rules                                                                #
# ------------------------------------------------------------------ #
.PHONY: all clean install uninstall run

all: $(TARGET)

# Link
$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "Build successful → $@"

# Compile each source into build/obj/<path>.o
$(OBJ_DIR)/%.o: %.c | $(OBJ_SUBDIRS)
	$(CC) $(CFLAGS) -c $< -o $@

# Create output directories on demand
$(BUILD_DIR) $(OBJ_SUBDIRS):
	mkdir -p $@

# Rebuild when any header changes
$(OBJS): $(wildcard inc/*.h)

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned build directory."

run: all
	@./$(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/pcan-view
	@echo "Installed to $(DESTDIR)/usr/local/bin/pcan-view"

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/pcan-view
	@echo "Uninstalled pcan-view."
