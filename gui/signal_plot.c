/**
 * @file signal_plot.c
 * @brief Signal Analysis Viewer — multi-signal value-over-time graph (Cairo).
 *
 * @details
 * Implements a Vector-CANalyzer "Graphics"-style oscilloscope tab: any subset of
 * the signals defined by the loaded DBC can be selected and plotted together on
 * one time axis.  Each decoded sample (delivered from the Signal Analysis decode
 * path via @ref gui_plot_add_sample) is appended to a per-signal ring buffer; a
 * Cairo @c GtkDrawingArea renders a scrolling window of the most recent samples.
 *
 * Because signals have wildly different physical ranges (rpm vs. °C vs. %), each
 * trace is normalised to its DBC `[min,max]` range so all selected signals share
 * the vertical space; the legend reports each signal's real latest value and
 * unit.  A redraw timer repaints at ~30 fps only while the page is mapped, and
 * the visible-window width is adjustable.
 *
 * All sample appends and draws happen on the GTK main thread (the decode path
 * runs in a GLib idle callback), so no locking is required.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/dbc.h"

#define PLOT_RING       30000   /**< Samples kept per series (~30 s @ 1 kHz).  */
#define PLOT_REDRAW_MS  33      /**< Redraw period (~30 fps) while mapped.     */

/** @brief Distinct trace colours (RGB 0..1), cycled across series. */
static const double PALETTE[][3] = {
    { 0.20, 0.60, 0.86 }, { 0.91, 0.30, 0.24 }, { 0.18, 0.80, 0.44 },
    { 0.95, 0.61, 0.07 }, { 0.61, 0.35, 0.71 }, { 0.10, 0.74, 0.61 },
    { 0.90, 0.49, 0.13 }, { 0.52, 0.58, 0.59 }, { 0.83, 0.33, 0.64 },
    { 0.16, 0.50, 0.73 }, { 0.86, 0.78, 0.13 }, { 0.40, 0.76, 0.65 },
};
#define PALETTE_N ((int)(sizeof(PALETTE) / sizeof(PALETTE[0])))

/**
 * @brief One plottable signal with its rolling sample history.
 */
typedef struct {
    char       label[2 * DBC_NAME_MAX]; /**< "Message.Signal".                 */
    char       unit[DBC_UNIT_MAX];      /**< Engineering unit.                 */
    uint32_t   id;                      /**< Raw CAN identifier.               */
    uint8_t    ext;                     /**< Extended-ID flag.                 */
    int        sig_idx;                 /**< Signal index within its message.  */
    double     dmin, dmax;              /**< DBC range (for normalisation).    */
    gboolean   enabled;                 /**< Plotted when TRUE.                */
    double     col[3];                  /**< Trace colour.                     */

    double     t[PLOT_RING];            /**< Sample times (s since start).     */
    double     v[PLOT_RING];            /**< Sample values.                    */
    int        head;                    /**< Next write index.                 */
    int        count;                   /**< Valid sample count.               */
    double     last;                    /**< Most recent value.                */
    gboolean   has_last;                /**< Whether @ref last is valid.       */

    GtkWidget *check;                   /**< Selector check button.            */
} plot_series_t;

static struct {
    plot_series_t *series;       /**< Dynamically sized series array.          */
    int            n_series;     /**< Number of series.                        */
    GtkWidget     *area;         /**< The Cairo drawing area.                  */
    GtkWidget     *list_box;     /**< Selector check-button container.         */
    double         window_sec;   /**< Visible time window width (seconds).     */
    gint64         start_us;     /**< Monotonic origin for sample times.       */
    gboolean       paused;       /**< Freeze sample intake when TRUE.          */
    gboolean       dirty;        /**< New data since the last repaint.         */
    guint          timer_id;     /**< Redraw timer source id.                  */
} s_plot;

/** @brief Seconds elapsed since the plot's monotonic origin. */
static double plot_now(void)
{
    return (double)(g_get_monotonic_time() - s_plot.start_us) / 1e6;
}

/* ------------------------------------------------------------------ */
/* Series management                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Free the current series array and clear the selector list.
 */
static void plot_free_series(void)
{
    if (s_plot.list_box) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(s_plot.list_box));
        for (GList *l = kids; l; l = l->next)
            gtk_widget_destroy(GTK_WIDGET(l->data));
        g_list_free(kids);
    }
    g_free(s_plot.series);
    s_plot.series   = NULL;
    s_plot.n_series = 0;
}

/**
 * @brief Selector check-button toggled — enable/disable a series.
 */
static void on_series_toggled(GtkToggleButton *btn, gpointer data)
{
    int idx = GPOINTER_TO_INT(data);
    if (idx >= 0 && idx < s_plot.n_series)
        s_plot.series[idx].enabled = gtk_toggle_button_get_active(btn);
    s_plot.dirty = TRUE;
    if (s_plot.area)
        gtk_widget_queue_draw(s_plot.area);
}

void gui_plot_set_database(const dbc_db_t *db)
{
    plot_free_series();

    if (!db || db->signal_count == 0) {
        if (s_plot.area)
            gtk_widget_queue_draw(s_plot.area);
        return;
    }

    s_plot.series = g_malloc0(sizeof(plot_series_t) * db->signal_count);
    int n = 0;

    for (size_t mi = 0; mi < db->message_count; mi++) {
        const dbc_message_t *m = &db->messages[mi];
        for (uint16_t si = 0; si < m->signal_count; si++) {
            const dbc_signal_t *s = &m->signals[si];
            plot_series_t *ps = &s_plot.series[n];

            snprintf(ps->label, sizeof(ps->label), "%s.%s", m->name, s->name);
            snprintf(ps->unit, sizeof(ps->unit), "%s", s->unit);
            ps->id      = m->id;
            ps->ext     = m->is_extended;
            ps->sig_idx = si;
            ps->dmin    = s->min;
            ps->dmax    = s->max;
            ps->enabled = FALSE;
            ps->col[0]  = PALETTE[n % PALETTE_N][0];
            ps->col[1]  = PALETTE[n % PALETTE_N][1];
            ps->col[2]  = PALETTE[n % PALETTE_N][2];

            char swatch[2 * DBC_NAME_MAX + 16];
            snprintf(swatch, sizeof(swatch), "%s", ps->label);
            ps->check = gtk_check_button_new_with_label(swatch);

            /* Colour the label text to match the trace. */
            char css[96];
            snprintf(css, sizeof(css),
                     "* { color: #%02x%02x%02x; }",
                     (int)(ps->col[0] * 255), (int)(ps->col[1] * 255),
                     (int)(ps->col[2] * 255));
            GtkCssProvider *prov = gtk_css_provider_new();
            gtk_css_provider_load_from_data(prov, css, -1, NULL);
            gtk_style_context_add_provider(
                gtk_widget_get_style_context(ps->check),
                GTK_STYLE_PROVIDER(prov),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(prov);

            g_signal_connect(ps->check, "toggled",
                             G_CALLBACK(on_series_toggled),
                             GINT_TO_POINTER(n));
            gtk_box_pack_start(GTK_BOX(s_plot.list_box), ps->check,
                               FALSE, FALSE, 0);
            n++;
        }
    }
    s_plot.n_series = n;
    gtk_widget_show_all(s_plot.list_box);
    s_plot.dirty = TRUE;
    if (s_plot.area)
        gtk_widget_queue_draw(s_plot.area);
}

void gui_plot_add_sample(uint32_t id, int is_ext, int sig_idx, double value)
{
    if (s_plot.paused || !s_plot.series)
        return;

    for (int i = 0; i < s_plot.n_series; i++) {
        plot_series_t *ps = &s_plot.series[i];
        if (ps->id != id || ps->ext != (is_ext ? 1 : 0) || ps->sig_idx != sig_idx)
            continue;

        ps->t[ps->head] = plot_now();
        ps->v[ps->head] = value;
        ps->head = (ps->head + 1) % PLOT_RING;
        if (ps->count < PLOT_RING)
            ps->count++;
        ps->last     = value;
        ps->has_last = TRUE;
        s_plot.dirty = TRUE;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Map a normalised value (0..1) to a Y pixel within the plot rect.
 */
static double norm_to_y(double norm, double y0, double y1)
{
    if (norm < 0.0) norm = 0.0;
    if (norm > 1.0) norm = 1.0;
    return y1 - norm * (y1 - y0);
}

/**
 * @brief Cairo draw handler for the graph.
 */
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)data;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    const double W = a.width, H = a.height;

    /* Background. */
    cairo_set_source_rgb(cr, 0.12, 0.12, 0.14);
    cairo_rectangle(cr, 0, 0, W, H);
    cairo_fill(cr);

    const double left = 50, right = 12, top = 10, bottom = 24;
    const double x0 = left, x1 = W - right, y0 = top, y1 = H - bottom;
    if (x1 <= x0 + 10 || y1 <= y0 + 10)
        return FALSE;

    const double now  = plot_now();
    const double winw = s_plot.window_sec;
    const double tmin = now - winw;

    /* Grid + Y axis labels (normalised 0..100 %). */
    cairo_set_line_width(cr, 1.0);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    for (int i = 0; i <= 10; i++) {
        double yy = y0 + (y1 - y0) * i / 10.0;
        cairo_set_source_rgb(cr, 0.22, 0.22, 0.25);
        cairo_move_to(cr, x0, yy);
        cairo_line_to(cr, x1, yy);
        cairo_stroke(cr);
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%d%%", 100 - i * 10);
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.65);
        cairo_move_to(cr, 6, yy + 3);
        cairo_show_text(cr, lbl);
    }

    /* X axis time gridlines/labels (relative seconds). */
    int xdiv = 10;
    for (int i = 0; i <= xdiv; i++) {
        double xx = x0 + (x1 - x0) * i / (double)xdiv;
        cairo_set_source_rgb(cr, 0.22, 0.22, 0.25);
        cairo_move_to(cr, xx, y0);
        cairo_line_to(cr, xx, y1);
        cairo_stroke(cr);
        char lbl[16];
        double rel = -winw + winw * i / (double)xdiv;
        snprintf(lbl, sizeof(lbl), "%.0fs", rel);
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.65);
        cairo_move_to(cr, xx - 8, y1 + 16);
        cairo_show_text(cr, lbl);
    }

    /* Plot frame. */
    cairo_set_source_rgb(cr, 0.4, 0.4, 0.45);
    cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
    cairo_stroke(cr);

    if (!s_plot.series)
        return FALSE;

    int enabled = 0;
    for (int i = 0; i < s_plot.n_series; i++)
        if (s_plot.series[i].enabled)
            enabled++;

    if (enabled == 0) {
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.75);
        cairo_set_font_size(cr, 13);
        cairo_move_to(cr, x0 + 14, y0 + 24);
        cairo_show_text(cr,
            "Select one or more signals on the left to plot them over time.");
        return FALSE;
    }

    /* Traces. */
    for (int i = 0; i < s_plot.n_series; i++) {
        plot_series_t *ps = &s_plot.series[i];
        if (!ps->enabled || ps->count == 0)
            continue;

        double span = ps->dmax - ps->dmin;
        double base = ps->dmin;
        if (!(span > 0.0)) {
            /* Invalid/empty DBC range — auto-scale from visible samples. */
            double mn = 1e300, mx = -1e300;
            for (int k = 0; k < ps->count; k++) {
                int idx = (ps->head - ps->count + k + PLOT_RING) % PLOT_RING;
                if (ps->t[idx] < tmin) continue;
                if (ps->v[idx] < mn) mn = ps->v[idx];
                if (ps->v[idx] > mx) mx = ps->v[idx];
            }
            if (mx > mn) { base = mn; span = mx - mn; }
            else         { base = mn - 0.5; span = 1.0; }
        }

        /* Stride-decimate so we draw at most ~2 segments per horizontal pixel. */
        int visible = 0;
        int first = -1;
        for (int k = 0; k < ps->count; k++) {
            int idx = (ps->head - ps->count + k + PLOT_RING) % PLOT_RING;
            if (ps->t[idx] >= tmin) { if (first < 0) first = k; visible++; }
        }
        if (visible == 0)
            continue;
        int max_pts = (int)((x1 - x0) * 2.0);
        int stride  = visible > max_pts ? visible / max_pts : 1;

        cairo_set_source_rgb(cr, ps->col[0], ps->col[1], ps->col[2]);
        cairo_set_line_width(cr, 1.5);
        gboolean started = FALSE;
        for (int k = first; k < ps->count; k += stride) {
            int idx = (ps->head - ps->count + k + PLOT_RING) % PLOT_RING;
            double tt = ps->t[idx];
            if (tt < tmin) continue;
            double x = x0 + (tt - tmin) / winw * (x1 - x0);
            double norm = (ps->v[idx] - base) / span;
            double y = norm_to_y(norm, y0, y1);
            if (!started) { cairo_move_to(cr, x, y); started = TRUE; }
            else            cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
    }

    /* Legend (top-left, over the plot). */
    cairo_set_font_size(cr, 11);
    double ly = y0 + 14;
    for (int i = 0; i < s_plot.n_series; i++) {
        plot_series_t *ps = &s_plot.series[i];
        if (!ps->enabled)
            continue;
        cairo_set_source_rgb(cr, ps->col[0], ps->col[1], ps->col[2]);
        cairo_rectangle(cr, x0 + 8, ly - 8, 12, 10);
        cairo_fill(cr);
        char txt[2 * DBC_NAME_MAX + 64];
        if (ps->has_last)
            snprintf(txt, sizeof(txt), "%s = %.3g %s",
                     ps->label, ps->last, ps->unit);
        else
            snprintf(txt, sizeof(txt), "%s", ps->label);
        cairo_move_to(cr, x0 + 26, ly + 1);
        cairo_show_text(cr, txt);
        ly += 15;
        if (ly > y1 - 4)
            break;
    }
    return FALSE;
}

/**
 * @brief Redraw timer — repaint while the page is visible and data changed.
 */
static gboolean plot_tick(gpointer data)
{
    (void)data;
    if (s_plot.area && gtk_widget_get_mapped(s_plot.area)) {
        /* Repaint continuously so the time axis scrolls smoothly; the data is
         * cheap to redraw and we only run while the page is on screen. */
        gtk_widget_queue_draw(s_plot.area);
        s_plot.dirty = FALSE;
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Toolbar callbacks                                                    */
/* ------------------------------------------------------------------ */

/** @brief "Select All" — enable every series. */
static void on_select_all(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    for (int i = 0; i < s_plot.n_series; i++)
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(s_plot.series[i].check), TRUE);
}

/** @brief "Clear Selection" — disable every series. */
static void on_select_none(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    for (int i = 0; i < s_plot.n_series; i++)
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(s_plot.series[i].check), FALSE);
}

/** @brief "Reset" — drop all buffered samples and restart the time origin. */
static void on_reset(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    for (int i = 0; i < s_plot.n_series; i++) {
        s_plot.series[i].head = s_plot.series[i].count = 0;
        s_plot.series[i].has_last = FALSE;
    }
    s_plot.start_us = g_get_monotonic_time();
    if (s_plot.area)
        gtk_widget_queue_draw(s_plot.area);
}

/** @brief Pause toggle — freeze/resume sample intake. */
static void on_pause(GtkToggleButton *btn, gpointer d)
{
    (void)d;
    s_plot.paused = gtk_toggle_button_get_active(btn);
}

/** @brief Window-width spin — change the visible time span. */
static void on_window_changed(GtkSpinButton *btn, gpointer d)
{
    (void)d;
    s_plot.window_sec = gtk_spin_button_get_value(btn);
}

/** @brief Free the redraw timer when the page is destroyed. */
static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (s_plot.timer_id) {
        g_source_remove(s_plot.timer_id);
        s_plot.timer_id = 0;
    }
}

/* ------------------------------------------------------------------ */
/* View construction                                                   */
/* ------------------------------------------------------------------ */

GtkWidget *gui_create_signal_plot(void)
{
    s_plot.window_sec = 20.0;
    s_plot.start_us   = g_get_monotonic_time();

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* --- Toolbar --- */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 4);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

    GtkWidget *all_btn = gtk_button_new_with_label("Select All");
    g_signal_connect(all_btn, "clicked", G_CALLBACK(on_select_all), NULL);
    gtk_box_pack_start(GTK_BOX(bar), all_btn, FALSE, FALSE, 0);

    GtkWidget *none_btn = gtk_button_new_with_label("Clear Selection");
    g_signal_connect(none_btn, "clicked", G_CALLBACK(on_select_none), NULL);
    gtk_box_pack_start(GTK_BOX(bar), none_btn, FALSE, FALSE, 0);

    GtkWidget *reset_btn = gtk_button_new_with_label("Reset");
    g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_reset), NULL);
    gtk_box_pack_start(GTK_BOX(bar), reset_btn, FALSE, FALSE, 0);

    GtkWidget *pause_btn = gtk_toggle_button_new_with_label("Pause");
    g_signal_connect(pause_btn, "toggled", G_CALLBACK(on_pause), NULL);
    gtk_box_pack_start(GTK_BOX(bar), pause_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("Window (s):"),
                       FALSE, FALSE, 4);
    GtkWidget *win_spin = gtk_spin_button_new_with_range(2, 120, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(win_spin), s_plot.window_sec);
    g_signal_connect(win_spin, "value-changed",
                     G_CALLBACK(on_window_changed), NULL);
    gtk_box_pack_start(GTK_BOX(bar), win_spin, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    /* --- Paned: signal selector | graph --- */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(box), paned, TRUE, TRUE, 0);

    GtkWidget *sel_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sel_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sel_scroll, 220, -1);
    s_plot.list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(s_plot.list_box, 6);
    gtk_widget_set_margin_top(s_plot.list_box, 4);
    gtk_container_add(GTK_CONTAINER(sel_scroll), s_plot.list_box);
    gtk_paned_pack1(GTK_PANED(paned), sel_scroll, FALSE, TRUE);

    s_plot.area = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_plot.area, 400, 200);
    g_signal_connect(s_plot.area, "draw", G_CALLBACK(on_draw), NULL);
    gtk_paned_pack2(GTK_PANED(paned), s_plot.area, TRUE, TRUE);

    g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), NULL);
    s_plot.timer_id = g_timeout_add(PLOT_REDRAW_MS, plot_tick, NULL);

    return box;
}
