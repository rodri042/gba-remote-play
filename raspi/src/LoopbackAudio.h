#ifndef LOOPBACK_AUDIO_H
#define LOOPBACK_AUDIO_H

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include "Protocol.h"

#define AUDIO_COMMAND                                                  \
  "ffmpeg -f alsa -i hw:0,1,0 -y -ac 1 -af 'aresample=18157' -strict " \
  "unofficial -c:a gsm -f gsm -loglevel quiet -"

#define AUDIO_NULL "/dev/null"

class LoopbackAudio {
 public:
  LoopbackAudio() { launchEncoder(); }

  uint8_t* loadChunk() {
    uint8_t* chunk = (uint8_t*)malloc(AUDIO_PADDED_SIZE);

    consumeExtraChunks();
    fread(chunk, AUDIO_CHUNK_SIZE, 1, pipe);

    return chunk;
  }

  ~LoopbackAudio() {
    pclose(pipe);
    close(nullFd);
  }

 private:
  FILE* pipe;
  int nullFd;

  void launchEncoder() {
    pipe = popen(AUDIO_COMMAND, "r");

    if (!pipe) {
      std::cout << "Error: cannot launch ffmpeg\n";
      exit(31);
    }

    nullFd = open(AUDIO_NULL, O_RDWR);
  }

  void consumeExtraChunks() {
    uint32_t availableBytes = 0;
    int fd = fileno(pipe);

    ioctl(fd, FIONREAD, &availableBytes);
    if (availableBytes > AUDIO_CHUNK_SIZE)
      splice(fd, NULL, nullFd, NULL, availableBytes - AUDIO_CHUNK_SIZE, 0);
  }
};

#endif  // LOOPBACK_AUDIO_H