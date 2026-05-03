# ISSUES – PCAN-View Linux

Tracks bugs, root causes, and the exact fixes applied to this project.

---

## Fixed Issues

---

### ISSUE-001 — IntelliSense / IDE Symbol Resolution Failure

| Field | Detail |
|---|---|
| **Status** | Fixed |
| **Severity** | Low (developer experience) |
| **File(s) affected** | *(no source file — IDE config only)* |

**Symptom**
VS Code / clangd reported unresolved symbols for all GTK3 and GLib types
(`GtkWidget`, `GtkApplication`, `gpointer`, etc.), making the IDE unable to
provide code completion, go-to-definition, or inline error checking.

**Root Cause**
No `.vscode/c_cpp_properties.json` existed in the project, so the IDE used
its default include search paths which do not include GTK3 headers
(`/usr/include/gtk-3.0`, `/usr/include/glib-2.0`, etc.).

**Fix**
Created [.vscode/c_cpp_properties.json](.vscode/c_cpp_properties.json) with
the full set of include paths required by GTK3 on a standard Debian/Ubuntu
Linux installation:

```
/usr/include/gtk-3.0
/usr/include/glib-2.0
/usr/lib/x86_64-linux-gnu/glib-2.0/include
/usr/include/pango-1.0
/usr/include/harfbuzz
/usr/include/cairo
/usr/include/gdk-pixbuf-2.0
/usr/include/atk-1.0
/usr/include/gio-unix-2.0
```

Also set `cStandard = c11`, `intelliSenseMode = linux-gcc-x64`, and defined
`_GNU_SOURCE` to match the compiler flags used in the `Makefile`.

---

### ISSUE-002 — Application Crash on Disconnect

| Field | Detail |
|---|---|
| **Status** | Fixed |
| **Severity** | Critical |
| **File(s) affected** | `gui/threads.c` |

**Symptom**
Clicking **Disconnect** (or closing the main window while connected) caused a
segmentation fault / application crash.

**Root Cause**
Race condition between background worker threads and GTK widget destruction:

1. `app_do_disconnect()` sets `rx_running = 0` and `stats_running = 0`.
2. The RX and stats threads execute one final loop iteration and call
   `gdk_threads_add_idle(idle_add_message, ...)` / `gdk_threads_add_idle(idle_update_stats, ...)`.
   These callbacks are queued in the GLib main context but **not yet executed**.
3. `pthread_join()` returns — the threads have exited.
4. `drv_can_deinit()` closes the socket.
5. GTK begins destroying window widgets (`g_gui.trace_store`, labels, etc.)
   as part of the normal destroy chain.
6. The queued idle callbacks fire **after** their target widgets are freed →
   null/dangling pointer dereference → **crash**.

**Fix** — two layers:

**Layer 1 (primary):** Flush the entire GLib main context queue immediately
after all threads have joined, while widgets are still alive:

```c
/* gui/threads.c – app_do_disconnect() */
pthread_join(g_app.rx_thread,    NULL);
pthread_join(g_app.tx_thread,    NULL);
pthread_join(g_app.stats_thread, NULL);

/* Flush idle callbacks queued by threads before they exited */
while (g_main_context_pending(g_main_context_default()))
    g_main_context_iteration(g_main_context_default(), FALSE);
```

**Layer 2 (defence-in-depth):** Added NULL guards in both idle callbacks so
that a late-arriving callback does not crash even if widgets are already gone:

```c
static gboolean idle_add_message(gpointer data) {
    can_msg_t *msg = (can_msg_t *)data;
    if (g_gui.trace_store)   /* guard */
        gui_add_message(msg);
    free(msg);
    return G_SOURCE_REMOVE;
}
```

**Additional safeguard:** The inline transmit panel's cyclic `g_timeout` is
explicitly stopped inside `on_window_delete()` before `app_do_disconnect()`
is called, so no timer callbacks fire on dead widgets.

---

### ISSUE-003 — Transmit Functionality Not Visible in Main Window

| Field | Detail |
|---|---|
| **Status** | Fixed |
| **Severity** | Medium (UX) |
| **File(s) affected** | `gui/main_window.c` |

**Symptom**
The only way to transmit a CAN message was through a separate floating
dialog opened via the toolbar button.  There was no inline transmit panel
visible inside the main window, unlike the reference PCAN-View (Windows)
application which shows both receive and transmit in one integrated view.

**Root Cause**
Original design put all transmit functionality in a separate non-modal
`GtkWindow` (`gui/transmit_dialog.c`).  No inline panel existed in the main
window layout.

**Fix**
Replaced the single-panel bottom section with a `GtkNotebook` containing two
tabs:

- **Statistics** — existing bus stats panel (unchanged)
- **Transmit** — new inline transmit panel built directly in `main_window.c`

The inline panel provides:
- ID entry (hex), Extended / RTR checkboxes
- DLC spinner (0–8)
- 8 data byte entry fields (hex, greyed out per DLC and RTR state)
- **Send Once** button
- Cyclic transmit: interval (ms), **Start Cyclic** / **Stop Cyclic**
- **Advanced TX…** button → opens `transmit_dialog.c` for FD frames, 64-byte payloads

The full `transmit_dialog.c` (advanced TX window) is retained unchanged for
CAN FD / extended DLC use cases.

---

### ISSUE-004 — Missing Application Footer

| Field | Detail |
|---|---|
| **Status** | Fixed |
| **Severity** | Low (cosmetic / branding) |
| **File(s) affected** | `gui/main_window.c` |

**Symptom**
The main window had no author / company attribution visible in the GUI.

**Fix**
Added a branded footer bar at the very bottom of the main window, below the
GtkStatusbar:

```
SS Electronics  |  Author: Subhajit Roy  |  subhajitroy005@gmail.com  |  License: GPL 3.0
```

Rendered as a centred GTK markup label with small font and a horizontal
separator above it.

---

### ISSUE-005 — Compiler Warnings (Clean Build)

| Field | Detail |
|---|---|
| **Status** | Fixed |
| **Severity** | Low |
| **File(s) affected** | `driver/socketcan.c`, `gui/threads.c`, `gui/message_view.c`, `main.c` |

The following warnings were resolved to achieve a zero-warning build under
`-Wall -Wextra -Wpedantic`:

| Warning | File | Fix |
|---|---|---|
| `CANFD_MAX_DLC` redefined (conflicts with kernel header) | `inc/can_message.h` | Renamed to `CANFD_DATA_MAX` |
| `G_APPLICATION_DEFAULT_FLAGS` deprecated (or vice-versa) | `main.c` | Matched to installed GLib version |
| `gtk_tree_view_set_rules_hint` deprecated in GTK ≥ 3.14 | `gui/message_view.c` | Removed call |
| `fscanf` return value ignored | `driver/socketcan.c` | Checked return value |
| `snprintf` / `strncpy` format-truncation | `driver/socketcan.c` | Added `strlen` length check; used `memcpy` |
| Type-punned pointer (`double*` ↔ `uint64_t*`) | `gui/threads.c` | Direct `double` assignment (naturally atomic on x86-64) |

---

### ISSUE-006 — Application Hangs on Disconnect (Force-Quit Required)

| Field | Detail |
|---|---|
| **Status** | Fixed |
| **Severity** | Critical |
| **File(s) affected** | `gui/threads.c`, `inc/app_state.h` |

**Symptom**
Clicking **Disconnect** caused the application to hang indefinitely, requiring a
force-quit. The window became unresponsive and the OS presented a "force quit"
dialog.

**Root Cause**
Two compounding problems:

1. The idle-flush loop added as part of the ISSUE-002 fix calls
   `g_main_context_pending()` / `g_main_context_iteration()` on the GTK main
   thread. GTK's internal machinery (timers, GDK events, redraw requests) keeps
   `g_main_context_pending()` returning `TRUE` indefinitely — the loop never
   terminates, permanently blocking the main thread.

2. The stats thread slept for a single 500 ms `nanosleep`. When
   `stats_running = 0` is set, `pthread_join` had to wait up to 500 ms for the
   thread to wake and check the flag, freezing the UI noticeably (and becoming
   the dominant wait time once the infinite loop was removed from a partial fix
   attempt).

**Fix**

**Change 1 — Remove the hang-causing flush loop** from `app_do_disconnect()`:
```c
/* REMOVED — causes infinite hang because GTK internal sources are always pending:
while (g_main_context_pending(g_main_context_default()))
    g_main_context_iteration(g_main_context_default(), FALSE);
*/
```

**Change 2 — Add `shutting_down` flag** to `app_state_t` (`inc/app_state.h`):
```c
volatile int shutting_down;
```
Set to `1` at the top of `app_do_disconnect()` before stopping threads; reset
to `0` after `drv_can_deinit()` so a subsequent reconnect works correctly.

**Change 3 — Guard idle callbacks with `shutting_down`** so any callbacks that
were already queued before the threads exited silently discard their payload
instead of touching widgets:
```c
static gboolean idle_add_message(gpointer data) {
    can_msg_t *msg = (can_msg_t *)data;
    if (!g_app.shutting_down && g_gui.trace_store)
        gui_add_message(msg);
    free(msg);
    return G_SOURCE_REMOVE;
}
```

**Change 4 — Break the stats thread's 500 ms sleep into 10 × 50 ms slices**
so `pthread_join` returns within 50 ms instead of 500 ms:
```c
unsigned int ticks = 0;
while (g_app.stats_running) {
    struct timespec ts = { 0, 50000000L }; /* 50 ms */
    nanosleep(&ts, NULL);
    if (++ticks < 10) continue;            /* fire every 500 ms */
    ticks = 0;
    /* ... calculate bus load ... */
}
```

---

## Open / Known Limitations

| ID | Description | Priority |
|---|---|---|
| OPEN-001 | Requires root / `CAP_NET_ADMIN` for real hardware; no privilege-escalation helper | Medium |
| OPEN-002 | Bus-off / error-passive state is not auto-detected from kernel netlink events; only tracked via error frames | Medium |
| OPEN-003 | Trace CSV format is proprietary; no import of PCAN `.trc` files | Low |
| OPEN-004 | Deduplication (unique-ID view) uses O(n) linear scan; may lag on very large unique-ID sets | Low |
| OPEN-005 | CAN FD data bitrate > 5 Mbit/s requires hardware that supports it; no runtime capability check | Low |
