#ifndef GUI_H
#define GUI_H

#include <gtk/gtk.h>
#include "can_message.h"
#include "app_state.h"

/* Trace-view column indices */
enum {
    TCOL_SEQ = 0,
    TCOL_DIR,
    TCOL_TIME,
    TCOL_ID,
    TCOL_TYPE,
    TCOL_DLC,
    TCOL_DATA,
    TCOL_INTERVAL,
    TCOL_FREQ,
    TCOL_COUNT,
    TCOL_FG,        /* row foreground colour (markup) */
    TCOL_ID_RAW,    /* hidden numeric ID for sorting / reformatting */
    TCOL_EXT_RAW,   /* hidden extended-ID flag */
    TCOL_DIR_RAW,   /* hidden direction flag for row roll-up */
    TCOL_DATA_RAW,  /* hidden compact hex data for reformatting */
    TCOL_TS_NS,     /* hidden last timestamp in nanoseconds */
    TCOL_NUM
};

/* GUI widget bundle (populated by gui_create_main_window) */
typedef struct {
    GtkWidget    *window;
    GtkWidget    *toolbar_connect;
    GtkWidget    *toolbar_disconnect;
    GtkWidget    *toolbar_clear;
    GtkWidget    *toolbar_trace_start;
    GtkWidget    *toolbar_trace_stop;
    GtkWidget    *trace_view;
    GtkListStore *trace_store;
    GtkWidget    *lbl_connection;
    GtkWidget    *lbl_bitrate;
    GtkWidget    *lbl_stats_summary;
    GtkWidget    *lbl_bus_load;
    GtkWidget    *lbl_rx;
    GtkWidget    *lbl_tx;
    GtkWidget    *lbl_err;
    GtkWidget    *lbl_bus_state;
    GtkWidget    *progress_bus_load;
    GtkWidget    *statusbar;
    guint         statusbar_ctx;
} gui_widgets_t;

extern gui_widgets_t g_gui;

/* Main window */
GtkWidget *gui_create_main_window(GtkApplication *app);

/* Message helpers */
void gui_add_message(const can_msg_t *msg);
void gui_clear_trace(void);
void gui_refresh_trace_display(void);
void gui_update_stats(void);
void gui_status_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Dialogs */
void gui_show_settings_dialog(GtkWidget *parent);
void gui_show_transmit_window(void);
void gui_show_about_dialog(GtkWidget *parent);
void gui_show_error(GtkWidget *parent, const char *title, const char *msg);

/* Formatting helpers */
void        gui_format_id(char *buf, size_t sz, uint32_t id, int is_ext);
void        gui_format_data(char *buf, size_t sz, const uint8_t *d, uint8_t dlc);
const char *gui_msg_type_str(const can_msg_t *m);
const char *gui_bus_state_str(uint8_t st);
const char *gui_bus_state_color(uint8_t st);

/* Thread entry points */
void *thread_rx(void *arg);
void *thread_tx(void *arg);
void *thread_stats(void *arg);

/* Connect / Disconnect */
void app_do_connect(void);
void app_do_disconnect(void);

#endif /* GUI_H */
