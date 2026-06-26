/*
 * settings_dialog.c – CAN connection settings dialog
 *
 * Allows the user to select:
 *   - CAN interface (auto-discovered from /sys/class/net)
 *   - Nominal bitrate
 *   - CAN FD mode + data bitrate
 *   - Listen-only mode
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/app_state.h"
#include "../inc/socketcan.h"

/* ------------------------------------------------------------------ */
/* Interface discovery                                                  */
/* ------------------------------------------------------------------ */

static void populate_interfaces(GtkComboBoxText *combo)
{
#define MAX_IF 32
    char names[MAX_IF][SOCKETCAN_MAX_IFACE];
    int n = socketcan_list_interfaces(names, MAX_IF);

    /* Always add vcan0 as the first / fallback option */
    gtk_combo_box_text_append_text(combo, "vcan0");

    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], "vcan0") != 0)
            gtk_combo_box_text_append_text(combo, names[i]);
    }

    /* Pre-select the interface currently stored in g_app */
    GtkTreeModel *model =
        gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
    GtkTreeIter iter;
    gboolean valid =
        gtk_tree_model_get_iter_first(model, &iter);
    int idx = 0, sel = 0;
    while (valid) {
        gchar *txt = NULL;
        gtk_tree_model_get(model, &iter, 0, &txt, -1);
        if (txt && strcmp(txt, g_app.iface) == 0) sel = idx;
        g_free(txt);
        valid = gtk_tree_model_iter_next(model, &iter);
        idx++;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), sel);
#undef MAX_IF
}

/* ------------------------------------------------------------------ */
/* Dialog                                                               */
/* ------------------------------------------------------------------ */

static struct {
    const char *label;
    uint32_t    value;
} s_bitrates[] = {
    { "10 kbit/s",   10000   },
    { "20 kbit/s",   20000   },
    { "50 kbit/s",   50000   },
    { "100 kbit/s", 100000   },
    { "125 kbit/s", 125000   },
    { "250 kbit/s", 250000   },
    { "500 kbit/s", 500000   },
    { "800 kbit/s", 800000   },
    { "1 Mbit/s",  1000000   },
};

static struct {
    const char *label;
    uint32_t    value;
} s_fd_bitrates[] = {
    { "1 Mbit/s",   1000000  },
    { "2 Mbit/s",   2000000  },
    { "4 Mbit/s",   4000000  },
    { "5 Mbit/s",   5000000  },
    { "8 Mbit/s",   8000000  },
    { "10 Mbit/s", 10000000  },
    { "12 Mbit/s", 12000000  },
};

/* Callback: enable/disable FD data-rate widgets */
static void on_fd_toggled(GtkToggleButton *btn, gpointer data)
{
    GtkWidget *drate_box = GTK_WIDGET(data);
    gtk_widget_set_sensitive(drate_box,
                             gtk_toggle_button_get_active(btn));
}

void gui_show_settings_dialog(GtkWidget *parent)
{
    if (g_app.connected) {
        /* Ask user to disconnect first */
        GtkWidget *ask = gtk_message_dialog_new(
            parent ? GTK_WINDOW(parent) : NULL,
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION,
            GTK_BUTTONS_YES_NO,
            "Disconnect from %s before changing settings?",
            g_app.iface);
        gint r = gtk_dialog_run(GTK_DIALOG(ask));
        gtk_widget_destroy(ask);
        if (r == GTK_RESPONSE_YES)
            app_do_disconnect();
        else
            return;
    }

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Connection Settings",
        parent ? GTK_WINDOW(parent) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Connect", GTK_RESPONSE_OK,
        NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

    int row = 0;
#define GRID_LABEL(str) do { \
    GtkWidget *_l = gtk_label_new(str); \
    gtk_label_set_xalign(GTK_LABEL(_l), 1.0f); \
    gtk_grid_attach(GTK_GRID(grid), _l, 0, row, 1, 1); \
} while(0)

    /* --- Interface --- */
    GRID_LABEL("Interface:");
    GtkWidget *iface_combo = GTK_WIDGET(gtk_combo_box_text_new_with_entry());
    populate_interfaces(GTK_COMBO_BOX_TEXT(iface_combo));
    gtk_grid_attach(GTK_GRID(grid), iface_combo, 1, row++, 1, 1);

    /* --- Bitrate --- */
    GRID_LABEL("Nominal Bitrate:");
    GtkWidget *brate_combo = GTK_WIDGET(gtk_combo_box_text_new());
    int brate_sel = 6; /* default 500k */
    for (size_t i = 0; i < sizeof(s_bitrates)/sizeof(s_bitrates[0]); i++) {
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(brate_combo), s_bitrates[i].label);
        if (s_bitrates[i].value == g_app.bitrate) brate_sel = (int)i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(brate_combo), brate_sel);
    gtk_grid_attach(GTK_GRID(grid), brate_combo, 1, row++, 1, 1);

    /* --- Listen Only --- */
    GRID_LABEL("Mode:");
    GtkWidget *lo_check = gtk_check_button_new_with_label("Listen Only");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lo_check),
                                  g_app.listen_only);
    gtk_grid_attach(GTK_GRID(grid), lo_check, 1, row++, 1, 1);

    /* --- CAN FD --- */
    GRID_LABEL("CAN FD:");
    GtkWidget *fd_check = gtk_check_button_new_with_label("Enable CAN FD");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fd_check),
                                  g_app.fd_mode);
    gtk_grid_attach(GTK_GRID(grid), fd_check, 1, row++, 1, 1);

    /* --- Data Bitrate (FD) --- */
    GRID_LABEL("Data Bitrate (FD):");
    GtkWidget *drate_combo = GTK_WIDGET(gtk_combo_box_text_new());
    int drate_sel = 1; /* default 2 Mbit/s */
    for (size_t i = 0; i < sizeof(s_fd_bitrates)/sizeof(s_fd_bitrates[0]); i++) {
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(drate_combo), s_fd_bitrates[i].label);
        if (s_fd_bitrates[i].value == g_app.data_bitrate)
            drate_sel = (int)i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(drate_combo), drate_sel);
    gtk_widget_set_sensitive(drate_combo, g_app.fd_mode);
    gtk_grid_attach(GTK_GRID(grid), drate_combo, 1, row++, 1, 1);

    g_signal_connect(fd_check, "toggled",
                     G_CALLBACK(on_fd_toggled), drate_combo);

#undef GRID_LABEL

    /* --- Tip label --- */
    GtkWidget *tip = gtk_label_new(
        "<small><i>Tip: select <b>vcan0</b> for off-hardware testing. "
        "Missing vcan/CAN interfaces are created and brought up automatically "
        "(a graphical authentication prompt may appear — no terminal sudo "
        "needed).</i></small>");
    gtk_label_set_use_markup(GTK_LABEL(tip), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(tip), TRUE);
    gtk_widget_set_margin_top(tip, 8);
    gtk_box_pack_start(GTK_BOX(content), tip, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        /* Read interface */
        const gchar *iface_text =
            gtk_entry_get_text(GTK_ENTRY(
                gtk_bin_get_child(GTK_BIN(iface_combo))));
        strncpy(g_app.iface, iface_text, APP_MAX_IFACE_LEN - 1);

        /* Read bitrate */
        int bi = gtk_combo_box_get_active(GTK_COMBO_BOX(brate_combo));
        if (bi >= 0 && bi < (int)(sizeof(s_bitrates)/sizeof(s_bitrates[0])))
            g_app.bitrate = s_bitrates[bi].value;

        /* Read FD mode */
        g_app.fd_mode = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(fd_check));

        /* Read data bitrate */
        int di = gtk_combo_box_get_active(GTK_COMBO_BOX(drate_combo));
        if (di >= 0 &&
            di < (int)(sizeof(s_fd_bitrates)/sizeof(s_fd_bitrates[0])))
            g_app.data_bitrate = s_fd_bitrates[di].value;

        /* Read listen-only */
        g_app.listen_only = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(lo_check));

        gtk_widget_destroy(dlg);
        app_do_connect();
    } else {
        gtk_widget_destroy(dlg);
    }
}
