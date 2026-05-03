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

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/app_state.h"
#include "../inc/can_message.h"

/* Declared in message_view.c */
extern GtkWidget *create_trace_view(void);
extern GtkWidget *create_stats_panel(void);

/* ------------------------------------------------------------------ */
/* Toolbar / menu callbacks                                             */
/* ------------------------------------------------------------------ */

static gboolean on_window_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w; (void)e; (void)d;
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
    gui_status_message("Deduplication: %s",
                       g_app.dedup_mode ? "ON" : "OFF");
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
        "_Deduplicate Messages");
    g_signal_connect(dedup_item, "toggled",
                     G_CALLBACK(on_dedup_toggle), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), dedup_item);

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

    /* Paned: trace on top, stats on bottom */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    GtkWidget *trace_scroll = create_trace_view();
    gtk_widget_set_size_request(trace_scroll, -1, 400);
    gtk_paned_pack1(GTK_PANED(paned), trace_scroll, TRUE, FALSE);

    GtkWidget *stats = create_stats_panel();
    gtk_paned_pack2(GTK_PANED(paned), stats, FALSE, FALSE);

    /* Status bar */
    g_gui.statusbar    = gtk_statusbar_new();
    g_gui.statusbar_ctx = gtk_statusbar_get_context_id(
        GTK_STATUSBAR(g_gui.statusbar), "main");
    gtk_box_pack_start(GTK_BOX(vbox),
                       g_gui.statusbar, FALSE, FALSE, 0);

    gtk_statusbar_push(GTK_STATUSBAR(g_gui.statusbar),
                       g_gui.statusbar_ctx,
                       "Ready – use File > Connect or press F5 to connect.");

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
