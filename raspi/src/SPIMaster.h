#ifndef SPI_MASTER_H
#define SPI_MASTER_H

#include <stdlib.h>
#include <iostream>

#include <byteswap.h>
#include "bcm2835.h"

class SPIMaster {
 public:
  SPIMaster(uint8_t mode,
            uint32_t slowFrequency,
            uint32_t fastFrequency,
            uint32_t delayMicroseconds,
            uint32_t gpioBusyFlagPin) {
    initialize();
    bcm2835_spi_setDataMode(mode);
    bcm2835_gpio_fsel(gpioBusyFlagPin, BCM2835_GPIO_FSEL_INPT);

    this->slowFrequency = slowFrequency;
    this->fastFrequency = fastFrequency;
    this->delayMicroseconds = delayMicroseconds;
    this->gpioBusyFlagPin = gpioBusyFlagPin;
  }

  void send(uint32_t value) {
    bcm2835_spi_set_speed_hz(fastFrequency);
    transfer(value);
  }

  uint32_t exchange(uint32_t value) {
    bcm2835_spi_set_speed_hz(slowFrequency);
    return transfer(value);
  }

  int canTransfer() { return !bcm2835_gpio_lev(gpioBusyFlagPin); }

  ~SPIMaster() { bcm2835_spi_end(); }

 private:
  uint32_t slowFrequency;
  uint32_t fastFrequency;
  uint32_t delayMicroseconds;
  uint32_t gpioBusyFlagPin;

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
    if (!canTransfer())
      return 0xffffffff;

    union {
      uint32_t u32;
      char uc[4];
    } x;

    x.u32 = bswap_32(value);
    bcm2835_delayMicroseconds(delayMicroseconds);
    bcm2835_spi_transfern(x.uc, 4);
    return bswap_32(x.u32);
  }
};

#endif  // SPI_MASTER_H
