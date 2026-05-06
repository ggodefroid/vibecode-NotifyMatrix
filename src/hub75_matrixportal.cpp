#include "hub75_matrixportal.h"

#include "app_config.h"
#include "text_utf8_fold.h"
#include "ui_model.h"

#if defined(BOARD_MATRIXPORTAL_S3)

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <cstring>

#include "boards/matrixportal_s3_pins.h"

static MatrixPanel_I2S_DMA* panel_ptr(void* p)
{
  return static_cast<MatrixPanel_I2S_DMA*>(p);
}

static void print_limited(MatrixPanel_I2S_DMA* d,
                          const char* text,
                          uint8_t max_chars)
{
  for (uint8_t i = 0; i < max_chars && text[i] != '\0'; i++) {
    d->print(text[i]);
  }
}

bool Hub75MatrixPortal::begin(uint8_t initial_brightness8)
{
  if (panel_ != nullptr) {
    return true;
  }

  HUB75_I2S_CFG::i2s_pins pins = {
      MP_S3_R1, MP_S3_G1, MP_S3_B1, MP_S3_R2, MP_S3_G2, MP_S3_B2,
      MP_S3_A,  MP_S3_B,  MP_S3_C,  MP_S3_D,
#if PANEL_HUB75_DISABLE_ROW_E
      -1,
#else
      MP_S3_E,
#endif
      MP_S3_LAT, MP_S3_OE, MP_S3_CLK};

  HUB75_I2S_CFG mxconfig(
      PANEL_MODULE_WIDTH,
      PANEL_MODULE_HEIGHT,
      PANEL_CHAIN_LENGTH,
      pins);

  mxconfig.clkphase = false;

#if defined(PANEL_HUB75_DRIVER_FM6126A)
  mxconfig.driver = HUB75_I2S_CFG::FM6126A;
#elif defined(PANEL_HUB75_DRIVER_ICN2038S)
  mxconfig.driver = HUB75_I2S_CFG::ICN2038S;
#endif

  auto* m = new MatrixPanel_I2S_DMA(mxconfig);

#if PANEL_HUB75_LAT_BLANKING > 0
  m->setLatBlanking((uint8_t)PANEL_HUB75_LAT_BLANKING);
#endif

  if (!m->begin()) {
    delete m;
    panel_ = nullptr;
    return false;
  }

  panel_ = m;
  m->setBrightness8(initial_brightness8);
  m->clearScreen();
  delay(100); // Give the USB CDC monitor a short window to attach.

  Serial.print(F("HUB75 OK - "));
  Serial.print((int)PANEL_MODULE_WIDTH * PANEL_CHAIN_LENGTH);
  Serial.print('x');
  Serial.print((int)PANEL_MODULE_HEIGHT);
#if defined(PANEL_HUB75_DRIVER_FM6126A)
  Serial.println(F(" driver=FM6126A"));
#elif defined(PANEL_HUB75_DRIVER_ICN2038S)
  Serial.println(F(" driver=ICN2038S"));
#else
  Serial.println(F(" driver=SHIFTREG"));
#endif

  return true;
}

void Hub75MatrixPortal::run_boot_rgb_horizontal_thirds(uint32_t hold_ms)
{
  if (panel_ == nullptr) {
    return;
  }
  MatrixPanel_I2S_DMA* d = panel_ptr(panel_);
  const uint16_t total_w = (uint16_t)(PANEL_MODULE_WIDTH * PANEL_CHAIN_LENGTH);
  const uint16_t total_h = (uint16_t)PANEL_MODULE_HEIGHT;
  const uint16_t third = (uint16_t)(total_w / 3);

  for (uint16_t y = 0; y < total_h; y++) {
    for (uint16_t x = 0; x < total_w; x++) {
      if (x < third) {
        d->drawPixelRGB888(x, y, 255, 0, 0);
      } else if (x < third * 2) {
        d->drawPixelRGB888(x, y, 0, 255, 0);
      } else {
        d->drawPixelRGB888(x, y, 0, 0, 255);
      }
    }
  }
  delay(hold_ms);
  d->clearScreen();
}

void Hub75MatrixPortal::fill_screen_rgb888(uint8_t r, uint8_t g, uint8_t b)
{
  if (panel_ == nullptr) {
    return;
  }
  panel_ptr(panel_)->fillScreenRGB888(r, g, b);
}

void Hub75MatrixPortal::draw_hello_world()
{
  if (panel_ == nullptr) {
    return;
  }
  MatrixPanel_I2S_DMA* d = panel_ptr(panel_);
  d->fillScreenRGB888(0, 0, 80);

  d->setTextWrap(false);
  d->setTextSize(2);

  d->setCursor(2, 0);
  d->setTextColor(d->color565(255, 255, 0), d->color565(0, 0, 80));
  d->print("HELLO");

  d->setCursor(2, 16);
  d->setTextColor(d->color565(255, 255, 255), d->color565(0, 0, 80));
  d->print("WORLD");
}

void Hub75MatrixPortal::draw_ui(const UiModel& model)
{
  if (panel_ == nullptr) {
    return;
  }

  MatrixPanel_I2S_DMA* d = panel_ptr(panel_);
  const uint16_t black = d->color565(0, 0, 0);
  const uint16_t amber = d->color565(255, 150, 0);
  const uint16_t sep_blue = d->color565(40, 120, 255);
  const uint16_t notify_bg = d->color565(0, 110, 28);
  const uint16_t white = d->color565(255, 255, 255);
  const uint16_t eta_blue_dim = d->color565(40, 120, 255);
  const uint16_t eta_green = d->color565(0, 220, 100);
  const uint16_t eta_orange = d->color565(255, 110, 0);
  const uint16_t eta_red = d->color565(255, 32, 28);
  const uint16_t warn = d->color565(255, 200, 0);
  const uint16_t cyan = d->color565(0, 200, 255);

  const int16_t full_h = (int16_t)DISPLAY_TOTAL_HEIGHT;
  const int16_t S = (int16_t)model.notify_shrink_px;
  const int16_t content_h = full_h - S;
  const int16_t y_shift =
      (int16_t)((full_h * (int32_t)HUD_VERTICAL_OFFSET_PERCENT + 50) / 100);

  const int16_t split_nom = (full_h > 20) ? 14 : (int16_t)(full_h * 3 / 5);
  const int16_t bus_line_nom = 4;
  const int16_t eta_y_nom = split_nom + 3;

  const int16_t tx0 = (int16_t)ZONE_TIME_X;
  const int16_t tw = (int16_t)ZONE_TIME_W;
  const int16_t bx0 = (int16_t)ZONE_BUS_X;
  const int16_t bw = (int16_t)ZONE_BUS_W;

  d->fillScreen(black);
  d->setTextWrap(false);

  if (content_h > 8) {
    d->setTextSize(3);
    d->setTextColor(amber, black);
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t twd = 0;
    uint16_t tht = 0;
    d->getTextBounds(model.time_text, 0, 0, &x1, &y1, &twd, &tht);
    (void)x1;
    (void)y1;
    const int16_t time_ty_nom =
        (int16_t)((full_h - (int16_t)tht) / 2) + y_shift;
    const int16_t time_ty = time_ty_nom - S;
    const int16_t tcx = (int16_t)(tx0 + tw - (int16_t)twd - 2);
    d->setCursor(tcx, time_ty);
    d->print(model.time_text);
  }

  const int16_t vline_y = y_shift > 0 ? y_shift - 1 : 0;
  const int16_t vline_h = (content_h - vline_y) > 0 ? (content_h - vline_y) : 1;
  d->drawFastVLine(bx0 - 2,
                   vline_y,
                   vline_h,
                   sep_blue);
  d->drawFastVLine(bx0 - 1,
                   vline_y,
                   vline_h,
                   sep_blue);

  if (content_h > 6) {
    d->setTextSize(1);
    d->setTextColor(amber, black);
    int16_t lx1 = 0;
    int16_t ly1 = 0;
    uint16_t lw = 0;
    uint16_t lh = 0;
    d->getTextBounds(model.bus_line, 0, 0, &lx1, &ly1, &lw, &lh);
    (void)lx1;
    (void)ly1;
    const int16_t lcx = (int16_t)(bx0 + (bw - (int16_t)lw) / 2);
    const int16_t line_y = bus_line_nom - S + y_shift - 1;
    if (line_y + (int16_t)lh <= content_h && line_y >= -2) {
      d->setCursor(lcx, line_y);
      print_limited(d, model.bus_line, 8);
    }

    const int16_t split_draw = split_nom - S + y_shift;
    if (split_draw >= 1 && split_draw < content_h) {
      d->drawFastHLine(bx0, split_draw - 1, bw, sep_blue);
      d->drawFastHLine(bx0, split_draw, bw, sep_blue);
    }

    uint16_t eta_color = amber;
    if (model.bus_state == BusState::Loading) {
      eta_color = cyan;
    } else if (model.bus_state == BusState::Error) {
      eta_color = warn;
    } else if (model.bus_state == BusState::Ready && model.bus_eta_minutes >= 0) {
      const int m = model.bus_eta_minutes;
      if (m > 10) {
        eta_color = eta_green;
      } else if (m >= 7) {
        eta_color = eta_orange;
      } else {
        eta_color = eta_red;
      }
    } else if (model.bus_state == BusState::Ready) {
      eta_color = eta_blue_dim;
    }

    const bool special_eta_text = std::strcmp(model.bus_text, "PCH") == 0 ||
                                  std::strcmp(model.bus_text, "QUAI") == 0;
    const bool blink_on = special_eta_text && (((millis() / 500) & 1) == 0);
    d->setTextColor(blink_on ? white : eta_color, black);
    int16_t ex1 = 0;
    int16_t ey1 = 0;
    uint16_t ew = 0;
    uint16_t eh = 0;
    d->getTextBounds(model.bus_text, 0, 0, &ex1, &ey1, &ew, &eh);
    (void)ex1;
    (void)ey1;
    const int16_t ecx = (int16_t)(bx0 + (bw - (int16_t)ew) / 2);
    const int16_t eta_y = eta_y_nom - S + y_shift + 2;
    if (eta_y + (int16_t)eh <= content_h && eta_y >= -2) {
      d->setCursor(ecx, eta_y);
      print_limited(d, model.bus_text, 5);
    }
  }

  if (S > 0) {
    d->fillRect(0, content_h, (int16_t)DISPLAY_TOTAL_WIDTH, S, notify_bg);
    if (model.notify_scroll_visible && model.notification[0] != '\0') {
      char folded[NOTIFICATION_TEXT_MAX];
      text_utf8_fold_latin(model.notification, folded, sizeof(folded));
      d->setTextSize(1);
      d->setTextColor(white, notify_bg);
      d->setCursor(model.notify_scroll_x, content_h);
      print_limited(d, folded, NOTIFICATION_TEXT_MAX - 1);
    }
  }
  const uint16_t indicator_color = model.bus_theoretical
      ? d->color565(180, 0, 200)
      : d->color565(0, 200, 80);
  d->fillRect((int16_t)DISPLAY_TOTAL_WIDTH - 2, full_h - 2, 2, 2, indicator_color);
}

void Hub75MatrixPortal::clear_screen()
{
  if (panel_ == nullptr) {
    return;
  }
  panel_ptr(panel_)->clearScreen();
}

void Hub75MatrixPortal::set_brightness8(uint8_t value)
{
  if (panel_ == nullptr) {
    return;
  }
  panel_ptr(panel_)->setBrightness8(value);
}

void Hub75MatrixPortal::flip_dma_buffer()
{
  if (panel_ == nullptr) {
    return;
  }
  panel_ptr(panel_)->flipDMABuffer();
}

#else // !BOARD_MATRIXPORTAL_S3

bool Hub75MatrixPortal::begin(uint8_t)
{
  return false;
}

void Hub75MatrixPortal::run_boot_rgb_horizontal_thirds(uint32_t) {}

void Hub75MatrixPortal::fill_screen_rgb888(uint8_t, uint8_t, uint8_t) {}

void Hub75MatrixPortal::draw_hello_world() {}

void Hub75MatrixPortal::draw_ui(const UiModel&) {}

void Hub75MatrixPortal::clear_screen() {}

void Hub75MatrixPortal::set_brightness8(uint8_t) {}

void Hub75MatrixPortal::flip_dma_buffer() {}

#endif
