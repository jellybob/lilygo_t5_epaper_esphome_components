#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/display/display_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "ed047tc1.h"
#ifdef __cplusplus
}
#endif

#define EPD_WIDTH  960
#define EPD_HEIGHT 540

namespace esphome {
namespace ed047tc1 {

class ED047TC1Display : public display::DisplayBuffer {
 public:
  void setup() override;
  void update() override;
  void fill(Color color) override;
  void dump_config() override;
  float get_setup_priority() const override { return esphome::setup_priority::HARDWARE; }

  display::DisplayType get_display_type() override {
    return display::DisplayType::DISPLAY_TYPE_GRAYSCALE;
  }

 protected:
  void draw_absolute_pixel_internal(int x, int y, Color color) override;
  int get_width_internal() override { return EPD_WIDTH; }
  int get_height_internal() override { return EPD_HEIGHT; }

  uint8_t *prev_buffer_{nullptr};
};

}  // namespace ed047tc1
}  // namespace esphome
