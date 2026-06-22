#pragma once

#include <Arduino.h>

const char UI_THEME_BASE_VARS[] PROGMEM = R"rawliteral(
  --bg:#121212;
  --text:#f2f2f2;
  --muted:#a8a8a8;
  --ok:#4caf50;
  --warn:#ffb74d;
  --bad:#ef5350;
  --accent:#80cbc4;
  --panel:#1d1d1d;
  --panel2:#262626;
  --line:#353535;
  --cell-low-bg:#181818;
  --cell-high-bg:#242424;
  --fill-low:#6e6e6e;
  --fill-mid:#9c9c9c;
  --fill-high:#d0d0d0;
)rawliteral";

#if __has_include("custom_ui_theme.h")
#include "custom_ui_theme.h"
#endif

#ifndef UI_THEME_CUSTOM_VARS
const char UI_THEME_CUSTOM_VARS[] PROGMEM = "";
#endif
