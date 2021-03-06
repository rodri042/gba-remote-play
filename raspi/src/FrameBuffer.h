#ifndef FRAME_BUFFER_H
#define FRAME_BUFFER_H

#include <bcm_host.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <iostream>

#define FB_DEVFILE "/dev/fb0"
#define FB_BYTES_PER_PIXEL 4
#define FB_IMAGE_MODE VC_IMAGE_ARGB8888

class FrameBuffer {
 public:
  FrameBuffer(uint32_t expectedXRes, uint32_t expectedYRes) {
    openFrameBuffer();
    retrieveFixedScreenInformation();
    retrieveVariableScreenInformation(expectedXRes, expectedYRes);
    allocateBuffer();

    openPrimaryDisplay();
    createScreenResource();
    setUpRect();
  }

  uint8_t* loadFrame() {
    vc_dispmanx_snapshot(display, screenResource, (DISPMANX_TRANSFORM_T)0);
    vc_dispmanx_resource_read_data(screenResource, &rect, buffer,
                                   variableInfo.xres * FB_BYTES_PER_PIXEL);

    return buffer;
  }

  template <typename F>
  inline void forEachPixel(F action) {
    loadFrame();

    for (int y = 0; y < variableInfo.yres; y++) {
      for (int x = 0; x < variableInfo.xres; x++) {
        size_t offset = x * FB_BYTES_PER_PIXEL + y * fixedInfo.line_length;
        uint32_t pixel = *(uint32_t*)(buffer + offset);

        uint32_t rMask = (1 << variableInfo.red.length) - 1;
        uint32_t gMask = (1 << variableInfo.green.length) - 1;
        uint32_t bMask = (1 << variableInfo.blue.length) - 1;
        uint8_t r = (pixel >> variableInfo.red.offset) & rMask;
        uint8_t g = (pixel >> variableInfo.green.offset) & gMask;
        uint8_t b = (pixel >> variableInfo.blue.offset) & bMask;

        action(x, y, r, g, b);
      }
    }
  }

  ~FrameBuffer() {
    close(fileDescriptor);
    vc_dispmanx_resource_delete(screenResource);
    vc_dispmanx_display_close(display);
  }

 private:
  int fileDescriptor;
  struct fb_fix_screeninfo fixedInfo;
  struct fb_var_screeninfo variableInfo;
  uint8_t* buffer;
  DISPMANX_DISPLAY_HANDLE_T display;
  DISPMANX_RESOURCE_HANDLE_T screenResource;
  VC_IMAGE_TRANSFORM_T transform;
  uint32_t image_prt;
  VC_RECT_T rect;

  void openFrameBuffer() {
    fileDescriptor = open(FB_DEVFILE, O_RDWR);
    if (fileDescriptor < 0) {
      std::cout << "Error (Image): cannot open framebuffer device\n";
      exit(21);
    }
  }

  void retrieveFixedScreenInformation() {
    if (ioctl(fileDescriptor, FBIOGET_FSCREENINFO, &fixedInfo) < 0) {
      std::cout << "Error (Image): cannot read fixed information\n";
      exit(22);
    }
  }

  void retrieveVariableScreenInformation(uint32_t expectedXRes,
                                         uint32_t expectedYRes) {
    if (ioctl(fileDescriptor, FBIOGET_VSCREENINFO, &variableInfo) < 0) {
      std::cout << "Error (Image): cannot read variable information\n";
      exit(23);
    }

    if (variableInfo.bits_per_pixel / 8 != FB_BYTES_PER_PIXEL) {
      std::cout << "Error (Image): only 32bpp is supported\n";
      exit(24);
    }

    if (variableInfo.xres != expectedXRes ||
        variableInfo.yres != expectedYRes) {
      std::cout << "Error (Image): frame buffer resolution doesn't match "
                   "render resolution\n";
      std::cout << "(frame buffer is " + std::to_string(variableInfo.xres) +
                       "x" + std::to_string(variableInfo.yres) + ")\n";
      exit(25);
    }

    if (variableInfo.xres % FB_BYTES_PER_PIXEL != 0 ||
        variableInfo.yres % FB_BYTES_PER_PIXEL != 0) {
      std::cout << "Error (Image): resolution must be word-aligned\n";
      exit(26);
    }
  }

  void allocateBuffer() {
    buffer = (uint8_t*)malloc(fixedInfo.smem_len);
    if (buffer == NULL) {
      std::cout << "Error (Image): malloc(" +
                       std::to_string(fixedInfo.smem_len) + ") failed\n";
      exit(27);
    }
  }

  void openPrimaryDisplay() {
    bcm_host_init();

    display = vc_dispmanx_display_open(0);
    if (display == DISPMANX_NO_HANDLE) {
      std::cout << "Error (Image): cannot open primary display\n";
      exit(28);
    }
  }

  void createScreenResource() {
    screenResource = vc_dispmanx_resource_create(
        FB_IMAGE_MODE, variableInfo.xres, variableInfo.yres, &image_prt);
    if (screenResource == DISPMANX_NO_HANDLE) {
      printf("Error (Image): cannot create screen resource\n");
      exit(29);
    }
  }

  void setUpRect() {
    vc_dispmanx_rect_set(&rect, 0, 0, variableInfo.xres, variableInfo.yres);
  }
};

#endif  // FRAME_BUFFER_H
