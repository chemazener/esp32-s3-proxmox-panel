// lv_conf.h minimo para LVGL 8.x. lv_conf_internal.h aplica defaults (#ifndef)
// a todo lo que no se defina aqui, asi que solo sobreescribimos lo necesario.
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// Profundidad de color: RGB565 (el panel RGB es 16-bit)
#define LV_COLOR_DEPTH 16
// El bus RGB paralelo espera los dos bytes del pixel en orden inverso al que
// produce LVGL en el ESP32-S3. Sin este swap el negro sale verde-lima, el rojo
// azul y el ambar magenta (byte-swap de cada RGB565). Con 1 los colores salen
// correctos (tema oscuro slate).
#define LV_COLOR_16_SWAP 1

// Pool estatico de LVGL en RAM interna (rapido y fiable). 200KB para las ~16
// tarjetas con sus mini-barras + panel 3080. (El asignador ps_malloc en PSRAM
// colgaba la UI; con el pool estatico y menos objetos por tarjeta va sobrado.)
#define LV_MEM_SIZE (200U * 1024U)

// Tick por millis() de Arduino
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

// Refresco
#define LV_DISP_DEF_REFR_PERIOD 20
#define LV_INDEV_DEF_READ_PERIOD 20

// Fuentes
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

// Sin logs por defecto
#define LV_USE_LOG 0

#endif // LV_CONF_H
