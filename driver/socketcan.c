/*
 * socketcan.c – SocketCAN back-end for PCAN-View Linux
 *
 * Uses the standard Linux SocketCAN subsystem (PF_CAN / SOCK_RAW).
 * Bitrate configuration is performed via the "ip" utility so that
 * the application does not need a Netlink implementation.
 *
 * Reference:
 *   https://www.kernel.org/doc/Documentation/networking/can.rst
 *   https://www.peak-system.com/fileadmin/media/linux/index.php
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/error.h>

#include "../inc/socketcan.h"
#include "../inc/drv_can.h"
#include "../inc/can_message.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static int run_cmd(const char *fmt, ...)
{
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    int ret = system(cmd);
    return (ret == 0) ? 0 : -1;
}

/* Bring interface down, configure bitrate, bring up */
static int configure_interface(const char *iface, uint32_t bitrate,
                                uint32_t data_bitrate, int fd_mode,
                                int listen_only)
{
    /* Bring down first (ignore error – may already be down) */
    run_cmd("ip link set %s down 2>/dev/null", iface);

    if (fd_mode && data_bitrate > 0) {
        if (run_cmd("ip link set %s type can bitrate %u dbitrate %u fd on",
                    iface, bitrate, data_bitrate) != 0) {
            fprintf(stderr, "socketcan: failed to set CAN FD bitrate on %s\n",
                    iface);
            return -1;
        }
    } else {
        if (run_cmd("ip link set %s type can bitrate %u",
                    iface, bitrate) != 0) {
            /* Virtual CAN doesn't need bitrate; try bringing up anyway */
            fprintf(stderr, "socketcan: warning – bitrate config failed on "
                    "%s (ok for vcan)\n", iface);
        }
    }

    if (listen_only) {
        run_cmd("ip link set %s type can listen-only on", iface);
    }

    if (run_cmd("ip link set %s up", iface) != 0) {
        fprintf(stderr, "socketcan: failed to bring up %s\n", iface);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int socketcan_open(socketcan_ctx_t *ctx, const char *iface,
                   uint32_t bitrate, uint32_t data_bitrate,
                   int fd_mode, int listen_only)
{
    if (!ctx || !iface || iface[0] == '\0')
        return DRV_CAN_ERR_PARAM;

    memset(ctx, 0, sizeof(*ctx));
    ctx->sock_fd     = -1;
    ctx->fd_enabled  = fd_mode;
    ctx->listen_only = listen_only;
    ctx->bitrate     = bitrate;
    ctx->data_bitrate = data_bitrate;
    strncpy(ctx->iface, iface, SOCKETCAN_MAX_IFACE - 1);

    /* Configure interface bitrate (skip for vcan) */
    if (strncmp(iface, "vcan", 4) != 0) {
        if (configure_interface(iface, bitrate, data_bitrate,
                                fd_mode, listen_only) != 0)
            return DRV_CAN_ERR_INIT;
    } else {
        /* vcan: just bring it up */
        run_cmd("ip link set %s up 2>/dev/null", iface);
    }

    /* Create raw CAN socket */
    ctx->sock_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (ctx->sock_fd < 0) {
        perror("socketcan: socket");
        return DRV_CAN_ERR_INIT;
    }

    /* Enable error frames */
    can_err_mask_t err_mask = CAN_ERR_MASK;
    setsockopt(ctx->sock_fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
               &err_mask, sizeof(err_mask));

    /* Enable CAN FD if requested */
    if (fd_mode) {
        int enable = 1;
        if (setsockopt(ctx->sock_fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES,
                       &enable, sizeof(enable)) < 0) {
            fprintf(stderr, "socketcan: CAN FD not supported on this "
                    "interface, falling back to classic CAN\n");
            ctx->fd_enabled = 0;
        }
    }

    /* Bind to the interface */
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(ctx->sock_fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("socketcan: ioctl SIOCGIFINDEX");
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
        return DRV_CAN_ERR_INIT;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(ctx->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("socketcan: bind");
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
        return DRV_CAN_ERR_INIT;
    }

    return DRV_CAN_OK;
}

int socketcan_close(socketcan_ctx_t *ctx)
{
    if (!ctx) return DRV_CAN_ERR_PARAM;
    if (ctx->sock_fd >= 0) {
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
    }
    if (strncmp(ctx->iface, "vcan", 4) != 0 && ctx->iface[0] != '\0') {
        run_cmd("ip link set %s down 2>/dev/null", ctx->iface);
    }
    return DRV_CAN_OK;
}

int socketcan_send(socketcan_ctx_t *ctx, const can_msg_t *msg)
{
    if (!ctx || ctx->sock_fd < 0 || !msg)
        return DRV_CAN_ERR_PARAM;

    if (msg->is_fd && ctx->fd_enabled) {
        struct canfd_frame frame;
        memset(&frame, 0, sizeof(frame));
        frame.can_id = msg->id;
        if (msg->is_extended) frame.can_id |= CAN_EFF_FLAG;
        if (msg->is_remote)   frame.can_id |= CAN_RTR_FLAG;
        frame.len = msg->dlc > CANFD_DATA_MAX ? CANFD_DATA_MAX : msg->dlc;
        if (msg->is_brs) frame.flags |= CANFD_BRS;
        if (msg->is_esi) frame.flags |= CANFD_ESI;
        memcpy(frame.data, msg->data, frame.len);

        ssize_t n = write(ctx->sock_fd, &frame, sizeof(frame));
        if (n != sizeof(frame)) return DRV_CAN_ERR;
    } else {
        struct can_frame frame;
        memset(&frame, 0, sizeof(frame));
        frame.can_id  = msg->id;
        if (msg->is_extended) frame.can_id |= CAN_EFF_FLAG;
        if (msg->is_remote)   frame.can_id |= CAN_RTR_FLAG;
        frame.can_dlc = msg->dlc > CAN_MAX_DLC ? CAN_MAX_DLC : msg->dlc;
        memcpy(frame.data, msg->data, frame.can_dlc);

        ssize_t n = write(ctx->sock_fd, &frame, sizeof(frame));
        if (n != sizeof(frame)) return DRV_CAN_ERR;
    }
    return DRV_CAN_OK;
}

int socketcan_recv(socketcan_ctx_t *ctx, can_msg_t *msg, int timeout_ms)
{
    if (!ctx || ctx->sock_fd < 0 || !msg)
        return DRV_CAN_ERR_PARAM;

    /* Wait with timeout */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(ctx->sock_fd, &rfds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(ctx->sock_fd + 1, &rfds, NULL, NULL,
                     timeout_ms >= 0 ? &tv : NULL);
    if (ret < 0)  return DRV_CAN_ERR;
    if (ret == 0) return DRV_CAN_ERR_TIMEOUT;

    memset(msg, 0, sizeof(*msg));
    clock_gettime(CLOCK_REALTIME, &msg->timestamp);

    /* Try CAN FD frame first */
    struct canfd_frame fdf;
    ssize_t n = read(ctx->sock_fd, &fdf, sizeof(fdf));
    if (n < 0) return DRV_CAN_ERR;

    if (n == (ssize_t)sizeof(struct canfd_frame) && ctx->fd_enabled) {
        /* CAN FD frame */
        msg->is_fd       = 1;
        msg->is_brs      = (fdf.flags & CANFD_BRS) ? 1 : 0;
        msg->is_esi      = (fdf.flags & CANFD_ESI) ? 1 : 0;
        msg->is_extended = (fdf.can_id & CAN_EFF_FLAG) ? 1 : 0;
        msg->is_remote   = (fdf.can_id & CAN_RTR_FLAG) ? 1 : 0;
        msg->is_error    = (fdf.can_id & CAN_ERR_FLAG) ? 1 : 0;
        msg->id          = fdf.can_id & (msg->is_extended ? CAN_EFF_MASK
                                                           : CAN_SFF_MASK);
        msg->dlc         = fdf.len;
        memcpy(msg->data, fdf.data, fdf.len);
    } else {
        /* Classic CAN frame */
        struct can_frame *cf = (struct can_frame *)&fdf;
        msg->is_fd       = 0;
        msg->is_extended = (cf->can_id & CAN_EFF_FLAG) ? 1 : 0;
        msg->is_remote   = (cf->can_id & CAN_RTR_FLAG) ? 1 : 0;
        msg->is_error    = (cf->can_id & CAN_ERR_FLAG) ? 1 : 0;
        msg->id          = cf->can_id & (msg->is_extended ? CAN_EFF_MASK
                                                           : CAN_SFF_MASK);
        msg->dlc         = cf->can_dlc;
        memcpy(msg->data, cf->data, cf->can_dlc);
    }
    msg->direction = CAN_DIR_RX;
    return DRV_CAN_OK;
}

int socketcan_get_stats(socketcan_ctx_t *ctx, can_stats_t *stats)
{
    (void)ctx;
    /* Stats are maintained by the application layer; just return OK */
    (void)stats;
    return DRV_CAN_OK;
}

int socketcan_set_filter(socketcan_ctx_t *ctx, uint32_t id,
                         uint32_t mask, int is_extended)
{
    if (!ctx || ctx->sock_fd < 0)
        return DRV_CAN_ERR_PARAM;

    struct can_filter filter;
    filter.can_id   = id | (is_extended ? CAN_EFF_FLAG : 0);
    filter.can_mask = mask | CAN_EFF_FLAG;

    if (setsockopt(ctx->sock_fd, SOL_CAN_RAW, CAN_RAW_FILTER,
                   &filter, sizeof(filter)) < 0) {
        perror("socketcan: setsockopt CAN_RAW_FILTER");
        return DRV_CAN_ERR;
    }
    return DRV_CAN_OK;
}

int socketcan_clear_filter(socketcan_ctx_t *ctx)
{
    if (!ctx || ctx->sock_fd < 0)
        return DRV_CAN_ERR_PARAM;

    /* NULL filter = accept all */
    if (setsockopt(ctx->sock_fd, SOL_CAN_RAW, CAN_RAW_FILTER,
                   NULL, 0) < 0) {
        perror("socketcan: setsockopt clear filter");
        return DRV_CAN_ERR;
    }
    return DRV_CAN_OK;
}

int socketcan_reset(socketcan_ctx_t *ctx)
{
    if (!ctx || ctx->iface[0] == '\0')
        return DRV_CAN_ERR_PARAM;

    run_cmd("ip link set %s down 2>/dev/null", ctx->iface);
    run_cmd("ip link set %s up 2>/dev/null",   ctx->iface);
    return DRV_CAN_OK;
}

const char *socketcan_error_string(int err)
{
    switch (err) {
    case DRV_CAN_OK:          return "OK";
    case DRV_CAN_ERR:         return "General error";
    case DRV_CAN_ERR_TIMEOUT: return "Timeout";
    case DRV_CAN_ERR_PARAM:   return "Invalid parameter";
    case DRV_CAN_ERR_INIT:    return "Initialization failed";
    default:                  return "Unknown error";
    }
}

int socketcan_list_interfaces(char ifnames[][SOCKETCAN_MAX_IFACE],
                               int max_count)
{
    int count = 0;
    DIR *d = opendir("/sys/class/net");
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_count) {
        if (ent->d_name[0] == '.') continue;

        char path[300];
        snprintf(path, sizeof(path),
                 "/sys/class/net/%s/type", ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        int type = 0;
        int n = fscanf(f, "%d", &type);
        fclose(f);

        if (n == 1 && type == 280 /* ARPHRD_CAN */) {
            size_t nl = strlen(ent->d_name);
            if (nl >= SOCKETCAN_MAX_IFACE) continue; /* name too long, skip */
            memcpy(ifnames[count], ent->d_name, nl + 1);
            count++;
        }
    }
    closedir(d);
    return count;
}
