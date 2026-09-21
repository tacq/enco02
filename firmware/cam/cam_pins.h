#pragma once

// AI-Thinker ESP32-CAM pin map, plus the two pins left over for the link to the
// main board.
//
// With the OV2640 attached there is almost nothing free on this board. What is
// exposed on the header, and why nearly all of it is unusable:
//
//   GPIO 0       camera XCLK, and the flash-mode strap. Untouchable.
//   GPIO 1, 3    UART0. This is how the board is flashed and how its log is
//                read. Taking it would make the cam undebuggable.
//   GPIO 2,12,15 free only when no microSD card is fitted, but all three are
//                boot strapping pins. A UART line idles HIGH, and a HIGH on
//                GPIO 12 (MTDI) at reset selects a 1.8V flash voltage and the
//                board simply does not boot.
//   GPIO 16      wired to the PSRAM chip select on this revision. Widely
//                reported to boot-loop or corrupt frames when driven.
//   GPIO 4       the flash LED. Usable, but it is blindingly bright.
//   GPIO 32      camera power-down. Reported to stop the sensor if repurposed.
//   GPIO 13, 14  free when no microSD card is fitted, and NEITHER is a
//                strapping pin.
//
// So 13/14 is not a preference, it is the only clean pair - at the cost of
// giving up the SD slot, which this firmware never uses because nothing is
// stored locally.

#ifndef _CAM_PINS_H_
#define _CAM_PINS_H_

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// Link to the main board. Cross-wired: our TX goes to its RX.
//   CAM GPIO 13 (TX) --> MAIN GPIO 18 (RX)
//   CAM GPIO 14 (RX) <-- MAIN GPIO 19 (TX)
//   GND <-> GND       (mandatory - without a common ground there is no link)
#define LINK_TX_GPIO 13
#define LINK_RX_GPIO 14
#define LINK_BAUD 115200

#endif  // _CAM_PINS_H_
