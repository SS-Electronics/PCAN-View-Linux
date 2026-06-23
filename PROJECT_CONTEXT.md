# PCAN-View Linux Project Context

Date: 2026-06-23

## Goal

Build a stable PCAN-View-style application for Linux: a GTK desktop CAN bus
monitor that uses SocketCAN first, supports PEAK-System CAN interfaces through
the Linux CAN stack, and covers the core workflows of PCAN-View:

- connect to a CAN/CAN FD interface
- monitor receive traffic in real time
- transmit one-shot and cyclic frames
- record trace files
- show bus/error state
- provide practical display, filtering, and message-management controls

The project should prioritize correctness, hardware safety, and a dependable
Linux workflow before adding PEAK-specific extensions.

## Product Reference

Reference URL:
https://www.peak-system.com/products/software/analysis-software/pcan-view/

Relevant PCAN-View capabilities from the product page:

- Simple CAN monitor for viewing, transmitting, and recording CAN traffic.
- Manual and periodic transmission at user-determined bit rates.
- Display of bus system errors and memory overflows in CAN hardware.
- Trace function for recording and saving CAN traffic.
- Connection dialog listing available PEAK CAN interfaces.
- Support for CAN 2.0 A/B and CAN FD.
- Nominal CAN bit rates up to 1 Mbit/s.
- CAN FD data bit rates up to 12 Mbit/s.
- Optional custom bit rates.
- Listen-only mode.
- Manual and periodic transmit resolution down to 1 ms.
- Receive timestamp resolution down to 100 us.
- Save and reload transmission messages.
- Sortable receive and transmit lists.
- CAN ID display in hexadecimal or decimal.
- Data byte display in hexadecimal, decimal, or ASCII.
- Display of receive, transmit, and error states.
- Hardware reset and access to hardware-specific settings/information.

The current PEAK page also describes the Windows download as supporting CAN CC,
CAN FD, and CAN XL messages. This Linux project currently targets classic CAN
and CAN FD only.

## Current Repository Shape

This is a C/GTK3 desktop application with a SocketCAN backend.

- `main.c`: GTK application entry point.
- `Makefile`: builds `build/pcan-view` with GTK3 and pthreads.
- `inc/`: shared message, app state, GUI, driver, and SocketCAN headers.
- `driver/drv_can.c`: generic CAN driver vtable wrapper.
- `driver/socketcan.c`: SocketCAN implementation using raw CAN sockets.
- `gui/threads.c`: application state, RX/TX/stats threads, connect/disconnect.
- `gui/main_window.c`: menu, toolbar, trace layout, inline transmit panel.
- `gui/message_view.c`: trace list, formatting helpers, stats panel.
- `gui/settings_dialog.c`: interface, bitrate, CAN FD, listen-only selection.
- `gui/transmit_dialog.c`: advanced transmit window with CAN FD payload support.
- `scripts/install_dependencies.sh`: dependency, module, and vcan setup helper.
- `ISSUES.md`: prior bug history and fixes.

## Verified Status

`make` succeeds and produces:

- `build/pcan-view`

The codebase is beyond a stub. It already has a working architecture for a
SocketCAN-based Linux CAN monitor.

## Implemented Foundation

Current code already includes:

- GTK3 main window, menubar, toolbar, status bar, and footer.
- SocketCAN raw socket backend.
- Interface discovery from `/sys/class/net`.
- Connection settings for interface, nominal bitrate, CAN FD, data bitrate,
  and listen-only mode.
- Classic CAN and CAN FD message structs.
- Receive thread with 100 ms polling timeout.
- Transmit queue and TX worker thread.
- Stats thread with 500 ms update period.
- One-shot transmit and cyclic transmit.
- Advanced transmit dialog with up to 64 CAN FD data bytes.
- Trace table with sequence, direction, timestamp, ID, type, DLC, data, count.
- CSV trace recording.
- Basic bus-load estimation.
- Error-frame counting and row coloring.
- Deduplication mode toggle.
- Driver abstraction that can support another backend later.

## Important Gaps

High-priority gaps against the target product:

- No CAN XL message model or SocketCAN CAN XL support.
- No save/reload support for transmit message sets.
- Receive and transmit views are not full sortable managed lists.
- ID/data display modes now support trace-view switching between hexadecimal,
  decimal, and ASCII where applicable, but preferences are not persisted.
- No custom bitrate entry in the connection dialog.
- Bus state is not read from the kernel; it is initialized as active and not
  meaningfully updated from interface state.
- Error frames are counted but not decoded into useful error details.
- Hardware reset exists in the driver but is not exposed in the UI.
- Hardware-specific PEAK settings/information are not exposed.
- Driver stats are a placeholder; most stats live only in app state.
- No automated tests, CI, or repeatable vcan integration test script.

Remaining stability and safety gaps:

- SocketCAN interface configuration now avoids shell execution and validates
  interface names, but a netlink implementation would still be cleaner.
- Inline and advanced transmit validation now check strict hex input, ID ranges,
  byte ranges, and invalid CAN FD/RTR combinations.
- Dedup mode scans the visible trace model linearly even though app state has a
  declared hash table field.
- UI timestamp display shows milliseconds, while the target product mentions
  100 us receive resolution.
- Bus-load estimation is approximate and does not account for all CAN/CAN FD
  frame overhead details.

## Recommended Build Plan

Phase 1 - Stabilize the Linux MVP:

- Harden interface and transmit input validation.
- Replace or isolate unsafe `system()` interface configuration.
- Add vcan smoke-test scripts for connect, receive, transmit, trace recording,
  and disconnect.
- Add ID range checks for standard, extended, and CAN FD frames.
- Make disconnect/shutdown behavior testable and robust.

Phase 2 - Reach core PCAN-View workflow parity:

- Add display format controls for IDs and data bytes.
- Add sortable columns and separate receive/transmit management where needed.
- Add save/reload for transmit message definitions.
- Add receive filters in the UI.
- Decode SocketCAN error frames into user-readable bus/error states.
- Expose hardware reset in the UI.

Phase 3 - Improve hardware integration:

- Read real bus state and interface statistics from netlink or sysfs.
- Improve bus-load calculation for classic CAN and CAN FD.
- Add PEAK-specific hardware information where available through the Linux
  driver stack.
- Consider an optional PCAN-Basic or PEAK chardev backend only if SocketCAN is
  insufficient for required hardware features.

Phase 4 - Future scope:

- Evaluate CAN XL support separately.
- Add packaging, desktop file, icons, and install/uninstall polish.
- Add documentation screenshots and hardware setup guide.

## Immediate Next Task

Start with stabilization:

Completed in the current working tree:

1. Sanitized SocketCAN interface names and replaced shell command formatting
   with direct `fork`/`execvp` calls to `ip`.
2. Validated transmit IDs/data in the inline and advanced transmit paths.
3. Added a `vcan` smoke-test helper script.
4. Added live trace display format controls for ID and data rendering.

Suggested next task:

Add save/reload support for transmit message definitions, or replace the
linear dedup scan with the existing hash-table field.
