// LovyanGFX config para Waveshare ESP32-S3-Touch-LCD-7C.
// Solo el panel RGB paralelo 16-bit 800x480 (ST7262). El tactil (GT911) y el
// backlight/reset (expansor I2C 0x24) los gestiona main.cpp por Wire, no aqui.
// Pinout y timings extraidos del driver oficial rgb_lcd_port de Waveshare.
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
// LovyanGFX.hpp no arrastra por si solo las cabeceras RGB del ESP32-S3; hay que
// incluirlas para que existan lgfx::Bus_RGB y lgfx::Panel_RGB.
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

class LGFX_7C : public lgfx::LGFX_Device {
  lgfx::Bus_RGB   _bus_instance;
  lgfx::Panel_RGB _panel_instance;

public:
  LGFX_7C(void) {
    { // ----- Panel -----
      auto cfg = _panel_instance.config();
      cfg.memory_width  = 800;
      cfg.memory_height = 480;
      cfg.panel_width   = 800;
      cfg.panel_height  = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel_instance.config(cfg);
    }
    { // ----- Detalle del panel: framebuffers en PSRAM -----
      auto cfg = _panel_instance.config_detail();
      cfg.use_psram = 1;
      _panel_instance.config_detail(cfg);
    }
    { // ----- Bus RGB -----
      auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;

      // Datos: D0..D4 = Blue, D5..D10 = Green, D11..D15 = Red
      cfg.pin_d0  = GPIO_NUM_14; // B3
      cfg.pin_d1  = GPIO_NUM_38; // B4
      cfg.pin_d2  = GPIO_NUM_18; // B5
      cfg.pin_d3  = GPIO_NUM_17; // B6
      cfg.pin_d4  = GPIO_NUM_10; // B7
      cfg.pin_d5  = GPIO_NUM_39; // G2
      cfg.pin_d6  = GPIO_NUM_0;  // G3
      cfg.pin_d7  = GPIO_NUM_45; // G4
      cfg.pin_d8  = GPIO_NUM_9;  // G5
      cfg.pin_d9  = GPIO_NUM_8;  // G6
      cfg.pin_d10 = GPIO_NUM_21; // G7
      cfg.pin_d11 = GPIO_NUM_1;  // R3
      cfg.pin_d12 = GPIO_NUM_2;  // R4
      cfg.pin_d13 = GPIO_NUM_42; // R5
      cfg.pin_d14 = GPIO_NUM_41; // R6
      cfg.pin_d15 = GPIO_NUM_40; // R7

      cfg.pin_henable = GPIO_NUM_5;  // DE
      cfg.pin_vsync   = GPIO_NUM_3;
      cfg.pin_hsync   = GPIO_NUM_46;
      cfg.pin_pclk    = GPIO_NUM_7;
      cfg.freq_write  = 16000000;    // PCLK 16 MHz

      cfg.hsync_polarity    = 0;
      cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4;
      cfg.hsync_back_porch  = 8;
      cfg.vsync_polarity    = 0;
      cfg.vsync_front_porch = 8;
      cfg.vsync_pulse_width = 4;
      cfg.vsync_back_porch  = 8;
      cfg.pclk_active_neg   = 1;     // PCLK activo en flanco de bajada
      cfg.de_idle_high      = 0;
      cfg.pclk_idle_high    = 0;
      _bus_instance.config(cfg);
    }
    _panel_instance.setBus(&_bus_instance);
    setPanel(&_panel_instance);
  }
};
