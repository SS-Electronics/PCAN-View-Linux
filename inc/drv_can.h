#ifndef DRV_CAN_H
#define DRV_CAN_H

#include <stdint.h>
#include "can_message.h"

#define DRV_CAN_OK          0
#define DRV_CAN_ERR        -1
#define DRV_CAN_ERR_TIMEOUT -2
#define DRV_CAN_ERR_PARAM  -3
#define DRV_CAN_ERR_INIT   -4

typedef enum {
    CAN_BAUD_10K   =   10000,
    CAN_BAUD_20K   =   20000,
    CAN_BAUD_50K   =   50000,
    CAN_BAUD_100K  =  100000,
    CAN_BAUD_125K  =  125000,
    CAN_BAUD_250K  =  250000,
    CAN_BAUD_500K  =  500000,
    CAN_BAUD_800K  =  800000,
    CAN_BAUD_1M    = 1000000,
} can_baud_t;

typedef enum {
    CAN_FD_BAUD_1M  =  1000000,
    CAN_FD_BAUD_2M  =  2000000,
    CAN_FD_BAUD_4M  =  4000000,
    CAN_FD_BAUD_5M  =  5000000,
    CAN_FD_BAUD_8M  =  8000000,
    CAN_FD_BAUD_10M = 10000000,
} can_fd_baud_t;

typedef struct can_driver {
    int         (*init)(const char *iface, uint32_t bitrate,
                        uint32_t data_bitrate, int fd_mode, int listen_only);
    int         (*deinit)(void);
    int         (*send)(const can_msg_t *msg);
    int         (*recv)(can_msg_t *msg, int timeout_ms);
    int         (*get_stats)(can_stats_t *stats);
    int         (*set_filter)(uint32_t id, uint32_t mask, int is_extended);
    int         (*clear_filter)(void);
    int         (*reset)(void);
    const char *(*error_string)(int err);
} can_driver_t;

struct can_driver *drv_can_get_socketcan(void);

int drv_can_init(can_driver_t *drv, const char *iface, uint32_t bitrate,
                 uint32_t data_bitrate, int fd_mode, int listen_only);
int drv_can_deinit(void);
int drv_can_send(const can_msg_t *msg);
int drv_can_recv(can_msg_t *msg, int timeout_ms);
int drv_can_get_stats(can_stats_t *stats);
int drv_can_set_filter(uint32_t id, uint32_t mask, int is_extended);
int drv_can_clear_filter(void);
int drv_can_reset(void);
const char *drv_can_error_string(int err);

#endif /* DRV_CAN_H */
