#ifndef APP_STATE_H
#define APP_STATE_H

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "can_message.h"
#include "drv_can.h"

/* Forward-declare GLib/GTK types so driver files don't need GTK headers */
typedef struct _GAsyncQueue GAsyncQueue;
typedef struct _GHashTable  GHashTable;

#define APP_MAX_IFACE_LEN   32
#define APP_TRACE_FILE_LEN 256

typedef enum {
    APP_ID_FORMAT_HEX = 0,
    APP_ID_FORMAT_DEC = 1,
} app_id_format_t;

typedef enum {
    APP_DATA_FORMAT_HEX   = 0,
    APP_DATA_FORMAT_DEC   = 1,
    APP_DATA_FORMAT_ASCII = 2,
} app_data_format_t;

typedef struct {
    /* --- Connection --- */
    volatile int  connected;
    char          iface[APP_MAX_IFACE_LEN];
    uint32_t      bitrate;
    uint32_t      data_bitrate;
    int           fd_mode;
    int           listen_only;

    /* --- Statistics --- */
    volatile uint64_t rx_count;
    volatile uint64_t tx_count;
    volatile uint64_t error_count;
    volatile double   bus_load;
    volatile uint8_t  bus_state;
    volatile uint64_t msg_seq;

    /* Intermediate bus-load counters (reset every stats period) */
    volatile uint64_t period_bits;

    /* --- Threads --- */
    pthread_t    rx_thread;
    pthread_t    tx_thread;
    pthread_t    stats_thread;
    volatile int rx_running;
    volatile int tx_running;
    volatile int stats_running;
    volatile int shutting_down;

    /* --- Queues (GLib GAsyncQueue) --- */
    GAsyncQueue *rx_queue;   /* can_msg_t* items  */
    GAsyncQueue *tx_queue;   /* can_msg_t* items  */

    /* --- Trace file --- */
    volatile int    tracing;
    FILE           *trace_file;
    char            trace_filename[APP_TRACE_FILE_LEN];
    pthread_mutex_t trace_mutex;

    /* --- Message deduplication (unique-IDs view) --- */
    int             dedup_mode;
    int             id_format;
    int             data_format;
    pthread_mutex_t dedup_mutex;
    GHashTable     *dedup_table;  /* uint32_t id -> row index */

    /* --- Driver --- */
    can_driver_t   *driver;
} app_state_t;

extern app_state_t g_app;

void app_state_init(void);
void app_state_cleanup(void);

#endif /* APP_STATE_H */
