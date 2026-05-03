/*
 * main.c – Entry point for PCAN-View Linux
 *
 * A GTK3-based CAN bus monitor / analyser using the Linux SocketCAN
 * subsystem.  Features:
 *   - Real-time receive trace with timestamp, ID, type, DLC, data
 *   - CAN FD support (ISO 11898-1:2015)
 *   - Listen-only mode
 *   - Message transmission (single-shot and cyclic)
 *   - Bus-load meter and error-frame detection
 *   - CSV trace recording
 *   - Message deduplication view
 *
 * Threading model:
 *   Main thread  – GTK event loop
 *   rx_thread    – socketcan_recv → g_idle_add → GUI trace
 *   tx_thread    – GAsyncQueue → socketcan_send
 *   stats_thread – 500 ms timer → bus-load calculation → g_idle_add
 */

#include <gtk/gtk.h>
#include "inc/app_state.h"
#include "inc/gui.h"

static void on_app_activate(GtkApplication *app, gpointer data)
{
    (void)data;
    gui_create_main_window(app);
}

int main(int argc, char *argv[])
{
    app_state_init();

    GtkApplication *app = gtk_application_new(
        "com.peak-system.pcan-view-linux",
        G_APPLICATION_FLAGS_NONE);

    g_signal_connect(app, "activate",
                     G_CALLBACK(on_app_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    app_state_cleanup();

    return status;
}
