#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// RENDER
#define RENDER_WIDTH 120
#define RENDER_HEIGHT 80
#define TOTAL_PIXELS (RENDER_WIDTH * RENDER_HEIGHT)
#define DRAW_SCALE_X 2
#define DRAW_SCALE_Y 2
#define DRAW_WIDTH (RENDER_WIDTH * DRAW_SCALE_X)
#define DRAW_HEIGHT (RENDER_HEIGHT * DRAW_SCALE_Y)
#define TOTAL_SCREEN_PIXELS (DRAW_WIDTH * DRAW_HEIGHT)
#define PALETTE_COLORS 256

// TRANSFER
#define PACKET_SIZE 4
#define COLOR_SIZE 2
#define PIXEL_SIZE 1
#define SPI_MODE 3
#define SPI_SLOW_FREQUENCY 1600000
#define SPI_FAST_FREQUENCY 2600000
#define SPI_DELAY_MICROSECONDS 5
#define MAX_BLIND_FRAMES 3
#define TRANSFER_SYNC_FREQUENCY 8
#define PRESSED_KEYS_MIN_VALIDATIONS 3
#define PRESSED_KEYS_REPETITIONS 10
#define COLORS_PER_PACKET (PACKET_SIZE / COLOR_SIZE)
#define PIXELS_PER_PACKET (PACKET_SIZE / PIXEL_SIZE)

// INPUT
#define VIRTUAL_GAMEPAD_NAME "Linked GBA"

// DIFFS
#define TEMPORAL_DIFF_THRESHOLD 1500
#define TEMPORAL_DIFF_SIZE (TOTAL_PIXELS / 8)
#define SPATIAL_DIFF_BLOCK_SIZE 4
#define SPATIAL_DIFF_SIZE (TOTAL_PIXELS / SPATIAL_DIFF_BLOCK_SIZE / 8)
inline bool SPATIAL_DIFF_IS_REPEATED_BLOCK(uint32_t* colors) {
  return colors[0] == colors[1] && colors[1] == colors[2] &&
         colors[2] == colors[3];
}

// FILES
#define PALETTE_CACHE_FILENAME "palette.cache"

// COMMANDS
#define MIN_COMMAND 0x11000000
#define CMD_RESET 0x98765400
#define CMD_RPI_OFFSET 1
#define CMD_GBA_OFFSET 2
#define CMD_FRAME_START 0x12345610
#define CMD_SPATIAL_DIFFS_START 0x98765420
#define CMD_PIXELS_START 0x98765430
#define CMD_FRAME_END 0x98765440
#define CMD_PAUSE 0x98765480
#define CMD_RESUME 0x98765490

#endif  // PROTOCOL_H
