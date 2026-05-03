#ifndef SOCKETCAN_H
#define SOCKETCAN_H

#include <stdint.h>
#include "can_message.h"
#include "drv_can.h"

#define SOCKETCAN_MAX_IFACE 32

typedef struct {
    int      sock_fd;
    int      fd_enabled;
    int      listen_only;
    char     iface[SOCKETCAN_MAX_IFACE];
    uint32_t bitrate;
    uint32_t data_bitrate;
} socketcan_ctx_t;

int  socketcan_open(socketcan_ctx_t *ctx, const char *iface,
                    uint32_t bitrate, uint32_t data_bitrate,
                    int fd_mode, int listen_only);
int  socketcan_close(socketcan_ctx_t *ctx);
int  socketcan_send(socketcan_ctx_t *ctx, const can_msg_t *msg);
int  socketcan_recv(socketcan_ctx_t *ctx, can_msg_t *msg, int timeout_ms);
int  socketcan_get_stats(socketcan_ctx_t *ctx, can_stats_t *stats);
int  socketcan_set_filter(socketcan_ctx_t *ctx, uint32_t id,
                          uint32_t mask, int is_extended);
int  socketcan_clear_filter(socketcan_ctx_t *ctx);
int  socketcan_reset(socketcan_ctx_t *ctx);
const char *socketcan_error_string(int err);

int socketcan_list_interfaces(char ifnames[][SOCKETCAN_MAX_IFACE],
                              int max_count);

#endif /* SOCKETCAN_H */
