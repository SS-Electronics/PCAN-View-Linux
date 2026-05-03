#ifndef CAN_MESSAGE_H
#define CAN_MESSAGE_H

#include <stdint.h>
#include <time.h>

#define CAN_MAX_DLC      8
#define CANFD_DATA_MAX  64   /* max data bytes in a CAN FD frame */

#define CAN_DIR_RX      0
#define CAN_DIR_TX      1

#define CAN_BUS_ACTIVE  0
#define CAN_BUS_WARNING 1
#define CAN_BUS_PASSIVE 2
#define CAN_BUS_OFF     3

typedef struct {
    uint32_t        id;
    uint8_t         dlc;
    uint8_t         data[CANFD_DATA_MAX];
    struct timespec timestamp;
    uint8_t         is_extended;
    uint8_t         is_remote;
    uint8_t         is_error;
    uint8_t         is_fd;
    uint8_t         is_brs;
    uint8_t         is_esi;
    uint8_t         direction;
    uint64_t        seq;
} can_msg_t;

typedef struct {
    uint64_t rx_count;
    uint64_t tx_count;
    uint64_t error_count;
    double   bus_load;
    uint8_t  bus_state;
} can_stats_t;

#endif /* CAN_MESSAGE_H */
