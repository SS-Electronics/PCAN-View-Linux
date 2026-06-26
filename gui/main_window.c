/*
 * main_window.c – Main GTK3 application window
 *
 * Layout:
 *   GtkApplicationWindow
 *   └─ GtkBox (vertical)
 *      ├─ GtkMenuBar
 *      ├─ GtkToolbar
 *      ├─ GtkPaned (vertical)
 *      │  ├─ trace view  (scrolled GtkTreeView)
 *      │  └─ transmit rows (scrolled GtkGrid)
 *      └─ GtkStatusbar
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/app_state.h"
#include "../inc/can_message.h"

/* Declared in message_view.c */
extern GtkWidget *create_trace_view(void);

/* Forward-declared here; defined in the transmit-panel section below */
#define TX_PANEL_DATA_COLS 8
#define TX_PANEL_MAX_ROWS   16
#define TX_STD_ID_MAX      0x7FFu
#define TX_EXT_ID_MAX      0x1FFFFFFFu
#define TX_BYTE_MAX        0xFFu

/* Grid column layout shared by the header row and every transmit row, so the
 * fields stay aligned and resize by ratio with the window. */
enum {
    TXG_COL_NUM = 0,
    TXG_COL_ID,
    TXG_COL_EXT,
    TXG_COL_RTR,
    TXG_COL_DLC,
    TXG_COL_DATA0,                                   /* 8 data byte columns */
    TXG_COL_INTERVAL = TXG_COL_DATA0 + TX_PANEL_DATA_COLS,
    TXG_COL_SEND,
    TXG_COL_START,
    TXG_COL_STOP,
    TXG_COL_STATUS,
};

typedef struct {
    GtkWidget *num_lbl;
    GtkWidget *id_entry;
    GtkWidget *ext_check;
    GtkWidget *rtr_check;
    GtkWidget *dlc_spin;
    GtkWidget *data_entry[TX_PANEL_DATA_COLS];
    GtkWidget *interval_spin;
    GtkWidget *send_btn;
    GtkWidget *start_btn;
    GtkWidget *stop_btn;
    GtkWidget *status_lbl;
    guint      cyclic_id;
} tx_panel_row_t;

static struct {
    tx_panel_row_t rows[TX_PANEL_MAX_ROWS];
    int            count;
    GtkWidget     *rows_box;
    GtkWidget     *placeholder;
} s_txp;

static tx_panel_row_t *txp_row_from_data(gpointer data);
static void txp_set_status(tx_panel_row_t *row,
                           const char *color,
                           const char *msg);
static void txp_clear_row(tx_panel_row_t *row);
static void txp_clear_all(void);
static void txp_add_row(void);
static void txp_remove_last_row(void);
static void txp_set_error(tx_panel_row_t *row, const char *msg);
static void txp_update_data_fields(tx_panel_row_t *row);
static gboolean txp_build_frame(tx_panel_row_t *row, can_msg_t *msg);
static gboolean txp_queue_frame(int row_index, gboolean cyclic);
static void txp_stop_cyclic_row(tx_panel_row_t *row, const char *status);
static void txp_stop_all_cyclic(void);
static gboolean txp_cyclic_tick(gpointer data);
static void txp_send_once(GtkWidget *w, gpointer d);
static void txp_start_cyclic(GtkWidget *w, gpointer d);
static void txp_stop_cyclic(GtkWidget *w, gpointer d);
static void txp_dlc_changed(GtkSpinButton *btn, gpointer d);
static void txp_rtr_toggled(GtkToggleButton *btn, gpointer d);

/* ------------------------------------------------------------------ */
/* Toolbar / menu callbacks                                             */
/* ------------------------------------------------------------------ */

static gboolean on_window_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    /* Stop cyclic timer before disconnect to prevent callbacks on dead widgets */
    txp_stop_all_cyclic();
    app_do_disconnect();
    return FALSE; /* allow default window destruction */
}

static void on_connect(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gui_show_settings_dialog(g_gui.window);
}

static void on_disconnect(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    txp_stop_all_cyclic();
    app_do_disconnect();
}

static void on_clear(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gui_clear_trace();
    gui_status_message("Trace cleared.");
}

static void on_trace_start(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!g_app.connected) {
        gui_show_error(g_gui.window, "Error",
                       "Not connected – cannot start trace.");
        return;
    }
    if (g_app.tracing) return;

    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Trace File",
        GTK_WINDOW(g_gui.window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(
        GTK_FILE_CHOOSER(dlg), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "trace.csv");

    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_add_pattern(ff, "*.csv");
    gtk_file_filter_set_name(ff, "CSV trace (*.csv)");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ff);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        strncpy(g_app.trace_filename, fn,
                APP_TRACE_FILE_LEN - 1);
        g_free(fn);

        pthread_mutex_lock(&g_app.trace_mutex);
        g_app.trace_file = fopen(g_app.trace_filename, "w");
        if (g_app.trace_file) {
            fprintf(g_app.trace_file,
                    "seq,timestamp,direction,id,type,dlc,data\n");
            g_app.tracing = 1;
            gtk_widget_set_sensitive(g_gui.toolbar_trace_start, FALSE);
            gtk_widget_set_sensitive(g_gui.toolbar_trace_stop,  TRUE);
            gui_status_message("Tracing to %s", g_app.trace_filename);
        } else {
            gui_show_error(g_gui.window, "Trace Error",
                           "Could not open trace file for writing.");
        }
        pthread_mutex_unlock(&g_app.trace_mutex);
    }
    gtk_widget_destroy(dlg);
}

static void on_trace_stop(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!g_app.tracing) return;

    g_app.tracing = 0;
    pthread_mutex_lock(&g_app.trace_mutex);
    if (g_app.trace_file) {
        fclose(g_app.trace_file);
        g_app.trace_file = NULL;
    }
    pthread_mutex_unlock(&g_app.trace_mutex);

    gtk_widget_set_sensitive(g_gui.toolbar_trace_start, TRUE);
    gtk_widget_set_sensitive(g_gui.toolbar_trace_stop,  FALSE);
    gui_status_message("Trace saved to %s", g_app.trace_filename);
}

static void on_transmit(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gui_show_transmit_window();
}

static void on_settings(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gui_show_settings_dialog(g_gui.window);
}

static void on_about(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gui_show_about_dialog(g_gui.window);
}

static void on_quit(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    txp_stop_all_cyclic();
    app_do_disconnect();
    gtk_widget_destroy(g_gui.window);
}

static void on_dedup_toggle(GtkCheckMenuItem *item, gpointer d)
{
    (void)d;
    g_app.dedup_mode = gtk_check_menu_item_get_active(item);
    gui_clear_trace();
    gui_status_message("Tx roll-up: %s",
                       g_app.dedup_mode ? "ON" : "OFF");
}

static void on_id_format_toggle(GtkCheckMenuItem *item, gpointer d)
{
    int fmt = GPOINTER_TO_INT(d);
    if (!gtk_check_menu_item_get_active(item))
        return;

    g_app.id_format = fmt;
    gui_refresh_trace_display();
    gui_status_message("ID display format: %s",
        fmt == APP_ID_FORMAT_DEC ? "decimal" : "hexadecimal");
}

static void on_data_format_toggle(GtkCheckMenuItem *item, gpointer d)
{
    int fmt = GPOINTER_TO_INT(d);
    if (!gtk_check_menu_item_get_active(item))
        return;

    g_app.data_format = fmt;
    gui_refresh_trace_display();
    gui_status_message("Data display format: %s",
        fmt == APP_DATA_FORMAT_ASCII ? "ASCII" :
        fmt == APP_DATA_FORMAT_DEC ? "decimal" : "hexadecimal");
}

/* ------------------------------------------------------------------ */
/* Menu helpers                                                         */
/* ------------------------------------------------------------------ */

static GtkWidget *menu_item(const char *label,
                             GCallback cb, gpointer data)
{
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    if (cb) g_signal_connect(item, "activate", cb, data);
    return item;
}

static GtkWidget *stats_menu_row(const char *name, GtkWidget **value_out)
{
    GtkWidget *item = gtk_menu_item_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);
    gtk_container_add(GTK_CONTAINER(item), box);

    GtkWidget *name_lbl = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
    gtk_widget_set_size_request(name_lbl, 96, -1);
    gtk_box_pack_start(GTK_BOX(box), name_lbl, FALSE, FALSE, 0);

    GtkWidget *value_lbl = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(value_lbl), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), value_lbl, TRUE, TRUE, 0);

    if (value_out)
        *value_out = value_lbl;
    return item;
}

static GtkWidget *build_stats_menu_item(void)
{
    GtkWidget *stats_menu = gtk_menu_new();

    gtk_menu_shell_append(GTK_MENU_SHELL(stats_menu),
        stats_menu_row("Interface", &g_gui.lbl_connection));
    gtk_menu_shell_append(GTK_MENU_SHELL(stats_menu),
        stats_menu_row("Bitrate", &g_gui.lbl_bitrate));
    gtk_menu_shell_append(GTK_MENU_SHELL(stats_menu),
        stats_menu_row("Bus Load", &g_gui.lbl_bus_load));
    gtk_menu_shell_append(GTK_MENU_SHELL(stats_menu),
        stats_menu_row("Rx Frames", &g_gui.lbl_rx));
    gtk_menu_shell_append(GTK_MENU_SHELL(stats_menu),
        stats_menu_row("Tx Frames", &g_gui.lbl_tx));
    gtk_menu_shell_append(GTK_MENU_SHELL(stats_menu),
        stats_menu_row("Error Frames", &g_gui.lbl_err));
    gtk_menu_shell_append(GTK_MENU_SHELL(stats_menu),
        stats_menu_row("Bus State", &g_gui.lbl_bus_state));

    GtkWidget *stats_item = gtk_menu_item_new();
    g_gui.lbl_stats_summary = gtk_label_new(
        "Statistics: — | Rx 0 | Tx 0 | Err 0 | Load 0.0%");
    gtk_container_add(GTK_CONTAINER(stats_item), g_gui.lbl_stats_summary);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(stats_item), stats_menu);

    return stats_item;
}

/* ------------------------------------------------------------------ */
/* Menu bar                                                             */
/* ------------------------------------------------------------------ */

static GtkWidget *build_menubar(void)
{
    GtkWidget *bar = gtk_menu_bar_new();

    /* --- File --- */
    GtkWidget *file_menu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
        menu_item("_Connect…",     G_CALLBACK(on_connect),     NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
        menu_item("_Disconnect",   G_CALLBACK(on_disconnect),  NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
        gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
        menu_item("_Start Trace…", G_CALLBACK(on_trace_start), NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
        menu_item("S_top Trace",   G_CALLBACK(on_trace_stop),  NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
        gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
        menu_item("_Quit",         G_CALLBACK(on_quit),        NULL));

    GtkWidget *file_item = gtk_menu_item_new_with_mnemonic("_File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), file_item);

    /* --- CAN --- */
    GtkWidget *can_menu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(can_menu),
        menu_item("_Settings…",    G_CALLBACK(on_settings),    NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(can_menu),
        menu_item("_Transmit…",    G_CALLBACK(on_transmit),    NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(can_menu),
        gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(can_menu),
        menu_item("_Clear Trace",  G_CALLBACK(on_clear),       NULL));

    GtkWidget *can_item = gtk_menu_item_new_with_mnemonic("_CAN");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(can_item), can_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), can_item);

    /* --- View --- */
    GtkWidget *view_menu = gtk_menu_new();
    GtkWidget *dedup_item = gtk_check_menu_item_new_with_mnemonic(
        "_Roll Up Tx Messages");
    g_signal_connect(dedup_item, "toggled",
                     G_CALLBACK(on_dedup_toggle), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), dedup_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
        gtk_separator_menu_item_new());

    GtkWidget *id_menu = gtk_menu_new();
    GtkWidget *id_hex = gtk_radio_menu_item_new_with_mnemonic(
        NULL, "_Hexadecimal");
    GtkWidget *id_dec = gtk_radio_menu_item_new_with_mnemonic_from_widget(
        GTK_RADIO_MENU_ITEM(id_hex), "_Decimal");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(id_hex),
        g_app.id_format == APP_ID_FORMAT_HEX);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(id_dec),
        g_app.id_format == APP_ID_FORMAT_DEC);
    g_signal_connect(id_hex, "toggled",
        G_CALLBACK(on_id_format_toggle),
        GINT_TO_POINTER(APP_ID_FORMAT_HEX));
    g_signal_connect(id_dec, "toggled",
        G_CALLBACK(on_id_format_toggle),
        GINT_TO_POINTER(APP_ID_FORMAT_DEC));
    gtk_menu_shell_append(GTK_MENU_SHELL(id_menu), id_hex);
    gtk_menu_shell_append(GTK_MENU_SHELL(id_menu), id_dec);

    GtkWidget *id_item = gtk_menu_item_new_with_mnemonic("_ID Format");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(id_item), id_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), id_item);

    GtkWidget *data_menu = gtk_menu_new();
    GtkWidget *data_hex = gtk_radio_menu_item_new_with_mnemonic(
        NULL, "_Hexadecimal");
    GtkWidget *data_dec = gtk_radio_menu_item_new_with_mnemonic_from_widget(
        GTK_RADIO_MENU_ITEM(data_hex), "_Decimal");
    GtkWidget *data_ascii = gtk_radio_menu_item_new_with_mnemonic_from_widget(
        GTK_RADIO_MENU_ITEM(data_hex), "_ASCII");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(data_hex),
        g_app.data_format == APP_DATA_FORMAT_HEX);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(data_dec),
        g_app.data_format == APP_DATA_FORMAT_DEC);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(data_ascii),
        g_app.data_format == APP_DATA_FORMAT_ASCII);
    g_signal_connect(data_hex, "toggled",
        G_CALLBACK(on_data_format_toggle),
        GINT_TO_POINTER(APP_DATA_FORMAT_HEX));
    g_signal_connect(data_dec, "toggled",
        G_CALLBACK(on_data_format_toggle),
        GINT_TO_POINTER(APP_DATA_FORMAT_DEC));
    g_signal_connect(data_ascii, "toggled",
        G_CALLBACK(on_data_format_toggle),
        GINT_TO_POINTER(APP_DATA_FORMAT_ASCII));
    gtk_menu_shell_append(GTK_MENU_SHELL(data_menu), data_hex);
    gtk_menu_shell_append(GTK_MENU_SHELL(data_menu), data_dec);
    gtk_menu_shell_append(GTK_MENU_SHELL(data_menu), data_ascii);

    GtkWidget *data_item = gtk_menu_item_new_with_mnemonic("_Data Format");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(data_item), data_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), data_item);

    GtkWidget *view_item = gtk_menu_item_new_with_mnemonic("_View");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_item), view_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), view_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(bar), build_stats_menu_item());

    /* --- Help --- */
    GtkWidget *help_menu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(help_menu),
        menu_item("_About",        G_CALLBACK(on_about),       NULL));

    GtkWidget *help_item = gtk_menu_item_new_with_mnemonic("_Help");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_item), help_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), help_item);

    return bar;
}

/* ------------------------------------------------------------------ */
/* Toolbar                                                              */
/* ------------------------------------------------------------------ */

static GtkWidget *make_toolbtn(const char *icon, const char *tooltip,
                                GCallback cb)
{
    GtkToolItem *btn = gtk_tool_button_new(
        gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_SMALL_TOOLBAR),
        NULL);
    gtk_tool_item_set_tooltip_text(btn, tooltip);
    if (cb) g_signal_connect(btn, "clicked", cb, NULL);
    return GTK_WIDGET(btn);
}

static GtkWidget *build_toolbar(void)
{
    GtkWidget *tb = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(tb), GTK_TOOLBAR_BOTH_HORIZ);

    g_gui.toolbar_connect = make_toolbtn(
        "network-wired", "Connect (F5)", G_CALLBACK(on_connect));
    gtk_toolbar_insert(GTK_TOOLBAR(tb),
                       GTK_TOOL_ITEM(g_gui.toolbar_connect), -1);

    g_gui.toolbar_disconnect = make_toolbtn(
        "network-offline", "Disconnect (F6)", G_CALLBACK(on_disconnect));
    gtk_widget_set_sensitive(g_gui.toolbar_disconnect, FALSE);
    gtk_toolbar_insert(GTK_TOOLBAR(tb),
                       GTK_TOOL_ITEM(g_gui.toolbar_disconnect), -1);

    GtkToolItem *sep1 = gtk_separator_tool_item_new();
    gtk_toolbar_insert(GTK_TOOLBAR(tb), sep1, -1);

    GtkWidget *clear_btn = make_toolbtn(
        "edit-clear", "Clear Trace", G_CALLBACK(on_clear));
    gtk_toolbar_insert(GTK_TOOLBAR(tb), GTK_TOOL_ITEM(clear_btn), -1);

    GtkToolItem *sep2 = gtk_separator_tool_item_new();
    gtk_toolbar_insert(GTK_TOOLBAR(tb), sep2, -1);

    g_gui.toolbar_trace_start = make_toolbtn(
        "media-record", "Start Trace", G_CALLBACK(on_trace_start));
    gtk_widget_set_sensitive(g_gui.toolbar_trace_start, FALSE);
    gtk_toolbar_insert(GTK_TOOLBAR(tb),
                       GTK_TOOL_ITEM(g_gui.toolbar_trace_start), -1);

    g_gui.toolbar_trace_stop = make_toolbtn(
        "media-playback-stop", "Stop Trace", G_CALLBACK(on_trace_stop));
    gtk_widget_set_sensitive(g_gui.toolbar_trace_stop, FALSE);
    gtk_toolbar_insert(GTK_TOOLBAR(tb),
                       GTK_TOOL_ITEM(g_gui.toolbar_trace_stop), -1);

    GtkToolItem *sep3 = gtk_separator_tool_item_new();
    gtk_toolbar_insert(GTK_TOOLBAR(tb), sep3, -1);

    GtkWidget *tx_btn = make_toolbtn(
        "mail-send", "Transmit Message…", G_CALLBACK(on_transmit));
    gtk_toolbar_insert(GTK_TOOLBAR(tb), GTK_TOOL_ITEM(tx_btn), -1);

    GtkWidget *cfg_btn = make_toolbtn(
        "preferences-system", "Connection Settings…",
        G_CALLBACK(on_settings));
    gtk_toolbar_insert(GTK_TOOLBAR(tb), GTK_TOOL_ITEM(cfg_btn), -1);

    return tb;
}

/* ------------------------------------------------------------------ */
/* Keyboard shortcuts                                                   */
/* ------------------------------------------------------------------ */

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    switch (ev->keyval) {
    case GDK_KEY_F5: on_connect(NULL, NULL);     return TRUE;
    case GDK_KEY_F6: on_disconnect(NULL, NULL);  return TRUE;
    case GDK_KEY_Delete:
        if (ev->state & GDK_CONTROL_MASK) {
            on_clear(NULL, NULL);
            return TRUE;
        }
        break;
    default: break;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Inline transmit panel (shown as a notebook tab in the main window)  */
/* ------------------------------------------------------------------ */

static int parse_hex_u32_strict(const char *s, uint32_t max, uint32_t *out)
{
    if (!s || !out) return -1;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0' || *s == '+' || *s == '-') return -1;

    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (errno == ERANGE || end == s || v > max) return -1;

    while (isspace((unsigned char)*end)) end++;
    if (*end != '\0') return -1;

    *out = (uint32_t)v;
    return 0;
}

static tx_panel_row_t *txp_row_from_data(gpointer data)
{
    int row = GPOINTER_TO_INT(data);
    if (row < 0 || row >= TX_PANEL_MAX_ROWS)
        return NULL;
    return &s_txp.rows[row];
}

static void txp_set_status(tx_panel_row_t *row,
                           const char *color,
                           const char *msg)
{
    if (!row || !GTK_IS_LABEL(row->status_lbl))
        return;

    if (!color || !msg || msg[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(row->status_lbl), msg ? msg : "");
        return;
    }

    char markup[192];
    snprintf(markup, sizeof(markup),
             "<span color='%s'>%s</span>", color, msg);
    gtk_label_set_markup(GTK_LABEL(row->status_lbl), markup);
}

static void txp_clear_row(tx_panel_row_t *row)
{
    if (!row) return;

    gtk_entry_set_text(GTK_ENTRY(row->id_entry), "001");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->ext_check), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->rtr_check), FALSE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(row->dlc_spin), 8);
    for (int i = 0; i < TX_PANEL_DATA_COLS; i++)
        gtk_entry_set_text(GTK_ENTRY(row->data_entry[i]), "00");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(row->interval_spin), 100);
    txp_stop_cyclic_row(row, "Cleared");
}

static void txp_clear_all(void)
{
    for (int i = 0; i < s_txp.count; i++)
        txp_clear_row(&s_txp.rows[i]);
}

static void txp_add_row(void)
{
    if (s_txp.count >= TX_PANEL_MAX_ROWS)
        return;

    int row_index = s_txp.count;
    int grid_row  = row_index + 1;          /* grid row 0 holds the header */
    GtkGrid *grid = GTK_GRID(s_txp.rows_box);
    tx_panel_row_t *row = &s_txp.rows[row_index];

    row->num_lbl = gtk_label_new(NULL);
    char num[16];
    snprintf(num, sizeof(num), "%d", row_index + 1);
    gtk_label_set_text(GTK_LABEL(row->num_lbl), num);
    gtk_grid_attach(grid, row->num_lbl, TXG_COL_NUM, grid_row, 1, 1);

    row->id_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(row->id_entry), 8);
    gtk_entry_set_text(GTK_ENTRY(row->id_entry), "001");
    gtk_entry_set_width_chars(GTK_ENTRY(row->id_entry), 5);
    gtk_widget_set_hexpand(row->id_entry, TRUE);
    gtk_grid_attach(grid, row->id_entry, TXG_COL_ID, grid_row, 1, 1);

    row->ext_check = gtk_check_button_new();
    gtk_widget_set_tooltip_text(row->ext_check, "Extended ID");
    gtk_widget_set_halign(row->ext_check, GTK_ALIGN_CENTER);
    gtk_grid_attach(grid, row->ext_check, TXG_COL_EXT, grid_row, 1, 1);

    row->rtr_check = gtk_check_button_new();
    gtk_widget_set_tooltip_text(row->rtr_check, "Remote frame");
    gtk_widget_set_halign(row->rtr_check, GTK_ALIGN_CENTER);
    g_signal_connect(row->rtr_check, "toggled",
                     G_CALLBACK(txp_rtr_toggled), GINT_TO_POINTER(row_index));
    gtk_grid_attach(grid, row->rtr_check, TXG_COL_RTR, grid_row, 1, 1);

    GtkAdjustment *dlc_adj = gtk_adjustment_new(8, 0, 8, 1, 1, 0);
    row->dlc_spin = gtk_spin_button_new(dlc_adj, 1, 0);
    g_signal_connect(row->dlc_spin, "value-changed",
                     G_CALLBACK(txp_dlc_changed), GINT_TO_POINTER(row_index));
    gtk_grid_attach(grid, row->dlc_spin, TXG_COL_DLC, grid_row, 1, 1);

    for (int i = 0; i < TX_PANEL_DATA_COLS; i++) {
        row->data_entry[i] = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(row->data_entry[i]), 2);
        gtk_entry_set_text(GTK_ENTRY(row->data_entry[i]), "00");
        gtk_entry_set_width_chars(GTK_ENTRY(row->data_entry[i]), 2);
        gtk_widget_set_hexpand(row->data_entry[i], TRUE);
        gtk_grid_attach(grid, row->data_entry[i],
                        TXG_COL_DATA0 + i, grid_row, 1, 1);
    }

    GtkAdjustment *int_adj = gtk_adjustment_new(100, 1, 60000, 10, 100, 0);
    row->interval_spin = gtk_spin_button_new(int_adj, 10, 0);
    gtk_grid_attach(grid, row->interval_spin, TXG_COL_INTERVAL, grid_row, 1, 1);

    row->send_btn = gtk_button_new_with_label("Send");
    g_signal_connect(row->send_btn, "clicked",
                     G_CALLBACK(txp_send_once), GINT_TO_POINTER(row_index));
    gtk_grid_attach(grid, row->send_btn, TXG_COL_SEND, grid_row, 1, 1);

    row->start_btn = gtk_button_new_with_label("Start");
    g_signal_connect(row->start_btn, "clicked",
                     G_CALLBACK(txp_start_cyclic), GINT_TO_POINTER(row_index));
    gtk_grid_attach(grid, row->start_btn, TXG_COL_START, grid_row, 1, 1);

    row->stop_btn = gtk_button_new_with_label("Stop");
    g_signal_connect(row->stop_btn, "clicked",
                     G_CALLBACK(txp_stop_cyclic), GINT_TO_POINTER(row_index));
    gtk_widget_set_sensitive(row->stop_btn, FALSE);
    gtk_grid_attach(grid, row->stop_btn, TXG_COL_STOP, grid_row, 1, 1);

    row->status_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(row->status_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(row->status_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(row->status_lbl, 80, -1);
    gtk_grid_attach(grid, row->status_lbl, TXG_COL_STATUS, grid_row, 1, 1);

    txp_update_data_fields(row);
    s_txp.count++;
    gtk_widget_show_all(s_txp.rows_box);
}

static void txp_remove_last_row(void)
{
    if (s_txp.count <= 0) return;
    tx_panel_row_t *row = &s_txp.rows[s_txp.count - 1];
    if (row->cyclic_id) {
        g_source_remove(row->cyclic_id);
        row->cyclic_id = 0;
    }
    /* Destroy every widget that makes up this grid row. */
    GtkWidget *widgets[] = {
        row->num_lbl, row->id_entry, row->ext_check, row->rtr_check,
        row->dlc_spin, row->interval_spin, row->send_btn,
        row->start_btn, row->stop_btn, row->status_lbl,
    };
    for (size_t i = 0; i < sizeof(widgets) / sizeof(widgets[0]); i++)
        if (widgets[i]) gtk_widget_destroy(widgets[i]);
    for (int i = 0; i < TX_PANEL_DATA_COLS; i++)
        if (row->data_entry[i]) gtk_widget_destroy(row->data_entry[i]);
    memset(row, 0, sizeof(*row));
    s_txp.count--;
}

static void txp_set_error(tx_panel_row_t *row, const char *msg)
{
    txp_set_status(row, "red", msg);
}

static void txp_update_data_fields(tx_panel_row_t *row)
{
    if (!row || !GTK_IS_SPIN_BUTTON(row->dlc_spin))
        return;

    gboolean rtr = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(row->rtr_check));
    int dlc = gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(row->dlc_spin));

    for (int i = 0; i < TX_PANEL_DATA_COLS; i++)
        gtk_widget_set_sensitive(row->data_entry[i], !rtr && i < dlc);
}

static gboolean txp_build_frame(tx_panel_row_t *row, can_msg_t *msg)
{
    if (!row || !msg)
        return FALSE;

    memset(msg, 0, sizeof(*msg));

    msg->is_extended = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(row->ext_check));
    msg->is_remote = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(row->rtr_check));
    msg->dlc = (uint8_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(row->dlc_spin));

    const char *ids = gtk_entry_get_text(GTK_ENTRY(row->id_entry));
    uint32_t id_max = msg->is_extended ? TX_EXT_ID_MAX : TX_STD_ID_MAX;
    if (parse_hex_u32_strict(ids, id_max, &msg->id) != 0) {
        txp_set_error(row, msg->is_extended
            ? "Invalid 29-bit ID"
            : "Invalid 11-bit ID");
        return FALSE;
    }

    if (!msg->is_remote) {
        for (int i = 0; i < msg->dlc && i < TX_PANEL_DATA_COLS; i++) {
            const char *ds = gtk_entry_get_text(
                GTK_ENTRY(row->data_entry[i]));
            uint32_t value = 0;
            if (parse_hex_u32_strict(ds, TX_BYTE_MAX, &value) != 0) {
                char err[64];
                snprintf(err, sizeof(err), "Invalid data byte [%d]", i);
                txp_set_error(row, err);
                return FALSE;
            }
            msg->data[i] = (uint8_t)value;
        }
    }

    return TRUE;
}

static gboolean txp_queue_frame(int row_index, gboolean cyclic)
{
    tx_panel_row_t *row = txp_row_from_data(GINT_TO_POINTER(row_index));
    if (!row)
        return FALSE;

    if (!g_app.connected) {
        txp_set_error(row, "Not connected");
        return FALSE;
    }

    can_msg_t *msg = calloc(1, sizeof(can_msg_t));
    if (!msg) {
        txp_set_error(row, "Out of memory");
        return FALSE;
    }

    if (!txp_build_frame(row, msg)) {
        free(msg);
        return FALSE;
    }

    g_async_queue_push(g_app.tx_queue, msg);
    if (!cyclic)
        txp_set_status(row, "green", "Queued");
    return TRUE;
}

static void txp_stop_cyclic_row(tx_panel_row_t *row, const char *status)
{
    if (!row)
        return;

    if (row->cyclic_id) {
        g_source_remove(row->cyclic_id);
        row->cyclic_id = 0;
    }
    if (GTK_IS_WIDGET(row->start_btn))
        gtk_widget_set_sensitive(row->start_btn, TRUE);
    if (GTK_IS_WIDGET(row->stop_btn))
        gtk_widget_set_sensitive(row->stop_btn, FALSE);
    if (status)
        txp_set_status(row, NULL, status);
}

static void txp_stop_all_cyclic(void)
{
    for (int i = 0; i < TX_PANEL_MAX_ROWS; i++)
        txp_stop_cyclic_row(&s_txp.rows[i], "");
}

static gboolean txp_cyclic_tick(gpointer data)
{
    int row_index = GPOINTER_TO_INT(data);
    tx_panel_row_t *row = txp_row_from_data(data);
    if (!row || !row->cyclic_id)
        return G_SOURCE_REMOVE;

    if (!g_app.connected) {
        txp_stop_cyclic_row(row, "Disconnected");
        return G_SOURCE_REMOVE;
    }

    txp_queue_frame(row_index, TRUE);
    return G_SOURCE_CONTINUE;
}

static void txp_send_once(GtkWidget *w, gpointer d)
{
    (void)w;
    txp_queue_frame(GPOINTER_TO_INT(d), FALSE);
}

static void txp_start_cyclic(GtkWidget *w, gpointer d)
{
    (void)w;
    int row_index = GPOINTER_TO_INT(d);
    tx_panel_row_t *row = txp_row_from_data(d);
    if (!row || row->cyclic_id)
        return;

    if (!g_app.connected) {
        txp_set_error(row, "Not connected");
        return;
    }

    can_msg_t frame;
    if (!txp_build_frame(row, &frame))
        return;

    guint ms = (guint)gtk_spin_button_get_value_as_int(
                   GTK_SPIN_BUTTON(row->interval_spin));
    if (ms < 1)
        ms = 100;

    row->cyclic_id = g_timeout_add(ms, txp_cyclic_tick,
                                   GINT_TO_POINTER(row_index));
    gtk_widget_set_sensitive(row->start_btn, FALSE);
    gtk_widget_set_sensitive(row->stop_btn, TRUE);
    txp_queue_frame(row_index, TRUE);
    txp_set_status(row, "blue", "Cyclic running");
}

static void txp_stop_cyclic(GtkWidget *w, gpointer d)
{
    (void)w;
    tx_panel_row_t *row = txp_row_from_data(d);
    txp_stop_cyclic_row(row, "");
}

static void txp_dlc_changed(GtkSpinButton *btn, gpointer d)
{
    (void)btn;
    txp_update_data_fields(txp_row_from_data(d));
}

static void txp_rtr_toggled(GtkToggleButton *btn, gpointer d)
{
    (void)btn;
    txp_update_data_fields(txp_row_from_data(d));
}

static GtkWidget *txp_header_label(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_margin_start(label, 3);
    gtk_widget_set_margin_end(label, 3);
    gtk_label_set_xalign(GTK_LABEL(label), 0.5f);
    return label;
}

static GtkWidget *create_transmit_panel(void)
{
    GtkWidget *frame = gtk_frame_new("Transmit Messages");
    gtk_widget_set_margin_start(frame, 4);
    gtk_widget_set_margin_end(frame, 4);
    gtk_widget_set_margin_top(frame, 4);
    gtk_widget_set_margin_bottom(frame, 4);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(outer, 8);
    gtk_widget_set_margin_end(outer, 8);
    gtk_widget_set_margin_top(outer, 6);
    gtk_widget_set_margin_bottom(outer, 6);
    gtk_container_add(GTK_CONTAINER(frame), outer);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(outer), top, FALSE, FALSE, 0);

    GtkWidget *control_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(top), control_btn_box, FALSE, FALSE, 0);

    GtkWidget *clear_btn = gtk_button_new_with_label("Clear Rows");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(txp_clear_all), NULL);
    gtk_box_pack_start(GTK_BOX(control_btn_box), clear_btn, FALSE, FALSE, 0);

    GtkWidget *add_btn = gtk_button_new_with_label("Add Row");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(txp_add_row), NULL);
    gtk_box_pack_start(GTK_BOX(control_btn_box), add_btn, FALSE, FALSE, 0);

    GtkWidget *remove_btn = gtk_button_new_with_label("Remove Last");
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(txp_remove_last_row), NULL);
    gtk_box_pack_start(GTK_BOX(control_btn_box), remove_btn, FALSE, FALSE, 0);

    GtkWidget *advanced_btn = gtk_button_new_with_label("Advanced TX...");
    g_signal_connect(advanced_btn, "clicked", G_CALLBACK(on_transmit), NULL);
    gtk_box_pack_end(GTK_BOX(top), advanced_btn, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    /* Never scroll horizontally: the grid stretches to the panel width so the
     * Send/Start/Stop buttons are always visible and the columns resize by
     * ratio with the window.  Vertical scrolling kicks in only for many rows. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

    s_txp.rows_box = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(s_txp.rows_box), 6);
    gtk_grid_set_row_spacing(GTK_GRID(s_txp.rows_box), 4);
    gtk_widget_set_hexpand(s_txp.rows_box, TRUE);

    /* Header row (grid row 0) – aligned with every transmit row below. */
    struct { int col; const char *txt; gboolean expand; } hdr[] = {
        { TXG_COL_NUM,      "#",          FALSE },
        { TXG_COL_ID,       "CAN-ID",     TRUE  },
        { TXG_COL_EXT,      "Ext",        FALSE },
        { TXG_COL_RTR,      "RTR",        FALSE },
        { TXG_COL_DLC,      "DLC",        FALSE },
        { TXG_COL_INTERVAL, "Cycle (ms)", FALSE },
        { TXG_COL_STATUS,   "Status",     FALSE },
    };
    for (size_t i = 0; i < sizeof(hdr) / sizeof(hdr[0]); i++) {
        GtkWidget *h = txp_header_label(hdr[i].txt);
        gtk_widget_set_hexpand(h, hdr[i].expand);
        gtk_grid_attach(GTK_GRID(s_txp.rows_box), h, hdr[i].col, 0, 1, 1);
    }
    for (int i = 0; i < TX_PANEL_DATA_COLS; i++) {
        char b[4];
        snprintf(b, sizeof(b), "%d", i);
        GtkWidget *h = txp_header_label(b);
        gtk_widget_set_hexpand(h, TRUE);
        gtk_grid_attach(GTK_GRID(s_txp.rows_box), h,
                        TXG_COL_DATA0 + i, 0, 1, 1);
    }

    gtk_container_add(GTK_CONTAINER(scroll), s_txp.rows_box);

    /* Initialize with one default row */
    s_txp.count = 0;
    txp_add_row();

    return frame;
}


/* ------------------------------------------------------------------ */
/* Main window                                                          */
/* ------------------------------------------------------------------ */

GtkWidget *gui_create_main_window(GtkApplication *app)
{
    GtkWidget *window = gtk_application_window_new(app);
    g_gui.window = window;

    gtk_window_set_title(GTK_WINDOW(window), "PCAN-View Linux");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 700);

    /* On window close: disconnect CAN then allow default destroy */
    g_signal_connect(window, "delete-event",
                     G_CALLBACK(on_window_delete), NULL);
    g_signal_connect(window, "key-press-event",
                     G_CALLBACK(on_key_press), NULL);

    /* Outer vertical box */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    /* Menu bar */
    GtkWidget *menubar = build_menubar();
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);

    /* Toolbar */
    GtkWidget *toolbar = build_toolbar();
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    /* Separator */
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    /* Paned: receive trace on top, transmit rows on bottom */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    GtkWidget *trace_scroll = create_trace_view();
    gtk_paned_pack1(GTK_PANED(paned), trace_scroll, TRUE, TRUE);

    GtkWidget *tx_panel = create_transmit_panel();
    gtk_paned_pack2(GTK_PANED(paned), tx_panel, TRUE, TRUE);

    /* Status bar */
    g_gui.statusbar    = gtk_statusbar_new();
    g_gui.statusbar_ctx = gtk_statusbar_get_context_id(
        GTK_STATUSBAR(g_gui.statusbar), "main");
    gtk_box_pack_start(GTK_BOX(vbox),
                       g_gui.statusbar, FALSE, FALSE, 0);

    gtk_statusbar_push(GTK_STATUSBAR(g_gui.statusbar),
                       g_gui.statusbar_ctx,
                       "Ready – use File > Connect or press F5 to connect.");

    /* ---- Footer bar ---- */
    GtkWidget *footer_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), footer_sep, FALSE, FALSE, 0);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start(footer, 8);
    gtk_widget_set_margin_end(footer, 8);
    gtk_widget_set_margin_top(footer, 3);
    gtk_widget_set_margin_bottom(footer, 3);
    gtk_box_pack_start(GTK_BOX(vbox), footer, FALSE, FALSE, 0);

    GtkWidget *footer_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(footer_lbl),
        "<small><b>SS Electronics</b>  |  "
        "Author: Subhajit Roy  |  "
        "<a href=\"mailto:subhajitroy005@gmail.com\">"
        "subhajitroy005@gmail.com</a>  |  "
        "License: Apache-2.0</small>");
    gtk_label_set_xalign(GTK_LABEL(footer_lbl), 0.5f);
    gtk_box_pack_start(GTK_BOX(footer), footer_lbl, TRUE, TRUE, 0);

    gtk_widget_show_all(window);
    gui_update_stats();
    return window;
}

/* ------------------------------------------------------------------ */
/* About dialog                                                         */
/* ------------------------------------------------------------------ */

void gui_show_about_dialog(GtkWidget *parent)
{
    GtkAboutDialog *dlg = GTK_ABOUT_DIALOG(gtk_about_dialog_new());
    gtk_about_dialog_set_program_name(dlg, "PCAN-View Linux");
    gtk_about_dialog_set_version(dlg, "1.0.0");
    gtk_about_dialog_set_comments(dlg,
        "Open-source CAN bus monitor and analyser for Linux.\n"
        "Uses the Linux SocketCAN subsystem (PF_CAN).\n"
        "Inspired by PEAK-System PCAN-View.");
    gtk_about_dialog_set_license_type(dlg, GTK_LICENSE_APACHE_2_0);
    gtk_about_dialog_set_website(dlg,
        "https://www.peak-system.com/fileadmin/media/linux/index.php");
    gtk_about_dialog_set_website_label(dlg, "PEAK-System Linux drivers");

    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(dlg),
                                     GTK_WINDOW(parent));
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(GTK_WIDGET(dlg));
}
