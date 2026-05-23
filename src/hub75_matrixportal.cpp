#include "hub75_matrixportal.h"

#include "app_config.h"
#include "runtime_config.h"
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

/// Small warning badge: triangle + "!" (blinks when active).
static void draw_disruption_icon(MatrixPanel_I2S_DMA* d, int16_t x, int16_t y)
{
  const bool blink_on = ((millis() / 400u) & 1) == 0;
  const uint16_t orange = d->color565(255, 140, 0);
  const uint16_t red = d->color565(255, 0, 0);
  const uint16_t white = d->color565(255, 255, 255);
  const uint16_t fill = blink_on ? orange : red;

  d->drawPixel(x + 2, y, fill);
  d->drawPixel(x + 1, y + 1, fill);
  d->drawPixel(x + 2, y + 1, fill);
  d->drawPixel(x + 3, y + 1, fill);
  d->drawPixel(x, y + 2, fill);
  d->drawPixel(x + 1, y + 2, fill);
  d->drawPixel(x + 2, y + 2, fill);
  d->drawPixel(x + 3, y + 2, fill);
  d->drawPixel(x + 4, y + 2, fill);
  d->drawPixel(x + 2, y + 3, white);
  d->drawPixel(x + 2, y + 4, white);
}

/// Horloge (zone gauche). Appelée aussi lors du clignotement QUAI/PCH pour garder les deux buffers DMA synchrones.
static void draw_time_area(MatrixPanel_I2S_DMA* d,
                           const UiModel& model,
                           int16_t content_h,
                           int16_t S,
                           int16_t y_shift,
                           uint16_t black,
                           uint16_t time_yellow_light)
{
  const int16_t tx0 = (int16_t)ZONE_TIME_X;
  const int16_t tw = (int16_t)ZONE_TIME_W;
  if (content_h <= 8) {
    return;
  }

  d->fillRect(tx0, 0, tw, content_h, black);
  d->setTextWrap(false);
  d->setTextSize(3);
  d->setTextColor(time_yellow_light, black);

  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t twd = 0;
  uint16_t tht = 0;
  d->getTextBounds(model.time_text, 0, 0, &x1, &y1, &twd, &tht);
  (void)x1;
  (void)y1;
  const int16_t tcx = (int16_t)(tx0 + tw - (int16_t)twd - 2);
  const int16_t time_ty_nom = (int16_t)((content_h - (int16_t)tht) / 2) + y_shift;
  int16_t time_ty = (int16_t)(time_ty_nom - S);
  if (time_ty < 0) {
    time_ty = 0;
  }

  d->setCursor(tcx, time_ty);
  d->print(model.time_text);
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
#if defined(BOARD_HAS_PSRAM)
  mxconfig.double_buff = true;
#endif

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
  const uint16_t stripe_h = (uint16_t)(total_h / 6); // 6 bandes pour le drapeau LGBT

  // Couleurs du drapeau LGBT (arc-en-ciel)
  const uint16_t red = d->color565(255, 0, 0);       // Rouge
  const uint16_t orange = d->color565(255, 165, 0);  // Orange
  const uint16_t yellow = d->color565(255, 255, 0);  // Jaune
  const uint16_t green = d->color565(0, 255, 0);     // Vert
  const uint16_t blue = d->color565(0, 0, 255);      // Bleu
  const uint16_t purple = d->color565(128, 0, 128);  // Violet
  const uint16_t white = d->color565(200, 200, 100); // Jaune pâle au lieu du blanc
  const uint16_t black = d->color565(0, 0, 0);

  // Dessiner les 6 bandes horizontales du drapeau LGBT
  d->fillRect(0, 0, total_w, stripe_h, red);
  d->fillRect(0, stripe_h, total_w, stripe_h, orange);
  d->fillRect(0, stripe_h * 2, total_w, stripe_h, yellow);
  d->fillRect(0, stripe_h * 3, total_w, stripe_h, green);
  d->fillRect(0, stripe_h * 4, total_w, stripe_h, blue);
  d->fillRect(0, stripe_h * 5, total_w, total_h - stripe_h * 5, purple);

  const char* text = cfg_boot_splash_text();
  if (text != nullptr && text[0] != '\0') {
    d->setTextWrap(false);
    d->setTextSize(2);
    char splash_buf[16];
    size_t n = 0;
    while (n < sizeof(splash_buf) - 1 && text[n] != '\0' && n < 12) {
      splash_buf[n] = text[n];
      n++;
    }
    splash_buf[n] = '\0';
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t tw = 0;
    uint16_t th = 0;
    d->getTextBounds(splash_buf, 0, 0, &x1, &y1, &tw, &th);
    const int16_t tx = (int16_t)((total_w - (int16_t)tw) / 2);
    const int16_t ty = (int16_t)((total_h - (int16_t)th) / 2);

    d->setTextColor(black, 0);
    d->setCursor(tx + 1, ty + 1);
    print_limited(d, splash_buf, 12);
    d->setTextColor(white, 0);
    d->setCursor(tx, ty);
    print_limited(d, splash_buf, 12);
  }

  flip_dma_buffer();
  delay(hold_ms);
  d->clearScreen();
  flip_dma_buffer();
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
  d->setTextColor(d->color565(200, 200, 100), d->color565(0, 0, 80));
  d->print("WORLD");
}

void Hub75MatrixPortal::draw_setup_screen(const char* line1, const char* line2, const char* line3)
{
  if (panel_ == nullptr) {
    return;
  }
  MatrixPanel_I2S_DMA* d = panel_ptr(panel_);
  const uint16_t black = d->color565(0, 0, 0);
  const uint16_t title_color = d->color565(255, 165, 0);
  const uint16_t hint_color = d->color565(100, 200, 255);

  d->fillScreen(black);
  d->setTextWrap(false);
  d->setTextSize(1);

  const int16_t w = (int16_t)DISPLAY_TOTAL_WIDTH;
  const int16_t h = (int16_t)DISPLAY_TOTAL_HEIGHT;

  auto draw_centered = [&](const char* text, int16_t y, uint16_t color) {
    if (text == nullptr || text[0] == '\0') {
      return;
    }
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t tw = 0;
    uint16_t th = 0;
    d->setTextColor(color, black);
    d->getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
    (void)x1;
    (void)y1;
    const int16_t x = (int16_t)((w - (int16_t)tw) / 2);
    d->setCursor(x, y);
    print_limited(d, text, 40);
  };

  draw_centered(line1 != nullptr ? line1 : "SETUP", 4, title_color);
  draw_centered(line2, 14, hint_color);
  draw_centered(line3, 24, hint_color);
  (void)h;
}

void Hub75MatrixPortal::draw_ui(const UiModel& model)
{
  if (panel_ == nullptr) {
    return;
  }

  MatrixPanel_I2S_DMA* d = panel_ptr(panel_);
  const uint16_t black = d->color565(0, 0, 0);
  // Couleurs optimisées pour girouette de bus
  const uint16_t time_yellow_light =
      d->color565(cfg_color_time_r(), cfg_color_time_g(), cfg_color_time_b());
  const uint16_t bus_line_yellow = d->color565(cfg_color_bus_line_r(),
                                               cfg_color_bus_line_g(),
                                               cfg_color_bus_line_b());
  const uint16_t separator_blue = d->color565(0, 100, 255);
  const uint16_t notify_bg_green = d->color565(0, 150, 0);
  const uint16_t eta_cyan_light = d->color565(100, 200, 255);
  const uint16_t eta_green = d->color565(cfg_color_eta_green_r(),
                                         cfg_color_eta_green_g(),
                                         cfg_color_eta_green_b());
  const uint16_t eta_orange = d->color565(cfg_color_eta_orange_r(),
                                          cfg_color_eta_orange_g(),
                                          cfg_color_eta_orange_b());
  const uint16_t eta_red =
      d->color565(cfg_color_eta_red_r(), cfg_color_eta_red_g(), cfg_color_eta_red_b());
  const uint16_t eta_no_info = d->color565(cfg_color_eta_noinfo_r(),
                                           cfg_color_eta_noinfo_g(),
                                           cfg_color_eta_noinfo_b());
  const uint8_t th_green = cfg_eta_threshold_green();
  const uint8_t th_orange = cfg_eta_threshold_orange();
  const uint16_t loading_cyan = d->color565(0, 255, 255);        // Cyan pour chargement
  const uint16_t error_yellow = d->color565(255, 255, 0);        // Jaune pour erreurs

  const int16_t full_h = (int16_t)DISPLAY_TOTAL_HEIGHT;
  const int16_t S = (int16_t)model.notify_shrink_px;
  const int16_t content_h = full_h - S;
  const int16_t y_shift =
      (int16_t)((full_h * (int32_t)HUD_VERTICAL_OFFSET_PERCENT + 50) / 100);

  const int16_t split_nom = (full_h > 20) ? 14 : (int16_t)(full_h * 3 / 5);
  const int16_t bus_line_nom = 4;
  const int16_t eta_y_nom = split_nom + 3;

  const int16_t bx0 = (int16_t)ZONE_BUS_X;
  const int16_t bw = (int16_t)ZONE_BUS_W;

  d->setTextWrap(false);

  d->fillScreen(black);
  draw_time_area(d, model, content_h, S, y_shift, black, time_yellow_light);

  const int16_t vline_y = y_shift > 0 ? y_shift - 1 : 0;
  const int16_t vline_h = (content_h - vline_y) > 0 ? (content_h - vline_y) : 1;
  d->drawFastVLine(bx0 - 2,
                   vline_y,
                   vline_h,
                   separator_blue);
  d->drawFastVLine(bx0 - 1,
                   vline_y,
                   vline_h,
                   separator_blue);

  if (content_h > 6) {
    d->setTextSize(1);
    d->setTextColor(bus_line_yellow, black); // Ambre girouette pour la ligne de bus
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
      d->drawFastHLine(bx0, split_draw - 1, bw, separator_blue);
      d->drawFastHLine(bx0, split_draw, bw, separator_blue);
    }

    uint16_t eta_color = eta_cyan_light;
    if (model.bus_state == BusState::Loading) {
      eta_color = loading_cyan;
    } else if (model.bus_state == BusState::Error) {
      eta_color = error_yellow;
    } else if (model.bus_state == BusState::Ready && model.bus_eta_minutes >= 0) {
      const int m = model.bus_eta_minutes;
      if (m > th_green) {
        eta_color = eta_green;
      } else if (m >= th_orange) {
        eta_color = eta_orange;
      } else {
        eta_color = eta_red;
      }
    } else if (model.bus_state == BusState::Ready) {
      if (std::strcmp(model.bus_text, "--") == 0) {
        eta_color = eta_no_info;
      } else {
        eta_color = eta_cyan_light;
      }
    }

    const bool no_info_text = std::strcmp(model.bus_text, "--") == 0;
    const bool special_eta_text =
        cfg_bus_eta_should_blink(model.bus_text, model.bus_eta_minutes);
    const uint32_t blink_ms = (uint32_t)cfg_bus_eta_blink_ms();
    const bool blink_on =
        special_eta_text && blink_ms > 0 && (((millis() / blink_ms) & 1) == 0);
    const uint16_t blink_on_color = d->color565(cfg_color_blink_on_r(),
                                                cfg_color_blink_on_g(),
                                                cfg_color_blink_on_b());
    const uint16_t blink_off_color = d->color565(cfg_color_blink_off_r(),
                                                 cfg_color_blink_off_g(),
                                                 cfg_color_blink_off_b());
    d->setTextSize(1);
    int16_t ex1 = 0;
    int16_t ey1 = 0;
    uint16_t ew = 0;
    uint16_t eh = 0;
    d->getTextBounds(model.bus_text, 0, 0, &ex1, &ey1, &ew, &eh);
    (void)ex1;
    (void)ey1;
    const int16_t ecx = (int16_t)(bx0 + (bw - (int16_t)ew) / 2);
    const int16_t offset_x = no_info_text ? 1 : 0;
    int16_t eta_y = eta_y_nom - S + y_shift + 2;
    if (eta_y + (int16_t)eh > content_h) {
      eta_y = content_h - (int16_t)eh;
    }
    if (eta_y < -2) {
      eta_y = -2;
    }
    if (!no_info_text && eta_y + (int16_t)eh <= content_h) {
      const uint16_t fg =
          special_eta_text ? (blink_on ? blink_on_color : blink_off_color) : eta_color;
      d->setTextColor(fg, black);
      d->setCursor(ecx + offset_x, eta_y);
      print_limited(d, model.bus_text, 5);
    } else if (no_info_text && eta_y + (int16_t)eh <= content_h) {
      d->setTextColor(eta_no_info, black);
      d->setCursor(ecx + offset_x, eta_y);
      print_limited(d, model.bus_text, 5);
    }

    if (model.bus_disrupted) {
      constexpr int16_t kIconW = 5;
      constexpr int16_t kIconH = 5;
      int16_t icon_y = eta_y + (int16_t)((eh > kIconH) ? (eh - kIconH) / 2 : 0);
      int16_t icon_x = (int16_t)(ecx - kIconW - 1);
      if (icon_x < bx0) {
        icon_x = (int16_t)(ecx + (int16_t)ew + 2);
        if (icon_x + kIconW > bx0 + bw) {
          icon_x = (int16_t)(bx0 + bw - kIconW);
        }
      }
      if (icon_y >= -2 && icon_y + kIconH <= content_h && icon_x >= bx0) {
        draw_disruption_icon(d, icon_x, icon_y);
      }
    }
  }

  if (S > 0) {
    d->fillRect(0, content_h, (int16_t)DISPLAY_TOTAL_WIDTH, S, notify_bg_green);
    if (model.notify_scroll_visible && model.notification[0] != '\0') {
      char folded[NOTIFICATION_TEXT_MAX];
      text_utf8_fold_latin(model.notification, folded, sizeof(folded));
      d->setTextSize(1);
      d->setTextColor(d->color565(255, 255, 255), notify_bg_green);
      d->setCursor(model.notify_scroll_x, content_h);
      print_limited(d, folded, 255);
    }
  }
  const uint16_t indicator_color = model.bus_theoretical
      ? d->color565(180, 0, 200)
      : d->color565(0, 200, 80);
  d->fillRect((int16_t)DISPLAY_TOTAL_WIDTH - 2, full_h - 2, 2, 2, indicator_color);
  flip_dma_buffer();
}

void Hub75MatrixPortal::refresh_bus_eta_blink(const UiModel& model)
{
  if (panel_ == nullptr) {
    return;
  }

  MatrixPanel_I2S_DMA* d = panel_ptr(panel_);
  const uint16_t black = d->color565(0, 0, 0);
  const uint16_t bus_line_yellow = d->color565(cfg_color_bus_line_r(),
                                               cfg_color_bus_line_g(),
                                               cfg_color_bus_line_b());
  const uint16_t separator_blue = d->color565(0, 100, 255);
  const uint16_t eta_cyan_light = d->color565(100, 200, 255);
  const uint16_t eta_green = d->color565(cfg_color_eta_green_r(),
                                         cfg_color_eta_green_g(),
                                         cfg_color_eta_green_b());
  const uint16_t eta_orange = d->color565(cfg_color_eta_orange_r(),
                                          cfg_color_eta_orange_g(),
                                          cfg_color_eta_orange_b());
  const uint16_t eta_red =
      d->color565(cfg_color_eta_red_r(), cfg_color_eta_red_g(), cfg_color_eta_red_b());
  const uint16_t eta_no_info = d->color565(cfg_color_eta_noinfo_r(),
                                           cfg_color_eta_noinfo_g(),
                                           cfg_color_eta_noinfo_b());
  const uint8_t th_green = cfg_eta_threshold_green();
  const uint8_t th_orange = cfg_eta_threshold_orange();
  const uint16_t loading_cyan = d->color565(0, 255, 255);
  const uint16_t error_yellow = d->color565(255, 255, 0);

  const int16_t full_h = (int16_t)DISPLAY_TOTAL_HEIGHT;
  const int16_t S = (int16_t)model.notify_shrink_px;
  const int16_t content_h = full_h - S;
  const int16_t y_shift =
      (int16_t)((full_h * (int32_t)HUD_VERTICAL_OFFSET_PERCENT + 50) / 100);
  const int16_t split_nom = (full_h > 20) ? 14 : (int16_t)(full_h * 3 / 5);
  const int16_t bus_line_nom = 4;
  const int16_t eta_y_nom = split_nom + 3;
  const int16_t bx0 = (int16_t)ZONE_BUS_X;
  const int16_t bw = (int16_t)ZONE_BUS_W;
  const int16_t clear_x = bx0;
  const int16_t clear_w = (int16_t)DISPLAY_TOTAL_WIDTH - clear_x;
  const uint16_t time_yellow_light =
      d->color565(cfg_color_time_r(), cfg_color_time_g(), cfg_color_time_b());

  if (content_h <= 6) {
    return;
  }

  draw_time_area(d, model, content_h, S, y_shift, black, time_yellow_light);
  d->fillRect(clear_x, 0, clear_w, content_h, black);
  d->setTextWrap(false);

  const int16_t vline_y = y_shift > 0 ? y_shift - 1 : 0;
  const int16_t vline_h = (content_h - vline_y) > 0 ? (content_h - vline_y) : 1;
  d->drawFastVLine(bx0 - 2, vline_y, vline_h, separator_blue);
  d->drawFastVLine(bx0 - 1, vline_y, vline_h, separator_blue);

  d->setTextSize(1);
  d->setTextColor(bus_line_yellow, black);
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
    d->drawFastHLine(bx0, split_draw - 1, bw, separator_blue);
    d->drawFastHLine(bx0, split_draw, bw, separator_blue);
  }

  uint16_t eta_color = eta_cyan_light;
  if (model.bus_state == BusState::Loading) {
    eta_color = loading_cyan;
  } else if (model.bus_state == BusState::Error) {
    eta_color = error_yellow;
  } else if (model.bus_state == BusState::Ready && model.bus_eta_minutes >= 0) {
    const int m = model.bus_eta_minutes;
    if (m > th_green) {
      eta_color = eta_green;
    } else if (m >= th_orange) {
      eta_color = eta_orange;
    } else {
      eta_color = eta_red;
    }
  } else if (model.bus_state == BusState::Ready) {
    if (std::strcmp(model.bus_text, "--") == 0) {
      eta_color = eta_no_info;
    } else {
      eta_color = eta_cyan_light;
    }
  }

  const bool no_info_text = std::strcmp(model.bus_text, "--") == 0;
  const bool special_eta_text =
      cfg_bus_eta_should_blink(model.bus_text, model.bus_eta_minutes);
  const uint32_t blink_ms = (uint32_t)cfg_bus_eta_blink_ms();
  const bool blink_on =
      special_eta_text && blink_ms > 0 && (((millis() / blink_ms) & 1) == 0);
  const uint16_t blink_on_color = d->color565(cfg_color_blink_on_r(),
                                              cfg_color_blink_on_g(),
                                              cfg_color_blink_on_b());
  const uint16_t blink_off_color = d->color565(cfg_color_blink_off_r(),
                                               cfg_color_blink_off_g(),
                                               cfg_color_blink_off_b());

  int16_t ex1 = 0;
  int16_t ey1 = 0;
  uint16_t ew = 0;
  uint16_t eh = 0;
  d->getTextBounds(model.bus_text, 0, 0, &ex1, &ey1, &ew, &eh);
  (void)ex1;
  (void)ey1;
  const int16_t ecx = (int16_t)(bx0 + (bw - (int16_t)ew) / 2);
  const int16_t offset_x = no_info_text ? 1 : 0;
  int16_t eta_y = eta_y_nom - S + y_shift + 2;
  if (eta_y + (int16_t)eh > content_h) {
    eta_y = content_h - (int16_t)eh;
  }
  if (eta_y < -2) {
    eta_y = -2;
  }
  if (!no_info_text && eta_y + (int16_t)eh <= content_h) {
    const uint16_t fg =
        special_eta_text ? (blink_on ? blink_on_color : blink_off_color) : eta_color;
    d->setTextColor(fg, black);
    d->setCursor(ecx + offset_x, eta_y);
    print_limited(d, model.bus_text, 5);
  } else if (no_info_text && eta_y + (int16_t)eh <= content_h) {
    d->setTextColor(eta_no_info, black);
    d->setCursor(ecx + offset_x, eta_y);
    print_limited(d, model.bus_text, 5);
  }

  if (model.bus_disrupted) {
    constexpr int16_t kIconW = 5;
    constexpr int16_t kIconH = 5;
    int16_t icon_y = eta_y + (int16_t)((eh > kIconH) ? (eh - kIconH) / 2 : 0);
    int16_t icon_x = (int16_t)(ecx - kIconW - 1);
    if (icon_x < bx0) {
      icon_x = (int16_t)(ecx + (int16_t)ew + 2);
      if (icon_x + kIconW > bx0 + bw) {
        icon_x = (int16_t)(bx0 + bw - kIconW);
      }
    }
    if (icon_y >= -2 && icon_y + kIconH <= content_h && icon_x >= bx0) {
      draw_disruption_icon(d, icon_x, icon_y);
    }
  }

  const uint16_t indicator_color = model.bus_theoretical ? d->color565(180, 0, 200)
                                                         : d->color565(0, 200, 80);
  d->fillRect((int16_t)DISPLAY_TOTAL_WIDTH - 2, full_h - 2, 2, 2, indicator_color);
  flip_dma_buffer();
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

void Hub75MatrixPortal::draw_setup_screen(const char*, const char*, const char*) {}

void Hub75MatrixPortal::draw_ui(const UiModel&) {}

void Hub75MatrixPortal::refresh_bus_eta_blink(const UiModel&) {}

void Hub75MatrixPortal::clear_screen() {}

void Hub75MatrixPortal::set_brightness8(uint8_t) {}

void Hub75MatrixPortal::flip_dma_buffer() {}

#endif
