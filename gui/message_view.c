/*
 * message_view.c – CAN trace view and statistics panel
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/app_state.h"
#include "../inc/can_message.h"

#define MAX_TRACE_ROWS  50000
#define DEDUP_TABLE_SZ  0x800u   /* initial hash table size */

/* ------------------------------------------------------------------ */
/* Formatting helpers                                                   */
/* ------------------------------------------------------------------ */

void gui_format_id(char *buf, size_t sz, uint32_t id, int is_ext)
{
    if (is_ext)
        snprintf(buf, sz, "%08X", id);
    else
        snprintf(buf, sz, "%03X", id);
}

void gui_format_data(char *buf, size_t sz, const uint8_t *d, uint8_t dlc)
{
    if (dlc == 0) {
        strncpy(buf, "-", sz);
        return;
    }
    size_t pos = 0;
    for (uint8_t i = 0; i < dlc && pos + 3 < sz; i++) {
        pos += (size_t)snprintf(buf + pos, sz - pos,
                                i ? " %02X" : "%02X", d[i]);
    }
}

const char *gui_msg_type_str(const can_msg_t *m)
{
    if (m->is_error)  return "Err";
    if (m->is_fd)     return m->is_brs ? "FD+BRS" : "FD";
    if (m->is_remote) return m->is_extended ? "ExtRTR" : "StdRTR";
    return m->is_extended ? "Ext" : "Std";
}

const char *gui_bus_state_str(uint8_t st)
{
    switch (st) {
    case CAN_BUS_WARNING: return "Warning";
    case CAN_BUS_PASSIVE: return "Passive";
    case CAN_BUS_OFF:     return "Bus-Off";
    default:              return "Active";
    }
}

const char *gui_bus_state_color(uint8_t st)
{
    switch (st) {
    case CAN_BUS_WARNING: return "orange";
    case CAN_BUS_PASSIVE: return "red";
    case CAN_BUS_OFF:     return "darkred";
    default:              return "green";
    }
}

/* Row foreground colour */
static const char *row_fg(const can_msg_t *m)
{
    if (m->is_error)                return "red";
    if (m->direction == CAN_DIR_TX) return "blue";
    return NULL; /* default */
}

/* ------------------------------------------------------------------ */
/* Trace view creation                                                  */
/* ------------------------------------------------------------------ */

GtkWidget *create_trace_view(void)
{
    GtkListStore *store = gtk_list_store_new(
        TCOL_NUM,
        G_TYPE_UINT64,   /* SEQ   */
        G_TYPE_STRING,   /* DIR   */
        G_TYPE_STRING,   /* TIME  */
        G_TYPE_STRING,   /* ID    */
        G_TYPE_STRING,   /* TYPE  */
        G_TYPE_UINT,     /* DLC   */
        G_TYPE_STRING,   /* DATA  */
        G_TYPE_UINT,     /* COUNT */
        G_TYPE_STRING    /* FG    */
    );
    g_gui.trace_store = store;

    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    g_gui.trace_view = tree;

    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), TRUE);
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(tree), TRUE);

    struct { int col; const char *title; int width; } cols[] = {
        { TCOL_SEQ,   "#",         70  },
        { TCOL_DIR,   "Dir",       50  },
        { TCOL_TIME,  "Timestamp", 130 },
        { TCOL_ID,    "ID",        90  },
        { TCOL_TYPE,  "Type",      75  },
        { TCOL_DLC,   "DLC",       45  },
        { TCOL_DATA,  "Data",      350 },
        { TCOL_COUNT, "Count",     60  },
    };

    for (size_t i = 0; i < sizeof(cols)/sizeof(cols[0]); i++) {
        GtkCellRenderer   *rend = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col  = gtk_tree_view_column_new_with_attributes(
            cols[i].title, rend,
            "text",       cols[i].col,
            "foreground", TCOL_FG,
            NULL);
        gtk_tree_view_column_set_sizing(col,
            GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(col, cols[i].width);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
    }

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tree);
    return scroll;
}

/* ------------------------------------------------------------------ */
/* Statistics panel                                                     */
/* ------------------------------------------------------------------ */

GtkWidget *create_stats_panel(void)
{
    GtkWidget *frame = gtk_frame_new("Bus Statistics");
    gtk_widget_set_margin_start(frame, 4);
    gtk_widget_set_margin_end(frame, 4);
    gtk_widget_set_margin_top(frame, 2);
    gtk_widget_set_margin_bottom(frame, 2);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_widget_set_margin_start(grid, 8);
    gtk_widget_set_margin_end(grid, 8);
    gtk_widget_set_margin_top(grid, 6);
    gtk_widget_set_margin_bottom(grid, 6);
    gtk_container_add(GTK_CONTAINER(frame), grid);

    /* Helper */
    int row = 0;
#define ADD_ROW(lbl_str, widget) do { \
    GtkWidget *_l = gtk_label_new(lbl_str); \
    gtk_label_set_xalign(GTK_LABEL(_l), 1.0f); \
    gtk_grid_attach(GTK_GRID(grid), _l, 0, row, 1, 1); \
    gtk_grid_attach(GTK_GRID(grid), (widget), 1, row, 1, 1); \
    row++; \
} while(0)

    g_gui.lbl_connection = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(g_gui.lbl_connection), 0.0f);
    ADD_ROW("Interface:", g_gui.lbl_connection);

    g_gui.lbl_bitrate = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(g_gui.lbl_bitrate), 0.0f);
    ADD_ROW("Bitrate:", g_gui.lbl_bitrate);

    g_gui.progress_bus_load = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(
        GTK_PROGRESS_BAR(g_gui.progress_bus_load), TRUE);
    gtk_widget_set_size_request(g_gui.progress_bus_load, 200, -1);
    ADD_ROW("Bus Load:", g_gui.progress_bus_load);

    g_gui.lbl_rx = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(g_gui.lbl_rx), 0.0f);
    ADD_ROW("Rx Frames:", g_gui.lbl_rx);

    g_gui.lbl_tx = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(g_gui.lbl_tx), 0.0f);
    ADD_ROW("Tx Frames:", g_gui.lbl_tx);

    g_gui.lbl_err = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(g_gui.lbl_err), 0.0f);
    ADD_ROW("Error Frames:", g_gui.lbl_err);

    g_gui.lbl_bus_state = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(g_gui.lbl_bus_state), 0.0f);
    ADD_ROW("Bus State:", g_gui.lbl_bus_state);
#undef ADD_ROW

    return frame;
}

/* ------------------------------------------------------------------ */
/* Add message to trace (called from GTK main thread via idle_add)     */
/* ------------------------------------------------------------------ */

void gui_add_message(const can_msg_t *msg)
{
    if (!g_gui.trace_store) return;

    /* Enforce max rows – remove oldest when limit reached */
    gint n = gtk_tree_model_iter_n_children(
        GTK_TREE_MODEL(g_gui.trace_store), NULL);
    if (n >= MAX_TRACE_ROWS) {
        GtkTreeIter first;
        if (gtk_tree_model_get_iter_first(
                GTK_TREE_MODEL(g_gui.trace_store), &first))
            gtk_list_store_remove(g_gui.trace_store, &first);
    }

    /* In dedup mode, search for existing row with same ID */
    if (g_app.dedup_mode && !msg->is_error) {
        GtkTreeIter iter;
        gboolean found = FALSE;
        if (gtk_tree_model_get_iter_first(
                GTK_TREE_MODEL(g_gui.trace_store), &iter)) {
            do {
                gchar *id_str = NULL;
                gtk_tree_model_get(GTK_TREE_MODEL(g_gui.trace_store),
                                   &iter, TCOL_ID, &id_str, -1);
                char this_id[16];
                gui_format_id(this_id, sizeof(this_id),
                              msg->id, msg->is_extended);
                if (id_str && strcmp(id_str, this_id) == 0) {
                    found = TRUE;
                    g_free(id_str);

                    guint cnt = 0;
                    gtk_tree_model_get(GTK_TREE_MODEL(g_gui.trace_store),
                                       &iter, TCOL_COUNT, &cnt, -1);

                    char ts[32], data_buf[192];
                    struct tm *tm_info = localtime(&msg->timestamp.tv_sec);
                    snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03ld",
                             tm_info->tm_hour, tm_info->tm_min,
                             tm_info->tm_sec,
                             msg->timestamp.tv_nsec / 1000000L);
                    gui_format_data(data_buf, sizeof(data_buf),
                                    msg->data, msg->dlc);

                    gtk_list_store_set(g_gui.trace_store, &iter,
                        TCOL_TIME,  ts,
                        TCOL_DATA,  data_buf,
                        TCOL_COUNT, cnt + 1,
                        -1);
                    break;
                }
                g_free(id_str);
            } while (gtk_tree_model_iter_next(
                         GTK_TREE_MODEL(g_gui.trace_store), &iter));
        }
        if (found) return;
    }

    /* Format fields */
    char id_buf[16], data_buf[192], ts[32];
    gui_format_id(id_buf, sizeof(id_buf), msg->id, msg->is_extended);
    gui_format_data(data_buf, sizeof(data_buf), msg->data, msg->dlc);

    struct tm *tm_info = localtime(&msg->timestamp.tv_sec);
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03ld",
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
             msg->timestamp.tv_nsec / 1000000L);

    GtkTreeIter iter;
    gtk_list_store_append(g_gui.trace_store, &iter);
    gtk_list_store_set(g_gui.trace_store, &iter,
        TCOL_SEQ,   msg->seq,
        TCOL_DIR,   msg->direction == CAN_DIR_TX ? "Tx" : "Rx",
        TCOL_TIME,  ts,
        TCOL_ID,    id_buf,
        TCOL_TYPE,  gui_msg_type_str(msg),
        TCOL_DLC,   (guint)msg->dlc,
        TCOL_DATA,  data_buf,
        TCOL_COUNT, 1u,
        TCOL_FG,    row_fg(msg),
        -1);

    /* Auto-scroll to bottom */
    GtkTreePath *path = gtk_tree_model_get_path(
        GTK_TREE_MODEL(g_gui.trace_store), &iter);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(g_gui.trace_view),
                                  path, NULL, FALSE, 0, 0);
    gtk_tree_path_free(path);
}

/* ------------------------------------------------------------------ */
/* Clear trace                                                          */
/* ------------------------------------------------------------------ */

void gui_clear_trace(void)
{
    if (g_gui.trace_store)
        gtk_list_store_clear(g_gui.trace_store);
}

/* ------------------------------------------------------------------ */
/* Update statistics panel                                              */
/* ------------------------------------------------------------------ */

void gui_update_stats(void)
{
    char buf[64];

    if (g_app.connected) {
        gtk_label_set_text(GTK_LABEL(g_gui.lbl_connection), g_app.iface);
        snprintf(buf, sizeof(buf), "%u bps", g_app.bitrate);
        gtk_label_set_text(GTK_LABEL(g_gui.lbl_bitrate), buf);
    } else {
        gtk_label_set_text(GTK_LABEL(g_gui.lbl_connection), "—");
        gtk_label_set_text(GTK_LABEL(g_gui.lbl_bitrate), "—");
    }

    double load = g_app.bus_load;
    snprintf(buf, sizeof(buf), "%.1f%%", load);
    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(g_gui.progress_bus_load), load / 100.0);
    gtk_progress_bar_set_text(
        GTK_PROGRESS_BAR(g_gui.progress_bus_load), buf);

    snprintf(buf, sizeof(buf), "%llu",
             (unsigned long long)g_app.rx_count);
    gtk_label_set_text(GTK_LABEL(g_gui.lbl_rx), buf);

    snprintf(buf, sizeof(buf), "%llu",
             (unsigned long long)g_app.tx_count);
    gtk_label_set_text(GTK_LABEL(g_gui.lbl_tx), buf);

    snprintf(buf, sizeof(buf), "%llu",
             (unsigned long long)g_app.error_count);
    gtk_label_set_text(GTK_LABEL(g_gui.lbl_err), buf);

    const char *state_str   = gui_bus_state_str(g_app.bus_state);
    const char *state_color = gui_bus_state_color(g_app.bus_state);
    char markup[128];
    snprintf(markup, sizeof(markup),
             "<span foreground=\"%s\">%s</span>",
             state_color, state_str);
    gtk_label_set_markup(GTK_LABEL(g_gui.lbl_bus_state), markup);
}

/* ------------------------------------------------------------------ */
/* Status bar                                                           */
/* ------------------------------------------------------------------ */

void gui_status_message(const char *fmt, ...)
{
    if (!g_gui.statusbar) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gtk_statusbar_pop(GTK_STATUSBAR(g_gui.statusbar),
                      g_gui.statusbar_ctx);
    gtk_statusbar_push(GTK_STATUSBAR(g_gui.statusbar),
                       g_gui.statusbar_ctx, buf);
}

/* ------------------------------------------------------------------ */
/* Error dialog                                                         */
/* ------------------------------------------------------------------ */

void gui_show_error(GtkWidget *parent, const char *title, const char *msg)
{
    GtkWidget *dlg = gtk_message_dialog_new(
        parent ? GTK_WINDOW(parent) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK,
        "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}
