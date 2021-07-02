#include <tonc.h>

#include "Benchmark.h"
#include "Config.h"
#include "Palette.h"
#include "Protocol.h"
#include "SPISlave.h"

extern "C" {
#include "gsmplayer/player.h"
}

#define VBLANK_TRACKER (pal_obj_mem[0])
#define TRY(ACTION) \
  if (!(ACTION))    \
    goto reset;

// -----
// STATE
// -----

typedef struct {
  u32 expectedPackets;
  u8 temporalDiffs[TEMPORAL_DIFF_SIZE];
  u8 paletteIndexByCompressedIndex[SPATIAL_DIFF_COLOR_LIMIT];
  u8 compressedPixels[TOTAL_PIXELS];
  u8 audioChunks[AUDIO_PADDED_SIZE];
  u32 pixelCount;
  bool isSpatialCompressed;
  bool hasAudio;
  bool isAudioReady;
} State;

SPISlave* spiSlave = new SPISlave();
DATA_EWRAM u16 temporalDiffJumps[TOTAL_PIXELS];
DATA_EWRAM u8 frameBuffer[TOTAL_SCREEN_PIXELS];

// ---------
// BENCHMARK
// ---------

#ifdef BENCHMARK
int main() {
  Benchmark::init();
  Benchmark::mainLoop();
  return 0;
}
#endif

// ------------
// DECLARATIONS
// ------------

void init();
void mainLoop();
bool sendKeysAndReceiveMetadata(State& state);
bool receiveAudio(State& state);
bool receivePixels(State& state);
bool isNewVBlank();
void driveAudio(State& state);
u32 transfer(State& state, u32 packetToSend, bool withRecovery = true);
bool sync(State& state, u32 command);
u32 x(u32 cursor);
u32 y(u32 cursor);

// -----------
// DEFINITIONS
// -----------

#ifndef BENCHMARK
int main() {
  init();
  mainLoop();

  return 0;
}
#endif

inline void init() {
  enableMode4AndBackground2();
  overclockEWRAM();
  enableMosaic(DRAW_SCALE_X, DRAW_SCALE_Y);
  dma3_cpy(pal_bg_mem, MAIN_PALETTE, sizeof(COLOR) * PALETTE_COLORS);
  player_init();
}

CODE_IWRAM void mainLoop() {
  State state;
  state.isAudioReady = false;

reset:
  transfer(state, CMD_RESET, false);

  while (true) {
    state.expectedPackets = 0;
    state.pixelCount = 0;
    state.isSpatialCompressed = false;
    state.hasAudio = false;

    TRY(sync(state, CMD_FRAME_START))
    TRY(sendKeysAndReceiveMetadata(state))
    if (state.hasAudio) {
      TRY(sync(state, CMD_AUDIO))
      TRY(receiveAudio(state))
    }
    TRY(sync(state, CMD_PIXELS))
    TRY(receivePixels(state))
    TRY(sync(state, CMD_FRAME_END))

    if (state.expectedPackets > 0) {
      u32 pixelCursor = temporalDiffJumps[0];
      u32 diffCursor = 1;
      u32 drawCursor, drawCursor32Bit;
#define DRAW_UNIQUE_PIXEL(PIXEL)                                           \
  drawCursor = y(pixelCursor) * DRAW_WIDTH + x(pixelCursor);               \
  drawCursor32Bit = drawCursor / 4;                                        \
  frameBuffer[drawCursor] =                                                \
      state.isSpatialCompressed                                            \
          ? state.paletteIndexByCompressedIndex[PIXEL &                    \
                                                ~SPATIAL_DIFF_COLOR_LIMIT] \
          : PIXEL;                                                         \
  ((u32*)vid_mem_front)[drawCursor32Bit] = ((u32*)frameBuffer)[drawCursor32Bit];

#define DRAW(PIXEL)                                                           \
  if (diffCursor >= state.pixelCount)                                         \
    continue;                                                                 \
  DRAW_UNIQUE_PIXEL(PIXEL);                                                   \
  if (state.isSpatialCompressed && (PIXEL & SPATIAL_DIFF_COLOR_LIMIT) != 0) { \
    pixelCursor++;                                                            \
    diffCursor++;                                                             \
    DRAW_UNIQUE_PIXEL(PIXEL)                                                  \
  }                                                                           \
  pixelCursor += temporalDiffJumps[diffCursor];                               \
  diffCursor++;

      for (u32 i = 0; i < state.expectedPackets * PIXELS_PER_PACKET; i++) {
        u8 pixel = state.compressedPixels[i];

        DRAW(pixel)
      }
    }
  }
}

inline bool sendKeysAndReceiveMetadata(State& state) {
  u16 keys = pressedKeys();
  u32 expectedPackets = spiSlave->transfer(keys);
  if (spiSlave->transfer(expectedPackets) != keys)
    return false;

  state.expectedPackets = expectedPackets & PACKS_BIT_MASK;
  state.isSpatialCompressed = (expectedPackets & COMPR_BIT_MASK) != 0;
  state.hasAudio = (expectedPackets & AUDIO_BIT_MASK) != 0;

  for (u32 i = 0; i < TEMPORAL_DIFF_SIZE / PACKET_SIZE; i++)
    ((u32*)state.temporalDiffs)[i] = transfer(state, i);

  if (state.isSpatialCompressed)
    for (u32 i = 0; i < SPATIAL_DIFF_COLOR_LIMIT / PACKET_SIZE; i++)
      ((u32*)state.paletteIndexByCompressedIndex)[i] = transfer(state, i);

  // ---
  // TODO: REFACTOR
  u32 cursor = 0;
  u32 currentJump = 0;
#define SAVE_JUMP(CHANGED_BIT)               \
  currentJump++;                             \
  if ((CHANGED_BIT) != 0) {                  \
    temporalDiffJumps[cursor] = currentJump; \
    currentJump = 0;                         \
    cursor++;                                \
  }

  for (u32 i = 0; i < TEMPORAL_DIFF_SIZE; i++) {
    u8 packet = state.temporalDiffs[i];

    if (packet == 0) {
      currentJump += 8 - (i == 0);
      continue;
    }

    if (i > 0) {
      SAVE_JUMP(packet & (1 << 0))
    }
    SAVE_JUMP(packet & (1 << 1))
    SAVE_JUMP(packet & (1 << 2))
    SAVE_JUMP(packet & (1 << 3))
    SAVE_JUMP(packet & (1 << 4))
    SAVE_JUMP(packet & (1 << 5))
    SAVE_JUMP(packet & (1 << 6))
    SAVE_JUMP(packet & (1 << 7))
  }

  state.pixelCount = cursor;
  // ---

  return true;
}

inline bool receiveAudio(State& state) {
  for (u32 i = 0; i < AUDIO_SIZE_PACKETS; i++)
    ((u32*)state.audioChunks)[i] = transfer(state, i);

  state.isAudioReady = true;

  return true;
}

inline bool receivePixels(State& state) {
  for (u32 i = 0; i < state.expectedPackets; i++)
    ((u32*)state.compressedPixels)[i] = transfer(state, i);

  return true;
}

inline bool isNewVBlank() {
  if (!VBLANK_TRACKER && IS_VBLANK) {
    VBLANK_TRACKER = true;
    return true;
  } else if (VBLANK_TRACKER && !IS_VBLANK)
    VBLANK_TRACKER = false;

  return false;
}

CODE_IWRAM void driveAudio(State& state) {
  if (player_needsData() && state.isAudioReady) {
    player_play((const unsigned char*)state.audioChunks, AUDIO_CHUNK_SIZE);
    state.isAudioReady = false;
  }

  spiSlave->stop();
  player_run();
  spiSlave->start();
}

inline u32 transfer(State& state, u32 packetToSend, bool withRecovery) {
  bool breakFlag = false;
  u32 receivedPacket =
      spiSlave->transfer(packetToSend, isNewVBlank, &breakFlag);

  if (breakFlag) {
    driveAudio(state);

    if (withRecovery) {
      sync(state, CMD_RECOVERY);
      spiSlave->transfer(packetToSend);
      receivedPacket = spiSlave->transfer(packetToSend);
    }
  }

  return receivedPacket;
}

inline bool sync(State& state, u32 command) {
  u32 local = command + CMD_GBA_OFFSET;
  u32 remote = command + CMD_RPI_OFFSET;
  bool wasVBlank = IS_VBLANK;

  while (true) {
    bool breakFlag = false;
    bool isOnSync =
        spiSlave->transfer(local, isNewVBlank, &breakFlag) == remote;

    if (breakFlag) {
      driveAudio(state);
      continue;
    }

    if (isOnSync)
      return true;
    else {
      bool isVBlank = IS_VBLANK;

      if (!wasVBlank && isVBlank)
        wasVBlank = true;
      else if (wasVBlank && !isVBlank)
        return false;
    }
  }
}

inline u32 x(u32 cursor) {
  return (cursor % RENDER_WIDTH) * DRAW_SCALE_X;
}

inline u32 y(u32 cursor) {
  return (cursor / RENDER_WIDTH) * DRAW_SCALE_Y;
}
