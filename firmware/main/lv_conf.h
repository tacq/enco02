#ifndef LV_CONF_H
#define LV_CONF_H

#define CONFIG_LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define CONFIG_LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define CONFIG_LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_CONF_SKIP (1)
#define CONFIG_LV_TXT_ENC LV_TXT_ENC_UTF8

#define CONFIG_LV_CONF_SKIP 1
#define CONFIG_LV_COLOR_DEPTH_16 1
#define CONFIG_LV_COLOR_DEPTH 16
#define CONFIG_LV_USE_CLIB_MALLOC 1
#define CONFIG_LV_USE_CLIB_STRING 1
#define CONFIG_LV_USE_CLIB_SPRINTF 1
#define CONFIG_LV_DEF_REFR_PERIOD 33
#define CONFIG_LV_DPI_DEF 130
#define CONFIG_LV_OS_NONE 1
#define CONFIG_LV_USE_OS 0
#define CONFIG_LV_DRAW_BUF_STRIDE_ALIGN 1
#define CONFIG_LV_DRAW_BUF_ALIGN 4
#define CONFIG_LV_DRAW_LAYER_SIMPLE_BUF_SIZE 8192
#define CONFIG_LV_USE_DRAW_SW 1
#define CONFIG_LV_DRAW_SW_SUPPORT_RGB565 1
#define CONFIG_LV_DRAW_SW_SUPPORT_RGB565A8 1
#define CONFIG_LV_DRAW_SW_SUPPORT_RGB888 1
#define CONFIG_LV_DRAW_SW_SUPPORT_XRGB8888 1
#define CONFIG_LV_DRAW_SW_SUPPORT_ARGB8888 1
#define CONFIG_LV_DRAW_SW_SUPPORT_L8 1
#define CONFIG_LV_DRAW_SW_SUPPORT_AL88 1
#define CONFIG_LV_DRAW_SW_SUPPORT_A8 1
#define CONFIG_LV_DRAW_SW_SUPPORT_I1 1
#define CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT 1
#define CONFIG_LV_DRAW_SW_COMPLEX 1
#define CONFIG_LV_DRAW_SW_SHADOW_CACHE_SIZE 0
#define CONFIG_LV_DRAW_SW_CIRCLE_CACHE_SIZE 16
#define LV_DRAW_SW_CIRCLE_CACHE_SIZE 16
#define CONFIG_LV_DRAW_SW_ASM_NONE 1
#define CONFIG_LV_USE_DRAW_SW_ASM 0
#define CONFIG_LV_USE_ASSERT_NULL 1
#define CONFIG_LV_USE_ASSERT_MALLOC 1
#define CONFIG_LV_ASSERT_HANDLER_INCLUDE "assert.h"
#define CONFIG_LV_CACHE_DEF_SIZE 0
#define CONFIG_LV_IMAGE_HEADER_CACHE_DEF_CNT 0
#define CONFIG_LV_GRADIENT_MAX_STOPS 2
#define CONFIG_LV_COLOR_MIX_ROUND_OFS 128
#define CONFIG_LV_ATTRIBUTE_MEM_ALIGN_SIZE 1
#define CONFIG_LV_FONT_MONTSERRAT_14 1
#define CONFIG_LV_FONT_DEFAULT_MONTSERRAT_14 1
// Big digits for the countdown overlay. Nothing else linked here is larger than 16px, and a
// T-MINUS readout at 16px is not a countdown, it is a caption.
//
// Montserrat rather than a generated subset because it ships with LVGL and costs only a flag;
// flash is at 72%, with ~1.1MB spare, so there is no reason to build a digits-only font. It has no
// CJK, which is fine - only the digits and the colon are ever rendered in it.
#define CONFIG_LV_FONT_MONTSERRAT_32 1
#define CONFIG_LV_FONT_FMT_TXT_LARGE 1
#define CONFIG_LV_USE_FONT_COMPRESSED 1
#define CONFIG_LV_USE_FONT_PLACEHOLDER 1
#define CONFIG_LV_TXT_ENC_UTF8 1
#define CONFIG_LV_TXT_BREAK_CHARS " ,.;:-_)}"
#define CONFIG_LV_TXT_LINE_BREAK_LONG_LEN 0
#define CONFIG_LV_WIDGETS_HAS_DEFAULT_VALUE 1
#define CONFIG_LV_USE_ARC 1
#define CONFIG_LV_USE_BAR 1
#define CONFIG_LV_USE_BUTTON 1
#define CONFIG_LV_USE_BUTTONMATRIX 1
#define CONFIG_LV_USE_CANVAS 1
#define CONFIG_LV_USE_CHECKBOX 1
#define CONFIG_LV_USE_DROPDOWN 1
#define CONFIG_LV_USE_IMAGE 1
#define CONFIG_LV_USE_IMAGEBUTTON 1
#define CONFIG_LV_USE_LABEL 1
#define CONFIG_LV_LABEL_TEXT_SELECTION 1
#define CONFIG_LV_LABEL_LONG_TXT_HINT 1
#define CONFIG_LV_LABEL_WAIT_CHAR_COUNT 3
#define CONFIG_LV_USE_LINE 1
#define CONFIG_LV_USE_ROLLER 1
#define CONFIG_LV_USE_SCALE 1
#define CONFIG_LV_USE_SLIDER 1
#define CONFIG_LV_USE_SWITCH 1
#define CONFIG_LV_USE_TEXTAREA 1
#define CONFIG_LV_TEXTAREA_DEF_PWD_SHOW_TIME 1500
#define CONFIG_LV_USE_TABLE 1
#define CONFIG_LV_USE_THEME_DEFAULT 1
#define CONFIG_LV_THEME_DEFAULT_GROW 1
#define CONFIG_LV_THEME_DEFAULT_TRANSITION_TIME 80
#define CONFIG_LV_USE_THEME_SIMPLE 1
#define CONFIG_LV_USE_FLEX 1
#define CONFIG_LV_USE_GRID 1
#define CONFIG_LV_FS_DEFAULT_DRIVE_LETTER 0
// Only the colour emoji font used this, and the status bar now draws monochrome Font Awesome
// glyphs instead so it matches the rest of the HUD.
#define CONFIG_LV_USE_IMGFONT 0
#define CONFIG_LV_USE_OBSERVER 1

// TJpgDec, for the camera viewfinder.
//
// We do not use LVGL's image decoder wrapper around it - the viewfinder calls
// jd_prepare()/jd_decomp() directly so the JPEG can be pulled off the UART a
// few hundred bytes at a time and blitted straight to the panel. A frame is
// ~6KB and the largest free heap block on this board is ~14KB, so there is
// nowhere to put a whole one. This switch exists purely to get tjpgd.c
// compiled; the decoder registration it also enables is harmless.
#define CONFIG_LV_USE_TJPGD 1

#endif