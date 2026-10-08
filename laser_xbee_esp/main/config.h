#pragma once

#define MONITOR_ID          1        // 1, 2, 3 -> XBee MY and command family (10/11, 20/21, 30/31)

#define RF_CHANNEL          21       // XBee CH = 0x15
#define PAN_ID              0x0B01   // XBee ID
#define DEST_ADDR           0x0000   // XBee DH/DL = 0/0 -> indicator MY
#define TX_POWER_DBM        0        // XB24 PL=4 is 0 dBm; C5/C6 can go to +20

#define LASER_GPIO          4
#define LASER_ACTIVE_HIGH   1
#define LASER_PULL_UP       0
#define DEBOUNCE_MS         30

#define RESEND_INTERVAL_MS  1000     // periodic heartbeat; match the original .ino
#define MAX_TRIES           4        // 1 + 3 retries, like XBee MAC retries
#define ACK_WAIT_MS         20

#define SNIFF_MODE          0        // 1: promiscuous, log every frame on the channel, no TX
