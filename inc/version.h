/**
 * @file version.h
 * @brief Centralised version and product-identity macros for PCAN-View Linux.
 *
 * @details
 * A single source of truth for the application version string, product name,
 * vendor, and contact details.  These macros are consumed by the command-line
 * `--version` handler (@ref main), the GTK *About* dialog
 * (@ref gui_show_about_dialog), and the Debian packaging metadata.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef PCAN_VIEW_VERSION_H
#define PCAN_VIEW_VERSION_H

/** Human-readable application name. */
#define PCAN_VIEW_APP_NAME    "PCAN-View Linux"

/** Semantic version of the application (MAJOR.MINOR.PATCH). */
#define PCAN_VIEW_VERSION     "1.1.0"

/** Vendor / brand owning this distribution. */
#define PCAN_VIEW_VENDOR      "Taksys"

/** Primary author. */
#define PCAN_VIEW_AUTHOR      "Subhajit Roy"

/** Author contact e-mail. */
#define PCAN_VIEW_EMAIL       "subhajitroy005@gmail.com"

/** Copyright line shown in the About dialog and `--version` output. */
#define PCAN_VIEW_COPYRIGHT   "Copyright \302\251 2026 " PCAN_VIEW_VENDOR

#endif /* PCAN_VIEW_VERSION_H */
