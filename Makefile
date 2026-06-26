# PCAN-View Linux – Makefile
#
# Build:   make               → build/pcan-view  (release)
#          make DEBUG=1       → build/pcan-view  (debug)
# Run:     ./build/pcan-view
# Clean:   make clean         → removes build/ entirely
#
# One-shot install (dependencies + compile + system install + desktop entry):
#          sudo make install
# Only install build/runtime dependencies:
#          sudo make install-deps
# Remove an installed copy:
#          sudo make uninstall

# ------------------------------------------------------------------ #
# Install layout (override with PREFIX=… DESTDIR=…)                    #
# ------------------------------------------------------------------ #
PREFIX  ?= /usr/local
DESTDIR ?=
BINDIR   := $(PREFIX)/bin
SHAREDIR := $(PREFIX)/share/pcan-view
APPDIR   := $(PREFIX)/share/applications
ICONDIR  := $(PREFIX)/share/icons/hicolor/256x256/apps

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
.PHONY: all clean install install-deps install-files uninstall run

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

# Install system build/runtime dependencies (auto-detects apt/dnf/pacman).
install-deps:
	@echo "==> Installing dependencies (requires root)…"
	@bash scripts/install_dependencies.sh

# Full end-user install: dependencies → compile → install binary + desktop entry
# + icon.  Run as root:  sudo make install
install: install-deps all install-files
	@echo ""
	@echo "==> PCAN-View Linux installed successfully."
	@echo "    Launch it from your application menu or run: pcan-view"

# Copy the built binary and application resources into the system.
install-files: $(TARGET)
	@echo "==> Installing files to $(DESTDIR)$(PREFIX)…"
	install -Dm755 $(TARGET)                  $(DESTDIR)$(BINDIR)/pcan-view
	install -Dm644 assets/taksys_logo.png     $(DESTDIR)$(SHAREDIR)/taksys_logo.png
	install -Dm644 assets/taksys_logo.png     $(DESTDIR)$(ICONDIR)/pcan-view.png
	install -Dm644 assets/pcan-view.desktop   $(DESTDIR)$(APPDIR)/pcan-view.desktop
	-update-desktop-database $(DESTDIR)$(APPDIR) 2>/dev/null || true
	-gtk-update-icon-cache -q $(DESTDIR)$(PREFIX)/share/icons/hicolor 2>/dev/null || true

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/pcan-view
	rm -f $(DESTDIR)$(APPDIR)/pcan-view.desktop
	rm -f $(DESTDIR)$(ICONDIR)/pcan-view.png
	rm -rf $(DESTDIR)$(SHAREDIR)
	-update-desktop-database $(DESTDIR)$(APPDIR) 2>/dev/null || true
	@echo "Uninstalled pcan-view."
