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
 *      │  └─ stats panel (GtkFrame)
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
extern GtkWidget *create_stats_panel(void);

/* Forward-declared here; defined in the transmit-panel section below */
#define TX_PANEL_DATA_COLS 8
#define TX_STD_ID_MAX      0x7FFu
#define TX_EXT_ID_MAX      0x1FFFFFFFu
#define TX_BYTE_MAX        0xFFu

static struct {
    GtkWidget *id_entry;
    GtkWidget *ext_check;
    GtkWidget *rtr_check;
    GtkWidget *dlc_spin;
    GtkWidget *data_entry[TX_PANEL_DATA_COLS];
    GtkWidget *interval_spin;
    GtkWidget *start_btn;
    GtkWidget *stop_btn;
    GtkWidget *status_lbl;
    guint      cyclic_id;
} s_txp;

/* ------------------------------------------------------------------ */
/* Toolbar / menu callbacks                                             */
/* ------------------------------------------------------------------ */

static gboolean on_window_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    /* Stop cyclic timer before disconnect to prevent callbacks on dead widgets */
    if (s_txp.cyclic_id) { g_source_remove(s_txp.cyclic_id); s_txp.cyclic_id = 0; }
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

static void txp_set_error(const char *msg)
{
    char markup[192];
    snprintf(markup, sizeof(markup),
             "<span color='red'>%s</span>", msg);
    gtk_label_set_markup(GTK_LABEL(s_txp.status_lbl), markup);
}

static gboolean txp_build_frame(can_msg_t *msg)
{
    memset(msg, 0, sizeof(*msg));

    msg->is_extended = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_txp.ext_check));
    msg->is_remote = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_txp.rtr_check));
    msg->dlc = (uint8_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_txp.dlc_spin));

    const char *ids = gtk_entry_get_text(GTK_ENTRY(s_txp.id_entry));
    uint32_t id_max = msg->is_extended ? TX_EXT_ID_MAX : TX_STD_ID_MAX;
    if (parse_hex_u32_strict(ids, id_max, &msg->id) != 0) {
        txp_set_error(msg->is_extended
            ? "Invalid 29-bit ID"
            : "Invalid 11-bit ID");
        return FALSE;
    }

    if (!msg->is_remote) {
        for (int i = 0; i < msg->dlc && i < TX_PANEL_DATA_COLS; i++) {
            const char *ds = gtk_entry_get_text(
                GTK_ENTRY(s_txp.data_entry[i]));
            uint32_t value = 0;
            if (parse_hex_u32_strict(ds, TX_BYTE_MAX, &value) != 0) {
                char err[64];
                snprintf(err, sizeof(err), "Invalid data byte [%d]", i);
                txp_set_error(err);
                return FALSE;
            }
            msg->data[i] = (uint8_t)value;
        }
    }

    return TRUE;
}

static gboolean txp_cyclic_tick(gpointer data)
{
    (void)data;
    if (!g_app.connected || !s_txp.cyclic_id) return G_SOURCE_REMOVE;

    if (!GTK_IS_WIDGET(s_txp.id_entry)) { s_txp.cyclic_id = 0; return G_SOURCE_REMOVE; }

    can_msg_t *msg = calloc(1, sizeof(can_msg_t));
    if (!msg) return G_SOURCE_CONTINUE;

    if (!txp_build_frame(msg)) {
        free(msg);
        return G_SOURCE_CONTINUE;
    }

    g_async_queue_push(g_app.tx_queue, msg);
    return G_SOURCE_CONTINUE;
}

static void txp_send_once(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!g_app.connected) {
        gtk_label_set_markup(GTK_LABEL(s_txp.status_lbl),
                             "<span color='red'>Not connected</span>");
        return;
    }
    can_msg_t frame;
    if (!txp_build_frame(&frame)) {
        return;
    }

    can_msg_t *msg = malloc(sizeof(can_msg_t));
    if (!msg) return;
    *msg = frame;

    g_async_queue_push(g_app.tx_queue, msg);
    gtk_label_set_markup(GTK_LABEL(s_txp.status_lbl),
                         "<span color='green'>Queued</span>");
}

static void txp_start_cyclic(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (s_txp.cyclic_id) return;
    guint ms = (guint)gtk_spin_button_get_value_as_int(
                   GTK_SPIN_BUTTON(s_txp.interval_spin));
    if (ms < 1) ms = 100;
    s_txp.cyclic_id = g_timeout_add(ms, txp_cyclic_tick, NULL);
    gtk_widget_set_sensitive(s_txp.start_btn, FALSE);
    gtk_widget_set_sensitive(s_txp.stop_btn,  TRUE);
    gtk_label_set_markup(GTK_LABEL(s_txp.status_lbl),
                         "<span color='blue'>Cyclic running</span>");
}

static void txp_stop_cyclic(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (s_txp.cyclic_id) { g_source_remove(s_txp.cyclic_id); s_txp.cyclic_id = 0; }
    gtk_widget_set_sensitive(s_txp.start_btn, TRUE);
    gtk_widget_set_sensitive(s_txp.stop_btn,  FALSE);
    gtk_label_set_text(GTK_LABEL(s_txp.status_lbl), "");
}

static void txp_dlc_changed(GtkSpinButton *btn, gpointer d)
{
    (void)d;
    int dlc = gtk_spin_button_get_value_as_int(btn);
    for (int i = 0; i < TX_PANEL_DATA_COLS; i++)
        gtk_widget_set_sensitive(s_txp.data_entry[i], i < dlc);
}

static void txp_rtr_toggled(GtkToggleButton *btn, gpointer d)
{
    (void)d;
    gboolean rtr = gtk_toggle_button_get_active(btn);
    for (int i = 0; i < TX_PANEL_DATA_COLS; i++)
        gtk_widget_set_sensitive(s_txp.data_entry[i], !rtr);
}

static GtkWidget *create_transmit_panel(void)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(outer, 8);
    gtk_widget_set_margin_end(outer, 8);
    gtk_widget_set_margin_top(outer, 6);
    gtk_widget_set_margin_bottom(outer, 6);

    /* ---- Frame setup row ---- */
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(outer), row1, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(row1), gtk_label_new("ID (hex):"),
                       FALSE, FALSE, 0);
    s_txp.id_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(s_txp.id_entry), 8);
    gtk_entry_set_text(GTK_ENTRY(s_txp.id_entry), "001");
    gtk_entry_set_width_chars(GTK_ENTRY(s_txp.id_entry), 9);
    gtk_box_pack_start(GTK_BOX(row1), s_txp.id_entry, FALSE, FALSE, 0);

    s_txp.ext_check = gtk_check_button_new_with_label("Ext (29-bit)");
    gtk_box_pack_start(GTK_BOX(row1), s_txp.ext_check, FALSE, FALSE, 0);

    s_txp.rtr_check = gtk_check_button_new_with_label("RTR");
    g_signal_connect(s_txp.rtr_check, "toggled",
                     G_CALLBACK(txp_rtr_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(row1), s_txp.rtr_check, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(row1), gtk_label_new("DLC:"),
                       FALSE, FALSE, 0);
    GtkAdjustment *dlc_adj = gtk_adjustment_new(8, 0, 8, 1, 1, 0);
    s_txp.dlc_spin = gtk_spin_button_new(dlc_adj, 1, 0);
    gtk_widget_set_size_request(s_txp.dlc_spin, 55, -1);
    g_signal_connect(s_txp.dlc_spin, "value-changed",
                     G_CALLBACK(txp_dlc_changed), NULL);
    gtk_box_pack_start(GTK_BOX(row1), s_txp.dlc_spin, FALSE, FALSE, 0);

    /* ---- Data bytes row ---- */
    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(outer), row2, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row2), gtk_label_new("Data:"),
                       FALSE, FALSE, 0);

    for (int i = 0; i < TX_PANEL_DATA_COLS; i++) {
        char lbuf[6];
        snprintf(lbuf, sizeof(lbuf), "[%d]", i);
        gtk_box_pack_start(GTK_BOX(row2), gtk_label_new(lbuf), FALSE, FALSE, 0);
        s_txp.data_entry[i] = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(s_txp.data_entry[i]), 2);
        gtk_entry_set_text(GTK_ENTRY(s_txp.data_entry[i]), "00");
        gtk_entry_set_width_chars(GTK_ENTRY(s_txp.data_entry[i]), 3);
        gtk_box_pack_start(GTK_BOX(row2), s_txp.data_entry[i], FALSE, FALSE, 0);
    }

    /* ---- Actions row ---- */
    GtkWidget *row3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(outer), row3, FALSE, FALSE, 0);

    GtkWidget *send_btn = gtk_button_new_with_label("Send Once");
    g_signal_connect(send_btn, "clicked", G_CALLBACK(txp_send_once), NULL);
    gtk_box_pack_start(GTK_BOX(row3), send_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(row3),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                       FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(row3),
                       gtk_label_new("Interval (ms):"), FALSE, FALSE, 0);
    GtkAdjustment *int_adj = gtk_adjustment_new(100, 1, 60000, 10, 100, 0);
    s_txp.interval_spin = gtk_spin_button_new(int_adj, 10, 0);
    gtk_widget_set_size_request(s_txp.interval_spin, 80, -1);
    gtk_box_pack_start(GTK_BOX(row3), s_txp.interval_spin, FALSE, FALSE, 0);

    s_txp.start_btn = gtk_button_new_with_label("Start Cyclic");
    g_signal_connect(s_txp.start_btn, "clicked",
                     G_CALLBACK(txp_start_cyclic), NULL);
    gtk_box_pack_start(GTK_BOX(row3), s_txp.start_btn, FALSE, FALSE, 0);

    s_txp.stop_btn = gtk_button_new_with_label("Stop Cyclic");
    g_signal_connect(s_txp.stop_btn, "clicked",
                     G_CALLBACK(txp_stop_cyclic), NULL);
    gtk_widget_set_sensitive(s_txp.stop_btn, FALSE);
    gtk_box_pack_start(GTK_BOX(row3), s_txp.stop_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(row3),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                       FALSE, FALSE, 4);

    GtkWidget *adv_btn = gtk_button_new_with_label("Advanced TX…");
    g_signal_connect(adv_btn, "clicked", G_CALLBACK(on_transmit), NULL);
    gtk_box_pack_start(GTK_BOX(row3), adv_btn, FALSE, FALSE, 0);

    s_txp.status_lbl = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(row3), s_txp.status_lbl, FALSE, FALSE, 0);

    return outer;
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

    /* Paned: receive trace on top, notebook (stats + transmit) on bottom */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    GtkWidget *trace_scroll = create_trace_view();
    gtk_widget_set_size_request(trace_scroll, -1, 380);
    gtk_paned_pack1(GTK_PANED(paned), trace_scroll, TRUE, FALSE);

    /* Bottom notebook: Statistics | Transmit */
    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
    gtk_paned_pack2(GTK_PANED(paned), notebook, FALSE, FALSE);

    GtkWidget *stats = create_stats_panel();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), stats,
                             gtk_label_new("Statistics"));

    GtkWidget *tx_panel = create_transmit_panel();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tx_panel,
                             gtk_label_new("Transmit"));

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
        "License: GPL 3.0</small>");
    gtk_label_set_xalign(GTK_LABEL(footer_lbl), 0.5f);
    gtk_box_pack_start(GTK_BOX(footer), footer_lbl, TRUE, TRUE, 0);

    gtk_widget_show_all(window);
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
    gtk_about_dialog_set_license_type(dlg, GTK_LICENSE_GPL_3_0);
    gtk_about_dialog_set_website(dlg,
        "https://www.peak-system.com/fileadmin/media/linux/index.php");
    gtk_about_dialog_set_website_label(dlg, "PEAK-System Linux drivers");

    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(dlg),
                                     GTK_WINDOW(parent));
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(GTK_WIDGET(dlg));
}
