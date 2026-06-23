# PCAN-View Linux

An open-source CAN bus monitor and analyser for Linux, inspired by
[PEAK-System PCAN-View](https://www.peak-system.com/products/software/analysis-software/pcan-view/).
Uses the Linux **SocketCAN** subsystem (`PF_CAN / SOCK_RAW`) and provides a
**GTK 3** GUI with real-time message tracing, message transmission, bus-load
metering, and CSV trace recording.

---

## Features

| Feature | Detail |
|---|---|
| **CAN standards** | CAN 2.0 A (11-bit ID), CAN 2.0 B (29-bit ID), CAN FD |
| **Nominal bitrate** | 10 kbit/s … 1 Mbit/s |
| **CAN FD data bitrate** | 1 … 12 Mbit/s |
| **Listen-only mode** | Passive monitoring without ACK generation |
| **Real-time trace** | Sequence #, direction, timestamp, ID, type, DLC, data |
| **Message deduplication** | Unique-ID view that shows latest value + hit count |
| **Bus load** | Live bar updated every 500 ms |
| **Error frames** | Highlighted in red with CAN error flags decoded |
| **Message transmit** | One-shot and cyclic (configurable interval) |
| **Trace recording** | CSV format: seq, timestamp, dir, ID, type, DLC, data |
| **Multithreading** | Separate threads for RX, TX, and statistics |
| **Virtual CAN** | Works with `vcan0` for off-hardware testing |

---

## Project Structure

```
PCAN-View-Linux/
├── main.c                     Entry point (GTK application init)
├── Makefile
├── inc/
│   ├── can_message.h          CAN message / stats types
│   ├── drv_can.h              Generic driver abstraction (vtable)
│   ├── socketcan.h            SocketCAN back-end interface
│   ├── app_state.h            Global application state
│   └── gui.h                  GUI widget bundle + declarations
├── driver/
│   ├── drv_can.c              Generic driver wrapper
│   └── socketcan.c            SocketCAN implementation (PF_CAN)
├── gui/
│   ├── threads.c              RX / TX / stats threads + connect logic
│   ├── main_window.c          Main GTK window (menu, toolbar, layout)
│   ├── message_view.c         Trace GtkTreeView + statistics panel
│   ├── settings_dialog.c      Connection settings dialog
│   └── transmit_dialog.c      Message transmit window
└── scripts/
    ├── install_dependencies.sh  Dependency installer (Debian/Ubuntu/Arch/Fedora)
    └── test_without_pcan.py     Local vcan smoke-test helper
```

---

## Driver Architecture

```
Application (GUI layer)
        │
        ▼
  can_driver_t  ← generic vtable (drv_can.h / drv_can.c)
        │
        ▼
  socketcan_ctx_t  ← SocketCAN back-end (socketcan.c)
        │
        ▼
  Linux kernel PF_CAN socket
        │
        ▼
  CAN hardware via kernel driver
  (vcan / peak_usb / peak_pci / slcan / …)
```

Adding a new back-end (e.g. PCAN-Basic chardev API) only requires
implementing the `can_driver_t` function-pointer table and passing it
to `drv_can_init()`.

---

## Threading Model

```
 ┌─────────────┐   g_idle_add   ┌─────────────┐
 │  rx_thread  │ ─────────────► │ GTK main    │
 │  (PF_CAN    │                │ thread      │
 │   select)   │                │ (event loop)│
 └─────────────┘                │             │
                                │             │
 ┌─────────────┐  GAsyncQueue   │             │
 │  tx_thread  │ ◄────────────  │ send button │
 │  (blocking  │                │ / cyclic    │
 │   write)    │                │ g_timeout   │
 └─────────────┘                └─────────────┘
 ┌─────────────┐   g_idle_add
 │ stats_thread│ ─────────────► gui_update_stats()
 │  500 ms     │
 └─────────────┘
```

All GTK widget updates happen exclusively in the GTK main thread via
`gdk_threads_add_idle()`.

---

## Prerequisites

### Dependencies

| Package | Purpose |
|---|---|
| `gcc` / `make` | Build toolchain |
| `libgtk-3-dev` | GTK 3 UI framework |
| `libglib2.0-dev` | GLib (async queues, threading utilities) |
| `pkg-config` | Compile flags for GTK |
| `can-utils` | `cansend`, `candump` for testing (optional) |
| `linux-headers` | SocketCAN header files (`linux/can.h`) |

### Kernel modules

```bash
modprobe can
modprobe can_raw
modprobe vcan        # for virtual CAN testing
modprobe peak_usb    # for PEAK-System USB hardware
```

---

## Build & Run

### 1. Install dependencies

```bash
sudo ./scripts/install_dependencies.sh
```

### 2. Build

```bash
make              # optimised release
make DEBUG=1      # debug build with -g -O0
```

### 3. Run

```bash
# With a virtual CAN interface (no hardware needed)
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0

./build/pcan-view
# → File > Connect (or F5) → select vcan0, 500 kbit/s → Connect
```

### 4. Send a test frame (another terminal)

```bash
cansend vcan0 123#DEADBEEF
cansend vcan0 18FF50E5#0102030405060708    # extended frame
```

### 5. Virtual CAN test workflow

For a complete no-hardware test flow, see [Virtual CAN Testing](#virtual-can-testing).

### 6. Run with real PEAK hardware

PEAK-System USB/PCIe interfaces are supported out-of-the-box via the
`peak_usb` / `peak_pci` kernel module (Linux ≥ 3.2).

```bash
# Check available CAN interfaces
ip link show type can

# Connect using e.g. can0 at 500 kbit/s
sudo ./build/pcan-view
# → File > Connect → interface: can0, bitrate: 500 kbit/s → Connect
```

The application will automatically run `ip link set <iface> type can
bitrate <rate>` and `ip link set <iface> up`, so **root privileges (or
`CAP_NET_ADMIN`)** are required for real hardware.

---

## Virtual CAN Testing

Use this workflow to test the application without PEAK/PCAN hardware. It uses
Linux SocketCAN with a virtual CAN interface named `vcan0`.

### 1. Install prerequisites

Recommended:

```bash
sudo ./scripts/install_dependencies.sh
```

Manual Debian/Ubuntu install:

```bash
sudo apt update
sudo apt install -y build-essential pkg-config libgtk-3-dev can-utils iproute2 python3
```

Prepare the virtual CAN kernel module manually if you do not use the helper
script:

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set dev vcan0 up
```

Check that the interface is available:

```bash
ip -details link show dev vcan0
```

### 2. Run test scripts

Build the app, create/bring up `vcan0` when needed, and verify SocketCAN
loopback:

```bash
python3 scripts/test_without_pcan.py
```

If the app is already built:

```bash
python3 scripts/test_without_pcan.py --skip-build
```

If `vcan0` already exists and is up, skip interface setup:

```bash
python3 scripts/test_without_pcan.py --skip-build --no-setup
```

Generate demo traffic for the GUI after connecting the app to `vcan0`:

```bash
python3 scripts/test_without_pcan.py --skip-build --no-setup --traffic-seconds 30
```

Launch the GTK application after the smoke test:

```bash
python3 scripts/test_without_pcan.py --skip-build --no-setup --launch-app
```

You can also send individual frames from another terminal:

```bash
cansend vcan0 123#DEADBEEF
cansend vcan0 18FF50E5#0102030405060708
```

### 3. What to check in the application

Open the app:

```bash
./build/pcan-view
```

Then verify these items:

1. **Connect**: use `File > Connect` or `F5`, select `vcan0`, keep bitrate at
   `500000`, and click Connect. The toolbar should show connected state and
   the statistics panel should show `vcan0`.
2. **Receive trace**: run the demo traffic command above or use `cansend`.
   New Rx rows should appear with timestamp, ID, type, DLC, data, and count.
3. **Transmit once**: open the Transmit tab, set ID `123`, data bytes such as
   `DE AD BE EF`, and click Send Once. A Tx row should appear.
4. **Cyclic transmit**: set an interval such as `100 ms`, click Start Cyclic,
   confirm repeated Tx activity, then click Stop Cyclic.
5. **Advanced transmit**: open `CAN > Transmit...` and verify standard,
   extended, RTR, and CAN FD validation messages behave as expected.
6. **Display modes**: use `View > ID Format` and `View > Data Format` to switch
   between hexadecimal, decimal, and ASCII display. Existing trace rows should
   refresh.
7. **Deduplication**: enable `View > Deduplicate Messages`, send repeated
   frames with the same ID, and confirm the Count column increments.
8. **Trace recording**: use `File > Start Trace...`, send traffic, stop the
   trace, and confirm the CSV file contains the captured frames.
9. **Disconnect**: use `File > Disconnect` or `F6`; the UI should return to a
   disconnected state without crashing.

---

## PCAN-View Feature Comparison

| PCAN-View (Windows) | This application |
|---|---|
| CAN 2.0 A/B trace | ✅ |
| CAN FD trace | ✅ |
| Timestamp (100 µs resolution) | ✅ (nanosecond via `CLOCK_REALTIME`) |
| Listen-only mode | ✅ |
| Bus load measurement | ✅ |
| Error frame display | ✅ |
| Message transmit (one-shot) | ✅ |
| Cyclic transmit | ✅ |
| Trace recording | ✅ (CSV) |
| Unique-message (dedup) view | ✅ |
| Filter by ID/mask | ✅ (driver level via `CAN_RAW_FILTER`) |
| Bus-off / warning states | ✅ |

---

## Extending the Driver

```c
/* Implement the can_driver_t interface */
static can_driver_t my_driver = {
    .init         = my_init,
    .deinit       = my_deinit,
    .send         = my_send,
    .recv         = my_recv,
    .get_stats    = my_get_stats,
    .set_filter   = my_set_filter,
    .clear_filter = my_clear_filter,
    .reset        = my_reset,
    .error_string = my_error_string,
};

/* Use it */
drv_can_init(&my_driver, "can0", 500000, 0, 0, 0);
```

---

## License

GNU General Public License v3.0 – see [LICENSE](LICENSE).

---

## References

- [PEAK-System Linux driver documentation](https://www.peak-system.com/fileadmin/media/linux/index.php)
- [Linux SocketCAN documentation](https://www.kernel.org/doc/Documentation/networking/can.rst)
- [PCAN-View product page](https://www.peak-system.com/products/software/analysis-software/pcan-view/)
- [can-utils](https://github.com/linux-can/can-utils)
