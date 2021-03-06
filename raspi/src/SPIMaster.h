#ifndef SPI_MASTER_H
#define SPI_MASTER_H

#include <stdlib.h>
#include <iostream>

#include <byteswap.h>
#include "bcm2835.h"

#define SPI_MISO_PIN 9

typedef struct {
  uint32_t slowFrequency;
  uint32_t fastFrequency;
  uint32_t delayMicroseconds;

  void reset() {
    slowFrequency = 0;
    fastFrequency = 0;
    delayMicroseconds = 0;
  }
} SPITiming;

class SPIMaster {
 public:
  bool isOverclocked = false;

  SPIMaster(uint8_t mode, SPITiming normalTiming, SPITiming overclockedTiming) {
    initialize();
    bcm2835_spi_setDataMode(mode);

    this->normalTiming = normalTiming;
    this->overclockedTiming = overclockedTiming;
  }

  void send(uint32_t value) {
    bcm2835_spi_set_speed_hz(timing().fastFrequency);
    transfer(value);
  }

  uint32_t exchange(uint32_t value) {
    bcm2835_spi_set_speed_hz(timing().slowFrequency);
    return transfer(value);
  }

  void setOverclocked(bool isOverclocked) {
    this->isOverclocked = isOverclocked;
  }

  ~SPIMaster() { bcm2835_spi_end(); }

 private:
  SPITiming normalTiming, overclockedTiming;

  SPITiming timing() {
    return isOverclocked ? overclockedTiming : normalTiming;
  }

  bool isSlaveBusy() { return bcm2835_gpio_lev(SPI_MISO_PIN); }

  void initialize() {
    if (!bcm2835_init()) {
      std::cout << "Error (SPI): cannot initialize SPI\n";
      exit(11);
    }

    if (!bcm2835_spi_begin()) {
      std::cout << "Error (SPI): cannot start SPI transfers\n";
      exit(12);
    }
  }

  uint32_t transfer(uint32_t value) {
    union {
      uint32_t u32;
      char uc[4];
    } x;
    x.u32 = bswap_32(value);

    bcm2835_delayMicroseconds(timing().delayMicroseconds);

#ifndef WITH_AUDIO
    while (isSlaveBusy())
      ;
#endif

    bcm2835_spi_transfern(x.uc, 4);

    return bswap_32(x.u32);
  }
};

#endif  // SPI_MASTER_H
