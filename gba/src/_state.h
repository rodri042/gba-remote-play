#ifndef STATE_H
#define STATE_H

#include <tonc.h>

#include "Protocol.h"

typedef struct {
  u8 temporalDiffs[TEMPORAL_DIFF_MAX_PADDED_SIZE(TOTAL_SCREEN_PIXELS)];
  u8 audioChunks[AUDIO_PADDED_SIZE];
  u32 expectedPackets;
  u32 startPixel;
  bool isRLE;
  bool hasAudio;
  bool isVBlank;
  bool isAudioReady;
} State;

extern State state;
extern u8 compressedPixels[MAX_PIXELS_SIZE];

#endif  // STATE_H
