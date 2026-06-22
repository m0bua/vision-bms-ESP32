#pragma once

#include <Arduino.h>

extern const char UI_THEME_BASE_VARS[] PROGMEM;

#if __has_include("custom_ui_theme.h")
#include "custom_ui_theme.h"
#endif

#ifndef UI_THEME_CUSTOM_VARS
extern const char UI_THEME_CUSTOM_VARS[] PROGMEM;
#endif
