#ifndef GBA_REMOTE_PLAY_H
#define GBA_REMOTE_PLAY_H

#include "Benchmark.h"
#include "BuildConfig.h"
#include "Config.h"
#include "Frame.h"
#include "FrameBuffer.h"
#include "ImageDiffRLECompressor.h"
#include "LoopbackAudio.h"
#include "PNGWriter.h"
#include "Palette.h"
#include "Protocol.h"
#include "ReliableStream.h"
#include "SPIMaster.h"
#include "Utils.h"
#include "VirtualGamepad.h"

#define TRY(ACTION) \
  if (!(ACTION))    \
    return false;

uint8_t LUT_24BPP_TO_8BIT_PALETTE[PALETTE_24BIT_MAX_COLORS];

class GBARemotePlay {
 public:
  GBARemotePlay() {
    config = new Config(CONFIG_FILENAME);
    spiMaster = new SPIMaster(SPI_MODE, config->spiNormalTiming,
                              config->spiOverclockedTiming);
    reliableStream = new ReliableStream(spiMaster);
    frameBuffer = new FrameBuffer(DRAW_WIDTH, DRAW_HEIGHT);
    loopbackAudio = new LoopbackAudio();
    virtualGamepad =
        new VirtualGamepad(config->virtualGamepadName, CONTROLS_FILENAME);
    lastFrame = Frame{0};
    renderMode = DEFAULT_RENDER_MODE;

    PALETTE_initializeCache(PALETTE_CACHE_FILENAME);
  }

  void run() {
#ifdef PROFILE
    auto startTime = PROFILE_START();
    uint32_t frames = 0;
#endif

  reset:
    syncReset();

    while (true) {
#ifdef DEBUG
      LOG("Waiting...");
      int _input;
      std::cin >> _input;
#endif

#ifdef PROFILE_VERBOSE
      auto frameGenerationStartTime = PROFILE_START();
#endif

      auto frame = loadFrame();

#ifdef PROFILE_VERBOSE
      auto frameGenerationElapsedTime = PROFILE_END(frameGenerationStartTime);
      auto frameDiffsStartTime = PROFILE_START();
#endif

      ImageDiffRLECompressor diffs;
      diffs.initialize(frame, lastFrame, diffThreshold, renderMode);

#ifdef PROFILE_VERBOSE
      auto frameDiffsElapsedTime = PROFILE_END(frameDiffsStartTime);
      auto frameTransferStartTime = PROFILE_START();
#endif

      if (!send(frame, diffs)) {
        frame.clean();
        lastFrame.clean();
        goto reset;
      }

      lastFrame.clean();
      lastFrame = frame;

#ifdef PROFILE_VERBOSE
      auto frameTransferElapsedTime = PROFILE_END(frameTransferStartTime);
      LOG("(build: " + std::to_string(frameGenerationElapsedTime) +
          "ms, diffs: " + std::to_string(frameDiffsElapsedTime) +
          "ms, transfer: " + std::to_string(frameTransferElapsedTime) + "ms)");
#endif

#ifdef PROFILE
      frames++;
      uint32_t elapsedTime = PROFILE_END(startTime);
      if (elapsedTime >= ONE_SECOND) {
        LOG("--- " + std::to_string(frames) + " frames ---");
        startTime = PROFILE_START();
        frames = 0;
      }
#endif
    }
  }

  ~GBARemotePlay() {
    lastFrame.clean();
    delete config;
    delete spiMaster;
    delete reliableStream;
    delete frameBuffer;
    delete loopbackAudio;
    delete virtualGamepad;
  }

 private:
  Config* config;
  SPIMaster* spiMaster;
  ReliableStream* reliableStream;
  FrameBuffer* frameBuffer;
  LoopbackAudio* loopbackAudio;
  VirtualGamepad* virtualGamepad;
  Frame lastFrame;
  uint32_t renderMode;
  uint32_t diffThreshold;
  uint32_t input;

  bool send(Frame& frame, ImageDiffRLECompressor& diffs) {
    if (!frame.hasData())
      return false;

#ifdef PROFILE_VERBOSE
    auto idleStartTime = PROFILE_START();
#endif

    DEBULOG("Syncing frame start...");
    TRY(reliableStream->sync(CMD_FRAME_START))

#ifdef PROFILE_VERBOSE
    auto idleElapsedTime = PROFILE_END(idleStartTime);
    LOG("  <" + std::to_string(idleElapsedTime) + "ms idle>");
    auto metadataStartTime = PROFILE_START();
#endif

    DEBULOG("Receiving keys and send metadata...");
    TRY(receiveKeysAndSendMetadata(frame, diffs))

#ifdef PROFILE_VERBOSE
    auto metadataElapsedTime = PROFILE_END(metadataStartTime);
    LOG("  <" + std::to_string(metadataElapsedTime) + "ms metadata>");
#endif

    if (frame.hasAudio()) {
      DEBULOG("Syncing audio...");
      TRY(reliableStream->sync(CMD_AUDIO))

      DEBULOG("Sending audio...");
      TRY(sendAudio(frame))
    }

    DEBULOG("Syncing pixels...");
    TRY(reliableStream->sync(CMD_PIXELS))

    DEBULOG("Sending pixels...");
    TRY(compressAndSendPixels(frame, diffs))

    DEBULOG("Syncing frame end...");
    TRY(reliableStream->sync(CMD_FRAME_END))

#ifdef DEBUG_PNG
    LOG("Writing debug PNG file...");
    WritePNG("debug.png", frame.raw8BitPixels, MAIN_PALETTE_24BPP,
             RENDER_MODE_WIDTH[renderMode], RENDER_MODE_HEIGHT[renderMode]);
    LOG("Frame end!");
#endif

    return true;
  }

  void syncReset() {
    uint32_t resetPacket;
    while (!IS_RESET(resetPacket = spiMaster->exchange(0)))
      ;
    spiMaster->exchange(resetPacket);

    renderMode = resetPacket & RENDER_MODE_BIT_MASK;
    virtualGamepad->setCurrentConfiguration(
        (resetPacket >> CONTROLS_BIT_OFFSET) & CONTROLS_BIT_MASK);
    diffThreshold = DIFF_THRESHOLDS[(resetPacket >> COMPRESSION_BIT_OFFSET) &
                                    COMPRESSION_BIT_MASK];
    spiMaster->setOverclocked((resetPacket >> CPU_OVERCLOCK_BIT_OFFSET) &
                              CPU_OVERCLOCK_BIT_MASK);

    if (RENDER_MODE_IS_BENCHMARK(renderMode))
      Benchmark::main(renderMode);
  }

  bool receiveKeysAndSendMetadata(Frame& frame, ImageDiffRLECompressor& diffs) {
  again:
    uint32_t metadata = diffs.startPixel |
                        (diffs.expectedPackets() << PACKS_BIT_OFFSET) |
                        (diffs.shouldUseRLE() ? COMPR_BIT_MASK : 0) |
                        (frame.hasAudio() ? AUDIO_BIT_MASK : 0);
    uint32_t keys = spiMaster->exchange(metadata);
    if (reliableStream->finishSyncIfNeeded(keys, CMD_FRAME_START))
      goto again;
    if (spiMaster->exchange(keys) != metadata)
      return false;

    processKeys(keys);

    uint32_t diffStart = (diffs.startPixel / 8) / PACKET_SIZE;
    spiMaster->exchange(diffs.temporalDiffEndPacket);
    return reliableStream->send(diffs.temporalDiffs,
                                diffs.temporalDiffEndPacket, CMD_FRAME_START,
                                diffStart);
  }

  bool sendAudio(Frame& frame) {
    return reliableStream->send(frame.audioChunk, AUDIO_SIZE_PACKETS,
                                CMD_AUDIO);
  }

  bool compressAndSendPixels(Frame& frame, ImageDiffRLECompressor& diffs) {
    uint32_t packetsToSend[MAX_PIXELS_SIZE];
    uint32_t size = 0;
    compressPixels(frame, diffs, packetsToSend, &size);

#ifdef DEBUG
    if (size != diffs.expectedPackets()) {
      LOG("[!!!] Sizes don't match (" + std::to_string(size) + " vs " +
          std::to_string(diffs.expectedPackets()) + ")");
    }
#endif

#ifdef PROFILE_VERBOSE
    LOG("  <" + std::to_string(size * PACKET_SIZE) + "bytes" +
        (diffs.shouldUseRLE()
             ? ", rle (" + std::to_string(diffs.omittedRLEPixels()) +
                   " omitted)"
             : "") +
        (frame.hasAudio() ? ", audio>" : ">"));
#endif

    return reliableStream->send(packetsToSend, size, CMD_PIXELS);
  }

  void compressPixels(Frame& frame,
                      ImageDiffRLECompressor& diffs,
                      uint32_t* packets,
                      uint32_t* totalPackets) {
    uint32_t currentPacket = 0;
    uint8_t byte = 0;

#define ADD_BYTE(DATA)                      \
  currentPacket |= DATA << (byte * 8);      \
  byte++;                                   \
  if (byte == PACKET_SIZE) {                \
    packets[*totalPackets] = currentPacket; \
    currentPacket = 0;                      \
    byte = 0;                               \
    (*totalPackets)++;                      \
  }

    if (diffs.shouldUseRLE()) {
      uint32_t rleIndex = 0, pixelIndex = 0;

      while (rleIndex < diffs.totalEncodedPixels()) {
        uint8_t times = diffs.runLengthEncoding[rleIndex];
        uint8_t pixel = diffs.compressedPixels[pixelIndex];

        ADD_BYTE(times)
        ADD_BYTE(pixel)

        pixelIndex += times;
        rleIndex++;
      }
    } else {
      for (int i = 0; i < diffs.totalCompressedPixels; i++) {
        uint8_t pixel = diffs.compressedPixels[i];
        ADD_BYTE(pixel)
      }
    }

    if (byte > 0) {
      packets[*totalPackets] = currentPacket;
      (*totalPackets)++;
    }
  }

  Frame loadFrame() {
    Frame frame;
    frame.totalPixels = RENDER_MODE_PIXELS[renderMode];
    frame.raw8BitPixels = (uint8_t*)malloc(RENDER_MODE_PIXELS[renderMode]);
    frame.palette = MAIN_PALETTE_24BPP;

    uint32_t width = RENDER_MODE_WIDTH[renderMode];
    uint32_t scaleX = RENDER_MODE_SCALEX[renderMode];
    uint32_t scaleY = RENDER_MODE_SCALEY[renderMode];

    frameBuffer->forEachPixel(
        [&frame, &width, &scaleX, &scaleY, this](int x, int y, uint8_t r,
                                                 uint8_t g, uint8_t b) {
          if (x % scaleX != 0 || y % scaleY != 0)
            return;
          x = x / scaleX;
          y = y / scaleY;

          frame.raw8BitPixels[y * width + x] =
              LUT_24BPP_TO_8BIT_PALETTE[(r << 0) | (g << 8) | (b << 16)];
        });

    frame.audioChunk = loopbackAudio->loadChunk();

    return frame;
  }

  void processKeys(uint16_t keys) { virtualGamepad->setButtons(keys); }
};

#endif  // GBA_REMOTE_PLAY_H
