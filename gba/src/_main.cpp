#include <tonc.h>

#include "Benchmark.h"
#include "BuildConfig.h"
#include "Palette.h"
#include "Protocol.h"
#include "SPISlave.h"
#include "Utils.h"

extern "C" {
#include "gsmplayer/player.h"
}

#define DRAW_PIXEL(PIXEL) m4Draw(y(cursor) * DRAW_WIDTH + x(cursor), PIXEL);
#define VBLANK_TRACKER (pal_obj_mem[0])
#define TRY(ACTION) \
  if (!(ACTION))    \
    goto reset;

// -----
// STATE
// -----

typedef struct {
  u8 temporalDiffs[DIFF_SIZE];
  u8 spatialDiffs[DIFF_SIZE];
  u8 audioChunks[AUDIO_PADDED_SIZE];
  u32 expectedPackets;
  u32 startPixel;
  bool hasAudio;
  bool isAudioReady;
} State;

SPISlave* spiSlave = new SPISlave();
DATA_EWRAM u8 compressedPixels[TOTAL_PIXELS];

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
void render(State& state);
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
  enableMosaic(DRAW_SCALE_X, DRAW_SCANLINES ? 1 : DRAW_SCALE_Y);
  dma3_cpy(pal_bg_mem, MAIN_PALETTE, sizeof(COLOR) * PALETTE_COLORS);
  player_init();
}

CODE_IWRAM void mainLoop() {
  State state;
  state.isAudioReady = false;

reset:
  transfer(state, CMD_RESET, false);

  while (true) {
    TRY(sync(state, CMD_FRAME_START))
    TRY(sendKeysAndReceiveMetadata(state))
    if (state.hasAudio) {
      TRY(sync(state, CMD_AUDIO))
      TRY(receiveAudio(state))
    }
    TRY(sync(state, CMD_PIXELS))
    TRY(receivePixels(state))
    TRY(sync(state, CMD_FRAME_END))

    render(state);
  }
}

inline bool sendKeysAndReceiveMetadata(State& state) {
  u16 keys = pressedKeys();
  u32 metadata = spiSlave->transfer(keys);
  if (spiSlave->transfer(metadata) != keys)
    return false;

  state.expectedPackets = (metadata >> PACKS_BIT_OFFSET) & PACKS_BIT_MASK;
  state.startPixel = metadata & START_BIT_MASK;
  state.hasAudio = (metadata & AUDIO_BIT_MASK) != 0;

  u32 diffsStart = (state.startPixel / 8) / PACKET_SIZE;

  for (u32 i = diffsStart; i < DIFF_SIZE / PACKET_SIZE; i++)
    ((u32*)state.temporalDiffs)[i] = transfer(state, i);

  for (u32 i = diffsStart; i < DIFF_SIZE / PACKET_SIZE; i++)
    ((u32*)state.spatialDiffs)[i] = transfer(state, i);

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
    ((u32*)compressedPixels)[i] = transfer(state, i);

  return true;
}

inline void render(State& state) {
  u32 decompressedPixels = 0;
  bool wasVBlank = IS_VBLANK;

  u32 cursor = state.startPixel;
  while (cursor < TOTAL_PIXELS) {
    if (!wasVBlank && IS_VBLANK) {
      wasVBlank = true;
      driveAudio(state);
    } else if (wasVBlank && !IS_VBLANK)
      wasVBlank = false;

    u32 diffByte = cursor / 8;
    u32 diffBit = cursor % 8;
    u32 temporalDiff = state.temporalDiffs[diffByte];

    if (diffBit == 0) {
      if (((u32*)state.temporalDiffs)[diffByte / 4] == 0) {
        // (no changes in the next 32 pixels)
        cursor += 32;
        continue;
      } else if (((u16*)state.temporalDiffs)[diffByte / 2] == 0) {
        // (no changes in the next 16 pixels)
        cursor += 16;
        continue;
      } else if (temporalDiff == 0) {
        // (no changes in the next 8 pixels)
        cursor += 8;
        continue;
      }
    }

    if (BIT_IS_HIGH(temporalDiff, diffBit)) {
      // (a pixel changed)

      u8 pixel = compressedPixels[decompressedPixels];
      DRAW_PIXEL(pixel);

      u32 spatialDiff = state.spatialDiffs[diffByte];
      if (BIT_IS_HIGH(spatialDiff, diffBit)) {
        // (repeated color)
        cursor++;
        DRAW_PIXEL(pixel);
      }

      decompressedPixels++;
    }

    cursor++;
  }
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
