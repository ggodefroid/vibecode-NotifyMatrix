#pragma once

#include <cstdint>

struct UiModel;

// HUB75 driver for Matrix Portal S3 with horizontally chained panels.

class Hub75MatrixPortal {
public:
  bool begin(uint8_t initial_brightness8 = 128);
  bool ok() const { return panel_ != nullptr; }

  // RGB bands across thirds of the full width, useful as a boot wiring test.
  void run_boot_rgb_horizontal_thirds(uint32_t hold_ms = 1200);

  // Fill the whole display with one color for power and pixel tests.
  void fill_screen_rgb888(uint8_t r, uint8_t g, uint8_t b);

  // Draw text through Adafruit_GFX using the default bitmap font.
  void draw_hello_world();
  void draw_ui(const UiModel& model);

  void clear_screen();
  void set_brightness8(uint8_t value);
  void flip_dma_buffer();

private:
  void* panel_ = nullptr; // MatrixPanel_I2S_DMA* when BOARD_MATRIXPORTAL_S3 is enabled.
};
