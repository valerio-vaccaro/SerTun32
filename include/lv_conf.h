/**
 * @file lv_conf.h
 * @brief Minimal LVGL v8 configuration for the SerTun32 project.
 *
 * Only the options that matter for this project are tuned here; everything
 * else keeps the LVGL defaults. This file is picked up because the build
 * defines `LV_CONF_INCLUDE_SIMPLE` and adds ./include to the include path.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
 *   COLOR SETTINGS
 *====================*/
/* ST7789 is an RGB565 panel. */
#define LV_COLOR_DEPTH 16
/* Do NOT let LVGL pre-swap the colour bytes: the flush callback already asks
 * TFT_eSPI to byte-swap (pushColors(..., true)). On the T-Display-S3's 8-bit
 * parallel bus, having both swap produces wrong colours (red->green, etc.), so
 * LVGL must hand over native byte order and let TFT_eSPI do the single swap. */
#define LV_COLOR_16_SWAP 0

/*=========================
 *   MEMORY SETTINGS
 *=========================*/
/* Static memory pool used by LVGL's allocator (in bytes). 32 kB is plenty for
 * a few labels on a 135x240 panel and keeps us within the ESP32-S2 DRAM. */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (32U * 1024U)

/*====================
 *   HAL / TICK
 *====================*/
/* Let LVGL read the system tick straight from Arduino's millis() so we do not
 * have to feed lv_tick_inc() ourselves. */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/* Default DPI for a small IPS panel. */
#define LV_DPI_DEF 130

/*====================
 *   FEATURE USAGE
 *====================*/
#define LV_USE_LOG 0

/*====================
 *   FONTS
 *====================*/
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16
/* Render the built-in FontAwesome symbols (WiFi, settings, OK, warning...). */
#define LV_FONT_FMT_TXT_LARGE 0

/*====================
 *   WIDGETS (only what we use)
 *====================*/
#define LV_USE_LABEL 1
#define LV_LABEL_TEXT_SELECTION 0
#define LV_USE_ARC 1
#define LV_USE_SPINNER 1
#define LV_USE_BAR 1

/*====================
 *   THEME
 *====================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

#endif /* LV_CONF_H */
