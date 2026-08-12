#include "wled.h"
#include <WiFi.h>
#include <M5GFX.h>

class CoreS3DisplayUsermod : public Usermod
{
private:
  M5GFX display;

  bool displayReady = false;
  bool touchReady = false;

  int16_t screenWidth = 0;
  int16_t screenHeight = 0;

  unsigned long lastUpdate = 0;
  unsigned long lastTouchPoll = 0;
  unsigned long lastTouchAction = 0;
  unsigned long touchReleaseCandidate = 0;

  bool lastWiFiConnected = false;
  String lastIPAddress = "";

  bool readyScreenShown = false;
  bool connectingScreenShown = false;

  int8_t lastLedState = -1;
  int lastBrightnessValue = -1;
  int lastEffectMode = -1;
  int lastHueValue = -1;

  uint32_t lastPrimaryColor = 0;
  bool lastPrimaryColorValid = false;

  // =========================================================
  // Persistent logical HSV state
  //
  // IMPORTANT:
  //
  // Local Hue operations use this logical HSV state instead
  // of converting RGB -> HSV again for every short press.
  //
  // This avoids Hue values becoming stuck because multiple
  // adjacent HSV values may quantize to the same RGB value.
  //
  // External Web UI color changes re-synchronize this state.
  // =========================================================

  CHSV32 logicalColorHsv;

  bool logicalColorHsvValid = false;

  uint8_t logicalHueValue = 0;
  uint8_t logicalWhiteValue = 0;

  // =========================================================
  // Screen pages
  // =========================================================

  enum ScreenPage : uint8_t
  {
    SCREEN_MAIN = 0,
    SCREEN_COLOR
  };

  ScreenPage currentPage =
    SCREEN_MAIN;

  // =========================================================
  // Touch targets
  // =========================================================

  enum TouchTarget : uint8_t
  {
    TOUCH_TARGET_NONE = 0,

    TOUCH_TARGET_POWER,

    TOUCH_TARGET_BRIGHTNESS_DOWN,
    TOUCH_TARGET_BRIGHTNESS_UP,

    TOUCH_TARGET_EFFECT_PREV,
    TOUCH_TARGET_EFFECT_NEXT,

    TOUCH_TARGET_COLOR_OPEN,
    TOUCH_TARGET_BACK,

    TOUCH_TARGET_HUE_DOWN,
    TOUCH_TARGET_HUE_UP
  };

  TouchTarget touchTarget =
    TOUCH_TARGET_NONE;

  // =========================================================
  // Touch state
  // =========================================================

  bool touchActive = false;

  bool lastTouchInsidePower = false;
  bool lastTouchInsideBrightness = false;
  bool lastTouchInsideEffect = false;
  bool lastTouchInsideColor = false;
  bool lastTouchInsideBack = false;
  bool lastTouchInsideHue = false;

  bool powerButtonVisualPressed = false;
  bool brightnessButtonVisualPressed = false;
  bool effectButtonVisualPressed = false;
  bool colorButtonVisualPressed = false;
  bool backButtonVisualPressed = false;
  bool hueButtonVisualPressed = false;

  bool brightnessLongPressActive = false;
  bool hueLongPressActive = false;

  int16_t lastTouchX = -1;
  int16_t lastTouchY = -1;

  unsigned long controlPressStartTime = 0;
  unsigned long lastBrightnessRepeat = 0;

  unsigned long huePressStartTime = 0;
  unsigned long lastHueRepeat = 0;

  // =========================================================
  // Hue gesture edit state
  //
  // A gesture starts from the persistent logical HSV state.
  // =========================================================

  CHSV32 hueEditHsv;

  bool hueEditValid = false;

  uint8_t hueEditValue = 0;
  uint8_t hueEditWhite = 0;

  // =========================================================
  // Power button
  // =========================================================

  static constexpr int16_t POWER_BUTTON_X = 8;
  static constexpr int16_t POWER_BUTTON_Y = 8;
  static constexpr int16_t POWER_BUTTON_W = 44;
  static constexpr int16_t POWER_BUTTON_H = 44;

  // =========================================================
  // MAIN header
  // =========================================================

  static constexpr int16_t HEADER_CONTENT_LEFT = 60;
  static constexpr int16_t HEADER_CONTENT_RIGHT = 312;

  static constexpr int16_t HEADER_CENTER_X =
    (
      HEADER_CONTENT_LEFT +
      HEADER_CONTENT_RIGHT
    ) / 2;

  static constexpr int16_t HEADER_TITLE_Y = 18;
  static constexpr int16_t HEADER_IP_Y = 41;

  // =========================================================
  // Common LEFT / RIGHT buttons
  //
  // Used by:
  //   Brightness
  //   Effect
  //   Hue
  // =========================================================

  static constexpr int16_t CONTROL_LEFT_X = 16;
  static constexpr int16_t CONTROL_RIGHT_X = 240;

  static constexpr int16_t CONTROL_BUTTON_W = 64;
  static constexpr int16_t CONTROL_BUTTON_H = 34;

  // =========================================================
  // Brightness row
  // =========================================================

  static constexpr int16_t BRI_BUTTON_Y = 82;

  // =========================================================
  // Effect row
  // =========================================================

  static constexpr int16_t FX_BUTTON_Y = 138;

  // =========================================================
  // MAIN Color button
  // =========================================================

  static constexpr int16_t COLOR_BUTTON_X = 64;
  static constexpr int16_t COLOR_BUTTON_Y = 188;
  static constexpr int16_t COLOR_BUTTON_W = 192;
  static constexpr int16_t COLOR_BUTTON_H = 40;

  // =========================================================
  // COLOR Back button
  // =========================================================

  static constexpr int16_t BACK_BUTTON_X = 268;
  static constexpr int16_t BACK_BUTTON_Y = 8;
  static constexpr int16_t BACK_BUTTON_W = 44;
  static constexpr int16_t BACK_BUTTON_H = 44;

  // =========================================================
  // COLOR preview
  // =========================================================

  static constexpr int16_t COLOR_PREVIEW_X = 92;
  static constexpr int16_t COLOR_PREVIEW_Y = 68;
  static constexpr int16_t COLOR_PREVIEW_W = 136;
  static constexpr int16_t COLOR_PREVIEW_H = 36;

  // =========================================================
  // Hue row
  // =========================================================

  static constexpr int16_t HUE_BUTTON_Y = 151;

  // =========================================================
  // Touch timing
  // =========================================================

  static constexpr unsigned long TOUCH_POLL_MS = 15;

  static constexpr unsigned long TOUCH_RELEASE_CONFIRM_MS = 70;

  static constexpr unsigned long TOUCH_ACTION_COOLDOWN_MS = 250;

  // =========================================================
  // Brightness behavior
  // =========================================================

  static constexpr unsigned long BRI_LONG_PRESS_MS = 400;
  static constexpr unsigned long BRI_REPEAT_MS = 80;

  static constexpr int BRI_SHORT_STEP = 1;
  static constexpr int BRI_LONG_STEP = 5;

  // =========================================================
  // Hue behavior
  //
  // Short:
  //   +/- 1
  //
  // Long:
  //   after 400 ms
  //   +/- 5 every 80 ms
  //
  // Wrap:
  //   255 + 1 -> 0
  //   0 - 1   -> 255
  // =========================================================

  static constexpr unsigned long HUE_LONG_PRESS_MS = 400;
  static constexpr unsigned long HUE_REPEAT_MS = 80;

  static constexpr int HUE_SHORT_STEP = 1;
  static constexpr int HUE_LONG_STEP = 5;

  // =========================================================
  // RGB888 -> RGB565
  // =========================================================

  uint16_t rgbTo565(
    uint8_t r,
    uint8_t g,
    uint8_t b
  )
  {
    return
      (
        ((uint16_t)(r & 0xF8) << 8) |
        ((uint16_t)(g & 0xFC) << 3) |
        ((uint16_t)b >> 3)
      );
  }

  // =========================================================
  // Primary Color
  // =========================================================

  uint32_t getPrimaryColor()
  {
    if (
      strip.getSegmentsNum() >
      0
    )
    {
      return
        strip.getMainSegment().colors[0];
    }

    return 0;
  }

  // =========================================================
  // Current Effect
  // =========================================================

  uint8_t getCurrentEffectMode()
  {
    if (
      strip.getSegmentsNum() >
      0
    )
    {
      return
        strip.getMainSegment().mode;
    }

    return 0;
  }

  // =========================================================
  // Raw RGB -> Hue
  //
  // Used only when synchronizing from an external RGB color.
  // =========================================================

  uint8_t getHueFromColor(
    uint32_t color
  )
  {
    CRGBW rgb(
      color
    );

    CHSV32 hsv;

    rgb2hsv(
      rgb,
      hsv
    );

    return
      (uint8_t)(
        hsv.h >>
        8
      );
  }

  // =========================================================
  // Synchronize persistent logical HSV from RGB
  //
  // This is intentionally NOT called for every local Hue
  // button press.
  //
  // Call it when:
  //
  //   - Initial screen is created
  //   - Color screen is opened
  //   - External Web UI changes Primary Color
  // =========================================================

  void syncLogicalColorFromRgb(
    uint32_t color
  )
  {
    CRGBW rgb(
      color
    );

    rgb2hsv(
      rgb,
      logicalColorHsv
    );

    logicalHueValue =
      (uint8_t)(
        logicalColorHsv.h >>
        8
      );

    logicalWhiteValue =
      rgb.w;

    logicalColorHsvValid =
      true;

    lastHueValue =
      logicalHueValue;

    Serial.printf(
      "[CoreS3_Display] "
      "HSV sync from RGB: "
      "H=%u S=%u V=%u W=%u\n",
      logicalHueValue,
      logicalColorHsv.s,
      logicalColorHsv.v,
      logicalWhiteValue
    );
  }

  // =========================================================
  // Hue displayed on LCD
  //
  // Prefer the logical Hue when it is synchronized with the
  // currently cached Primary Color.
  // =========================================================

  uint8_t getDisplayedHue()
  {
    uint32_t currentColor =
      getPrimaryColor();

    if (
      logicalColorHsvValid &&
      lastPrimaryColorValid &&
      currentColor ==
        lastPrimaryColor
    )
    {
      return
        logicalHueValue;
    }

    return
      getHueFromColor(
        currentColor
      );
  }

  // =========================================================
  // Boot screen
  // =========================================================

  void drawBootScreen()
  {
    display.fillScreen(
      TFT_BLACK
    );

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(
      2
    );

    display.drawString(
      "WLED M5Stack CoreS3",
      screenWidth / 2,
      82
    );

    display.setTextSize(
      2
    );

    display.drawString(
      "Starting...",
      screenWidth / 2,
      145
    );
  }

  // =========================================================
  // Wi-Fi connecting
  // =========================================================

  void drawConnectingScreen()
  {
    display.fillScreen(
      TFT_BLACK
    );

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(
      2
    );

    display.drawString(
      "WLED M5Stack CoreS3",
      screenWidth / 2,
      65
    );

    display.setTextSize(
      2
    );

    display.drawString(
      "Wi-Fi",
      screenWidth / 2,
      125
    );

    display.drawString(
      "Connecting...",
      screenWidth / 2,
      160
    );

    currentPage =
      SCREEN_MAIN;

    connectingScreenShown =
      true;

    readyScreenShown =
      false;

    resetTouchGesture();
  }

  // =========================================================
  // Power icon
  // =========================================================

  void drawPowerIcon(
    int16_t centerX,
    int16_t centerY,
    uint16_t iconColor,
    uint16_t backgroundColor
  )
  {
    display.drawCircle(
      centerX,
      centerY + 2,
      11,
      iconColor
    );

    display.drawCircle(
      centerX,
      centerY + 2,
      10,
      iconColor
    );

    display.fillRect(
      centerX - 4,
      centerY - 11,
      9,
      8,
      backgroundColor
    );

    display.drawFastVLine(
      centerX - 1,
      centerY - 14,
      12,
      iconColor
    );

    display.drawFastVLine(
      centerX,
      centerY - 14,
      12,
      iconColor
    );

    display.drawFastVLine(
      centerX + 1,
      centerY - 14,
      12,
      iconColor
    );
  }

  // =========================================================
  // Power button
  // =========================================================

  void drawPowerButton(
    bool ledOn,
    bool pressed
  )
  {
    uint16_t stateColor =
      ledOn
        ? TFT_GREEN
        : TFT_RED;

    uint16_t backgroundColor =
      pressed
        ? stateColor
        : TFT_BLACK;

    uint16_t iconColor =
      pressed
        ? TFT_BLACK
        : stateColor;

    display.fillRect(
      POWER_BUTTON_X - 2,
      POWER_BUTTON_Y - 2,
      POWER_BUTTON_W + 4,
      POWER_BUTTON_H + 4,
      TFT_BLACK
    );

    display.fillRect(
      POWER_BUTTON_X,
      POWER_BUTTON_Y,
      POWER_BUTTON_W,
      POWER_BUTTON_H,
      backgroundColor
    );

    display.drawRect(
      POWER_BUTTON_X,
      POWER_BUTTON_Y,
      POWER_BUTTON_W,
      POWER_BUTTON_H,
      stateColor
    );

    display.drawRect(
      POWER_BUTTON_X + 1,
      POWER_BUTTON_Y + 1,
      POWER_BUTTON_W - 2,
      POWER_BUTTON_H - 2,
      stateColor
    );

    int16_t centerX =
      POWER_BUTTON_X +
      (POWER_BUTTON_W / 2);

    int16_t centerY =
      POWER_BUTTON_Y +
      (POWER_BUTTON_H / 2);

    drawPowerIcon(
      centerX,
      centerY,
      iconColor,
      backgroundColor
    );

    powerButtonVisualPressed =
      pressed;
  }

  // =========================================================
  // Generic triangle button
  // =========================================================

  void drawTriangleButton(
    int16_t x,
    int16_t y,
    bool pointRight,
    bool pressed
  )
  {
    const uint16_t buttonColor =
      TFT_CYAN;

    const int16_t w =
      CONTROL_BUTTON_W;

    const int16_t h =
      CONTROL_BUTTON_H;

    display.fillRect(
      x - 2,
      y - 2,
      w + 4,
      h + 4,
      TFT_BLACK
    );

    if (pressed)
    {
      display.fillRect(
        x,
        y,
        w,
        h,
        buttonColor
      );
    }
    else
    {
      display.fillRect(
        x,
        y,
        w,
        h,
        TFT_BLACK
      );

      display.drawRect(
        x,
        y,
        w,
        h,
        buttonColor
      );

      display.drawRect(
        x + 1,
        y + 1,
        w - 2,
        h - 2,
        buttonColor
      );
    }

    int16_t centerX =
      x + (w / 2);

    int16_t centerY =
      y + (h / 2);

    uint16_t triangleColor =
      pressed
        ? TFT_BLACK
        : buttonColor;

    if (pointRight)
    {
      display.fillTriangle(
        centerX + 10,
        centerY,

        centerX - 7,
        centerY - 9,

        centerX - 7,
        centerY + 9,

        triangleColor
      );
    }
    else
    {
      display.fillTriangle(
        centerX - 10,
        centerY,

        centerX + 7,
        centerY - 9,

        centerX + 7,
        centerY + 9,

        triangleColor
      );
    }
  }

  // =========================================================
  // Brightness
  // =========================================================

  void drawBrightness(
    int brightnessValue,
    TouchTarget pressedTarget =
      TOUCH_TARGET_NONE
  )
  {
    display.fillRect(
      0,
      62,
      screenWidth,
      58,
      TFT_BLACK
    );

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(
      1
    );

    display.drawString(
      "Brightness",
      screenWidth / 2,
      70
    );

    drawTriangleButton(
      CONTROL_LEFT_X,
      BRI_BUTTON_Y,
      false,
      pressedTarget ==
        TOUCH_TARGET_BRIGHTNESS_DOWN
    );

    drawTriangleButton(
      CONTROL_RIGHT_X,
      BRI_BUTTON_Y,
      true,
      pressedTarget ==
        TOUCH_TARGET_BRIGHTNESS_UP
    );

    char valueText[8];

    snprintf(
      valueText,
      sizeof(valueText),
      "%d",
      brightnessValue
    );

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(
      2
    );

    display.drawString(
      valueText,
      screenWidth / 2,
      99
    );
  }

  // =========================================================
  // Effect name
  // =========================================================

  void getEffectName(
    uint8_t effectMode,
    char* effectName,
    size_t effectNameSize
  )
  {
    if (
      effectName == nullptr ||
      effectNameSize == 0
    )
    {
      return;
    }

    effectName[0] =
      '\0';

    extractModeName(
      effectMode,
      nullptr,
      effectName,
      effectNameSize - 1
    );

    if (
      strlen(effectName) ==
      0
    )
    {
      strncpy(
        effectName,
        "Unknown",
        effectNameSize - 1
      );

      effectName[
        effectNameSize - 1
      ] =
        '\0';
    }

    if (
      strlen(effectName) >
      22
    )
    {
      effectName[22] =
        '\0';
    }
  }

  // =========================================================
  // Effect
  // =========================================================

  void drawEffect(
    uint8_t effectMode,
    TouchTarget pressedTarget =
      TOUCH_TARGET_NONE
  )
  {
    display.fillRect(
      0,
      120,
      screenWidth,
      58,
      TFT_BLACK
    );

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(
      1
    );

    display.drawString(
      "Effect",
      screenWidth / 2,
      128
    );

    drawTriangleButton(
      CONTROL_LEFT_X,
      FX_BUTTON_Y,
      false,
      pressedTarget ==
        TOUCH_TARGET_EFFECT_PREV
    );

    drawTriangleButton(
      CONTROL_RIGHT_X,
      FX_BUTTON_Y,
      true,
      pressedTarget ==
        TOUCH_TARGET_EFFECT_NEXT
    );

    char effectName[64];

    getEffectName(
      effectMode,
      effectName,
      sizeof(effectName)
    );

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    if (
      strlen(effectName) <=
      12
    )
    {
      display.setTextSize(
        2
      );
    }
    else
    {
      display.setTextSize(
        1
      );
    }

    display.drawString(
      effectName,
      screenWidth / 2,
      155
    );
  }

  // =========================================================
  // MAIN Color button
  // =========================================================

  void drawColorButton(
    uint32_t color,
    bool pressed
  )
  {
    const uint16_t buttonColor =
      TFT_CYAN;

    uint16_t backgroundColor =
      pressed
        ? buttonColor
        : TFT_BLACK;

    uint16_t textColor =
      pressed
        ? TFT_BLACK
        : TFT_WHITE;

    uint8_t r =
      R(color);

    uint8_t g =
      G(color);

    uint8_t b =
      B(color);

    uint16_t previewColor =
      rgbTo565(
        r,
        g,
        b
      );

    display.fillRect(
      COLOR_BUTTON_X - 2,
      COLOR_BUTTON_Y - 2,
      COLOR_BUTTON_W + 4,
      COLOR_BUTTON_H + 4,
      TFT_BLACK
    );

    display.fillRect(
      COLOR_BUTTON_X,
      COLOR_BUTTON_Y,
      COLOR_BUTTON_W,
      COLOR_BUTTON_H,
      backgroundColor
    );

    display.drawRect(
      COLOR_BUTTON_X,
      COLOR_BUTTON_Y,
      COLOR_BUTTON_W,
      COLOR_BUTTON_H,
      buttonColor
    );

    display.drawRect(
      COLOR_BUTTON_X + 1,
      COLOR_BUTTON_Y + 1,
      COLOR_BUTTON_W - 2,
      COLOR_BUTTON_H - 2,
      buttonColor
    );

    static constexpr int16_t SWATCH_W = 26;
    static constexpr int16_t SWATCH_H = 24;

    int16_t swatchX =
      COLOR_BUTTON_X + 14;

    int16_t swatchY =
      COLOR_BUTTON_Y +
      (
        (
          COLOR_BUTTON_H -
          SWATCH_H
        ) / 2
      );

    display.fillRect(
      swatchX,
      swatchY,
      SWATCH_W,
      SWATCH_H,
      previewColor
    );

    display.drawRect(
      swatchX,
      swatchY,
      SWATCH_W,
      SWATCH_H,
      TFT_WHITE
    );

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      textColor,
      backgroundColor
    );

    display.setTextSize(
      2
    );

    display.drawString(
      "COLOR",
      COLOR_BUTTON_X + 115,
      COLOR_BUTTON_Y +
        (COLOR_BUTTON_H / 2)
    );

    colorButtonVisualPressed =
      pressed;
  }

  // =========================================================
  // Back button
  // =========================================================

  void drawBackButton(
    bool pressed
  )
  {
    const uint16_t buttonColor =
      TFT_CYAN;

    uint16_t backgroundColor =
      pressed
        ? buttonColor
        : TFT_BLACK;

    uint16_t iconColor =
      pressed
        ? TFT_BLACK
        : buttonColor;

    display.fillRect(
      BACK_BUTTON_X - 2,
      BACK_BUTTON_Y - 2,
      BACK_BUTTON_W + 4,
      BACK_BUTTON_H + 4,
      TFT_BLACK
    );

    display.fillRect(
      BACK_BUTTON_X,
      BACK_BUTTON_Y,
      BACK_BUTTON_W,
      BACK_BUTTON_H,
      backgroundColor
    );

    display.drawRect(
      BACK_BUTTON_X,
      BACK_BUTTON_Y,
      BACK_BUTTON_W,
      BACK_BUTTON_H,
      buttonColor
    );

    display.drawRect(
      BACK_BUTTON_X + 1,
      BACK_BUTTON_Y + 1,
      BACK_BUTTON_W - 2,
      BACK_BUTTON_H - 2,
      buttonColor
    );

    int16_t centerX =
      BACK_BUTTON_X +
      (BACK_BUTTON_W / 2);

    int16_t centerY =
      BACK_BUTTON_Y +
      (BACK_BUTTON_H / 2);

    display.fillTriangle(
      centerX - 11,
      centerY,

      centerX - 1,
      centerY - 9,

      centerX - 1,
      centerY + 9,

      iconColor
    );

    display.fillRect(
      centerX - 1,
      centerY - 2,
      13,
      5,
      iconColor
    );

    backButtonVisualPressed =
      pressed;
  }

  // =========================================================
  // COLOR details
  // =========================================================

  void drawColorDetails(
    uint32_t color
  )
  {
    display.fillRect(
      0,
      60,
      screenWidth,
      78,
      TFT_BLACK
    );

    uint8_t r =
      R(color);

    uint8_t g =
      G(color);

    uint8_t b =
      B(color);

    uint16_t previewColor =
      rgbTo565(
        r,
        g,
        b
      );

    display.fillRect(
      COLOR_PREVIEW_X,
      COLOR_PREVIEW_Y,
      COLOR_PREVIEW_W,
      COLOR_PREVIEW_H,
      previewColor
    );

    display.drawRect(
      COLOR_PREVIEW_X,
      COLOR_PREVIEW_Y,
      COLOR_PREVIEW_W,
      COLOR_PREVIEW_H,
      TFT_WHITE
    );

    display.drawRect(
      COLOR_PREVIEW_X + 1,
      COLOR_PREVIEW_Y + 1,
      COLOR_PREVIEW_W - 2,
      COLOR_PREVIEW_H - 2,
      TFT_DARKGREY
    );

    char hexText[16];

    snprintf(
      hexText,
      sizeof(hexText),
      "#%02X%02X%02X",
      r,
      g,
      b
    );

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(
      2
    );

    display.drawString(
      hexText,
      screenWidth / 2,
      121
    );

    display.drawFastHLine(
      32,
      136,
      screenWidth - 64,
      TFT_DARKGREY
    );
  }

  // =========================================================
  // Hue control
  // =========================================================

  void drawHue(
    uint8_t hueValue,
    TouchTarget pressedTarget =
      TOUCH_TARGET_NONE
  )
  {
    display.fillRect(
      0,
      138,
      screenWidth,
      56,
      TFT_BLACK
    );

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(
      1
    );

    display.drawString(
      "Hue",
      screenWidth / 2,
      144
    );

    drawTriangleButton(
      CONTROL_LEFT_X,
      HUE_BUTTON_Y,
      false,
      pressedTarget ==
        TOUCH_TARGET_HUE_DOWN
    );

    drawTriangleButton(
      CONTROL_RIGHT_X,
      HUE_BUTTON_Y,
      true,
      pressedTarget ==
        TOUCH_TARGET_HUE_UP
    );

    char valueText[8];

    snprintf(
      valueText,
      sizeof(valueText),
      "%u",
      hueValue
    );

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(
      2
    );

    display.drawString(
      valueText,
      screenWidth / 2,
      HUE_BUTTON_Y +
        (CONTROL_BUTTON_H / 2)
    );
  }

  // =========================================================
  // MAIN screen
  // =========================================================

  void drawMainScreen(
    const String& ipAddress
  )
  {
    display.fillScreen(
      TFT_BLACK
    );

    currentPage =
      SCREEN_MAIN;

    readyScreenShown =
      true;

    connectingScreenShown =
      false;

    resetTouchGesture();

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(
      2
    );

    display.drawString(
      "WLED M5Stack CoreS3",
      HEADER_CENTER_X,
      HEADER_TITLE_Y
    );

    display.setTextSize(
      1
    );

    display.drawString(
      ipAddress,
      HEADER_CENTER_X,
      HEADER_IP_Y
    );

    display.drawFastHLine(
      8,
      58,
      screenWidth - 16,
      TFT_DARKGREY
    );

    drawPowerButton(
      bri > 0,
      false
    );

    drawBrightness(
      bri,
      TOUCH_TARGET_NONE
    );

    uint8_t effectMode =
      getCurrentEffectMode();

    drawEffect(
      effectMode,
      TOUCH_TARGET_NONE
    );

    uint32_t primaryColor =
      getPrimaryColor();

    drawColorButton(
      primaryColor,
      false
    );

    // Initial synchronization of logical HSV.
    syncLogicalColorFromRgb(
      primaryColor
    );

    lastLedState =
      bri > 0
        ? 1
        : 0;

    lastBrightnessValue =
      bri;

    lastEffectMode =
      effectMode;

    lastPrimaryColor =
      primaryColor;

    lastPrimaryColorValid =
      true;
  }

  // =========================================================
  // COLOR screen
  // =========================================================

  void drawColorScreen()
  {
    display.fillScreen(
      TFT_BLACK
    );

    currentPage =
      SCREEN_COLOR;

    readyScreenShown =
      true;

    connectingScreenShown =
      false;

    resetTouchGesture();

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(
      2
    );

    display.drawString(
      "COLOR",
      screenWidth / 2,
      18
    );

    display.setTextSize(
      1
    );

    display.drawString(
      "Primary Color",
      screenWidth / 2,
      41
    );

    display.drawFastHLine(
      8,
      58,
      screenWidth - 16,
      TFT_DARKGREY
    );

    drawPowerButton(
      bri > 0,
      false
    );

    drawBackButton(
      false
    );

    uint32_t primaryColor =
      getPrimaryColor();

    // Synchronize once when entering COLOR screen.
    syncLogicalColorFromRgb(
      primaryColor
    );

    drawColorDetails(
      primaryColor
    );

    drawHue(
      logicalHueValue,
      TOUCH_TARGET_NONE
    );

    lastLedState =
      bri > 0
        ? 1
        : 0;

    lastPrimaryColor =
      primaryColor;

    lastPrimaryColorValid =
      true;

    lastHueValue =
      logicalHueValue;
  }

  // =========================================================
  // Generic rectangle hit test
  // =========================================================

  bool pointInsideRect(
    int16_t px,
    int16_t py,
    int16_t x,
    int16_t y,
    int16_t w,
    int16_t h
  )
  {
    return (
      px >= x &&
      px < x + w &&
      py >= y &&
      py < y + h
    );
  }

  // =========================================================
  // Hit tests
  // =========================================================

  bool isPowerButtonTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      POWER_BUTTON_X,
      POWER_BUTTON_Y,
      POWER_BUTTON_W,
      POWER_BUTTON_H
    );
  }

  bool isBrightnessDownTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      CONTROL_LEFT_X,
      BRI_BUTTON_Y,
      CONTROL_BUTTON_W,
      CONTROL_BUTTON_H
    );
  }

  bool isBrightnessUpTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      CONTROL_RIGHT_X,
      BRI_BUTTON_Y,
      CONTROL_BUTTON_W,
      CONTROL_BUTTON_H
    );
  }

  bool isEffectPrevTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      CONTROL_LEFT_X,
      FX_BUTTON_Y,
      CONTROL_BUTTON_W,
      CONTROL_BUTTON_H
    );
  }

  bool isEffectNextTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      CONTROL_RIGHT_X,
      FX_BUTTON_Y,
      CONTROL_BUTTON_W,
      CONTROL_BUTTON_H
    );
  }

  bool isColorButtonTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      COLOR_BUTTON_X,
      COLOR_BUTTON_Y,
      COLOR_BUTTON_W,
      COLOR_BUTTON_H
    );
  }

  bool isBackButtonTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      BACK_BUTTON_X,
      BACK_BUTTON_Y,
      BACK_BUTTON_W,
      BACK_BUTTON_H
    );
  }

  bool isHueDownTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      CONTROL_LEFT_X,
      HUE_BUTTON_Y,
      CONTROL_BUTTON_W,
      CONTROL_BUTTON_H
    );
  }

  bool isHueUpTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      CONTROL_RIGHT_X,
      HUE_BUTTON_Y,
      CONTROL_BUTTON_W,
      CONTROL_BUTTON_H
    );
  }

  // =========================================================
  // Begin Hue gesture
  //
  // IMPORTANT:
  //
  // Normally this starts from persistent logical HSV.
  //
  // RGB -> HSV is only performed if:
  //
  //   - Logical state is not initialized
  //   - Current RGB differs from our cached RGB
  //
  // The latter means the color was changed externally.
  // =========================================================

  void beginHueEdit()
  {
    uint32_t currentColor =
      getPrimaryColor();

    if (
      !logicalColorHsvValid ||
      !lastPrimaryColorValid ||
      currentColor !=
        lastPrimaryColor
    )
    {
      syncLogicalColorFromRgb(
        currentColor
      );

      lastPrimaryColor =
        currentColor;

      lastPrimaryColorValid =
        true;
    }

    hueEditHsv =
      logicalColorHsv;

    hueEditValue =
      logicalHueValue;

    hueEditWhite =
      logicalWhiteValue;

    hueEditValid =
      true;

    lastHueValue =
      hueEditValue;

    Serial.printf(
      "[CoreS3_Display] "
      "Hue edit start: "
      "H=%u S=%u V=%u W=%u\n",
      hueEditValue,
      hueEditHsv.s,
      hueEditHsv.v,
      hueEditWhite
    );
  }

  // =========================================================
  // Reset touch state
  //
  // NOTE:
  //
  // logicalColorHsv is intentionally NOT reset here.
  //
  // It persists between short Hue gestures.
  // =========================================================

  void resetTouchGesture()
  {
    touchActive =
      false;

    touchTarget =
      TOUCH_TARGET_NONE;

    lastTouchInsidePower =
      false;

    lastTouchInsideBrightness =
      false;

    lastTouchInsideEffect =
      false;

    lastTouchInsideColor =
      false;

    lastTouchInsideBack =
      false;

    lastTouchInsideHue =
      false;

    powerButtonVisualPressed =
      false;

    brightnessButtonVisualPressed =
      false;

    effectButtonVisualPressed =
      false;

    colorButtonVisualPressed =
      false;

    backButtonVisualPressed =
      false;

    hueButtonVisualPressed =
      false;

    brightnessLongPressActive =
      false;

    hueLongPressActive =
      false;

    hueEditValid =
      false;

    touchReleaseCandidate =
      0;

    controlPressStartTime =
      0;

    lastBrightnessRepeat =
      0;

    huePressStartTime =
      0;

    lastHueRepeat =
      0;

    lastTouchX =
      -1;

    lastTouchY =
      -1;
  }

  // =========================================================
  // Power
  // =========================================================

  void toggleLedPowerFromTouch()
  {
    Serial.println(
      F(
        "[CoreS3_Display] "
        "Touch POWER action"
      )
    );

    toggleOnOff();

    stateUpdated(
      CALL_MODE_BUTTON
    );

    lastLedState =
      -1;

    lastBrightnessValue =
      -1;

    Serial.printf(
      "[CoreS3_Display] "
      "WLED Power -> %s, Brightness=%u\n",
      bri > 0
        ? "ON"
        : "OFF",
      bri
    );
  }

  // =========================================================
  // Brightness exact value
  // =========================================================

  bool applyBrightnessValue(
    int newValue
  )
  {
    newValue =
      constrain(
        newValue,
        0,
        255
      );

    if (
      newValue ==
      bri
    )
    {
      return false;
    }

    if (
      newValue ==
      0
    )
    {
      if (
        bri >
        0
      )
      {
        briLast =
          bri;

        bri =
          0;
      }
    }
    else
    {
      if (
        bri ==
        0
      )
      {
        strip.restartRuntime();
      }

      bri =
        (uint8_t)newValue;
    }

    stateUpdated(
      CALL_MODE_BUTTON
    );

    lastLedState =
      -1;

    Serial.printf(
      "[CoreS3_Display] "
      "Brightness -> %u\n",
      bri
    );

    return true;
  }

  // =========================================================
  // Brightness step
  // =========================================================

  void applyBrightnessStep(
    int step
  )
  {
    int newValue =
      (int)bri +
      step;

    newValue =
      constrain(
        newValue,
        0,
        255
      );

    if (
      applyBrightnessValue(
        newValue
      )
    )
    {
      if (
        currentPage ==
        SCREEN_MAIN
      )
      {
        drawBrightness(
          bri,
          touchTarget
        );
      }

      lastBrightnessValue =
        bri;
    }
  }

  // =========================================================
  // Brightness short press
  // =========================================================

  void brightnessShortPress(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_BRIGHTNESS_DOWN
    )
    {
      applyBrightnessStep(
        -BRI_SHORT_STEP
      );
    }
    else if (
      target ==
      TOUCH_TARGET_BRIGHTNESS_UP
    )
    {
      applyBrightnessStep(
        BRI_SHORT_STEP
      );
    }
  }

  // =========================================================
  // Brightness long press
  // =========================================================

  void brightnessLongPressStep(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_BRIGHTNESS_DOWN
    )
    {
      applyBrightnessStep(
        -BRI_LONG_STEP
      );
    }
    else if (
      target ==
      TOUCH_TARGET_BRIGHTNESS_UP
    )
    {
      applyBrightnessStep(
        BRI_LONG_STEP
      );
    }
  }

  // =========================================================
  // Effect step
  // =========================================================

  void applyEffectStep(
    int step
  )
  {
    uint8_t modeCount =
      strip.getModeCount();

    if (
      modeCount ==
      0
    )
    {
      return;
    }

    Segment& mainSegment =
      strip.getMainSegment();

    int currentMode =
      mainSegment.mode;

    int newMode =
      currentMode +
      step;

    if (
      newMode <
      0
    )
    {
      newMode =
        modeCount - 1;
    }

    if (
      newMode >=
      modeCount
    )
    {
      newMode =
        0;
    }

    if (
      newMode ==
      currentMode
    )
    {
      return;
    }

    mainSegment.setMode(
      (uint8_t)newMode
    );

    stateUpdated(
      CALL_MODE_BUTTON
    );

    if (
      currentPage ==
      SCREEN_MAIN
    )
    {
      drawEffect(
        mainSegment.mode,
        TOUCH_TARGET_NONE
      );
    }

    lastEffectMode =
      mainSegment.mode;

    char effectName[64];

    getEffectName(
      mainSegment.mode,
      effectName,
      sizeof(effectName)
    );

    Serial.printf(
      "[CoreS3_Display] "
      "Effect -> %u (%s)\n",
      mainSegment.mode,
      effectName
    );
  }

  // =========================================================
  // Apply exact Hue
  //
  // Persistent logical HSV is the source of truth.
  //
  // Even if two adjacent Hue values produce the same RGB,
  // logicalHueValue still advances.
  // =========================================================

  bool applyHueValue(
    uint8_t newHue
  )
  {
    if (
      strip.getSegmentsNum() ==
      0
    )
    {
      return false;
    }

    if (!hueEditValid)
    {
      beginHueEdit();
    }

    if (!hueEditValid)
    {
      return false;
    }

    // -------------------------------------------------------
    // Update logical Hue FIRST.
    // -------------------------------------------------------

    hueEditValue =
      newHue;

    hueEditHsv.h =
      ((uint16_t)newHue) <<
      8;

    // Persist logical state.
    logicalHueValue =
      newHue;

    logicalColorHsv =
      hueEditHsv;

    logicalWhiteValue =
      hueEditWhite;

    logicalColorHsvValid =
      true;

    // -------------------------------------------------------
    // Convert logical HSV to RGB.
    // -------------------------------------------------------

    CRGBW newRgb;

    hsv2rgb_spectrum(
      logicalColorHsv,
      newRgb
    );

    // Preserve W.
    newRgb.w =
      logicalWhiteValue;

    uint32_t newColor =
      newRgb.color32;

    Segment& mainSegment =
      strip.getMainSegment();

    uint32_t oldColor =
      mainSegment.colors[0];

    // -------------------------------------------------------
    // RGB may occasionally be identical for adjacent Hue
    // values because RGB only has 8-bit channels.
    //
    // That is OK.
    //
    // Logical Hue still advances and remains the source of
    // truth for the next button press.
    // -------------------------------------------------------

    if (
      newColor !=
      oldColor
    )
    {
      mainSegment.setColor(
        0,
        newColor
      );

      stateUpdated(
        CALL_MODE_BUTTON
      );
    }

    // -------------------------------------------------------
    // Immediate LCD update.
    // -------------------------------------------------------

    if (
      currentPage ==
      SCREEN_COLOR
    )
    {
      drawColorDetails(
        newColor
      );

      drawHue(
        logicalHueValue,
        touchTarget
      );
    }

    // -------------------------------------------------------
    // Cache the RGB generated by our own logical Hue.
    //
    // This prevents loop() from mistaking our own local
    // change for an external Web UI change.
    // -------------------------------------------------------

    lastPrimaryColor =
      newColor;

    lastPrimaryColorValid =
      true;

    lastHueValue =
      logicalHueValue;

    Serial.printf(
      "[CoreS3_Display] "
      "Hue logical=%u "
      "RGB=#%02X%02X%02X\n",
      logicalHueValue,
      R(newColor),
      G(newColor),
      B(newColor)
    );

    return true;
  }

  // =========================================================
  // Hue step with 0..255 wrap
  // =========================================================

  void applyHueStep(
    int step
  )
  {
    if (!hueEditValid)
    {
      beginHueEdit();
    }

    if (!hueEditValid)
    {
      return;
    }

    // IMPORTANT:
    //
    // Start from persistent logical Hue, not RGB-derived Hue.
    int newValue =
      (int)logicalHueValue +
      step;

    while (
      newValue <
      0
    )
    {
      newValue +=
        256;
    }

    while (
      newValue >
      255
    )
    {
      newValue -=
        256;
    }

    applyHueValue(
      (uint8_t)newValue
    );
  }

  // =========================================================
  // Hue short press
  // =========================================================

  void hueShortPress(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_HUE_DOWN
    )
    {
      applyHueStep(
        -HUE_SHORT_STEP
      );
    }
    else if (
      target ==
      TOUCH_TARGET_HUE_UP
    )
    {
      applyHueStep(
        HUE_SHORT_STEP
      );
    }
  }

  // =========================================================
  // Hue long press
  // =========================================================

  void hueLongPressStep(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_HUE_DOWN
    )
    {
      applyHueStep(
        -HUE_LONG_STEP
      );
    }
    else if (
      target ==
      TOUCH_TARGET_HUE_UP
    )
    {
      applyHueStep(
        HUE_LONG_STEP
      );
    }
  }

  // =========================================================
  // Selected Brightness button
  // =========================================================

  bool isInsideSelectedBrightnessButton(
    int16_t x,
    int16_t y
  )
  {
    if (
      touchTarget ==
      TOUCH_TARGET_BRIGHTNESS_DOWN
    )
    {
      return
        isBrightnessDownTouched(
          x,
          y
        );
    }

    if (
      touchTarget ==
      TOUCH_TARGET_BRIGHTNESS_UP
    )
    {
      return
        isBrightnessUpTouched(
          x,
          y
        );
    }

    return false;
  }

  // =========================================================
  // Selected Effect button
  // =========================================================

  bool isInsideSelectedEffectButton(
    int16_t x,
    int16_t y
  )
  {
    if (
      touchTarget ==
      TOUCH_TARGET_EFFECT_PREV
    )
    {
      return
        isEffectPrevTouched(
          x,
          y
        );
    }

    if (
      touchTarget ==
      TOUCH_TARGET_EFFECT_NEXT
    )
    {
      return
        isEffectNextTouched(
          x,
          y
        );
    }

    return false;
  }

  // =========================================================
  // Selected Hue button
  // =========================================================

  bool isInsideSelectedHueButton(
    int16_t x,
    int16_t y
  )
  {
    if (
      touchTarget ==
      TOUCH_TARGET_HUE_DOWN
    )
    {
      return
        isHueDownTouched(
          x,
          y
        );
    }

    if (
      touchTarget ==
      TOUCH_TARGET_HUE_UP
    )
    {
      return
        isHueUpTouched(
          x,
          y
        );
    }

    return false;
  }

  // =========================================================
  // Touch processing
  // =========================================================

  void handleTouch()
  {
    if (!touchReady)
    {
      return;
    }

    if (!readyScreenShown)
    {
      return;
    }

    unsigned long now =
      millis();

    if (
      now -
      lastTouchPoll <
      TOUCH_POLL_MS
    )
    {
      return;
    }

    lastTouchPoll =
      now;

    int16_t touchX =
      -1;

    int16_t touchY =
      -1;

    bool touching =
      display.getTouch(
        &touchX,
        &touchY
      ) > 0;

    // =======================================================
    // Finger touching
    // =======================================================

    if (touching)
    {
      touchReleaseCandidate =
        0;

      lastTouchX =
        touchX;

      lastTouchY =
        touchY;

      bool insidePower =
        isPowerButtonTouched(
          touchX,
          touchY
        );

      bool insideBrightnessDown =
        false;

      bool insideBrightnessUp =
        false;

      bool insideEffectPrev =
        false;

      bool insideEffectNext =
        false;

      bool insideColor =
        false;

      if (
        currentPage ==
        SCREEN_MAIN
      )
      {
        insideBrightnessDown =
          isBrightnessDownTouched(
            touchX,
            touchY
          );

        insideBrightnessUp =
          isBrightnessUpTouched(
            touchX,
            touchY
          );

        insideEffectPrev =
          isEffectPrevTouched(
            touchX,
            touchY
          );

        insideEffectNext =
          isEffectNextTouched(
            touchX,
            touchY
          );

        insideColor =
          isColorButtonTouched(
            touchX,
            touchY
          );
      }

      bool insideBack =
        false;

      bool insideHueDown =
        false;

      bool insideHueUp =
        false;

      if (
        currentPage ==
        SCREEN_COLOR
      )
      {
        insideBack =
          isBackButtonTouched(
            touchX,
            touchY
          );

        insideHueDown =
          isHueDownTouched(
            touchX,
            touchY
          );

        insideHueUp =
          isHueUpTouched(
            touchX,
            touchY
          );
      }

      // -----------------------------------------------------
      // New gesture
      // -----------------------------------------------------

      if (!touchActive)
      {
        touchActive =
          true;

        touchTarget =
          TOUCH_TARGET_NONE;

        brightnessLongPressActive =
          false;

        hueLongPressActive =
          false;

        Serial.printf(
          "[CoreS3_Display] "
          "Touch start X=%d Y=%d Page=%d\n",
          touchX,
          touchY,
          (int)currentPage
        );
      }

      // -----------------------------------------------------
      // Select target
      // -----------------------------------------------------

      if (
        touchTarget ==
        TOUCH_TARGET_NONE
      )
      {
        if (insidePower)
        {
          touchTarget =
            TOUCH_TARGET_POWER;

          lastTouchInsidePower =
            true;
        }

        else if (
          currentPage ==
          SCREEN_MAIN
        )
        {
          if (
            insideBrightnessDown
          )
          {
            touchTarget =
              TOUCH_TARGET_BRIGHTNESS_DOWN;

            lastTouchInsideBrightness =
              true;

            controlPressStartTime =
              now;

            lastBrightnessRepeat =
              now;
          }

          else if (
            insideBrightnessUp
          )
          {
            touchTarget =
              TOUCH_TARGET_BRIGHTNESS_UP;

            lastTouchInsideBrightness =
              true;

            controlPressStartTime =
              now;

            lastBrightnessRepeat =
              now;
          }

          else if (
            insideEffectPrev
          )
          {
            touchTarget =
              TOUCH_TARGET_EFFECT_PREV;

            lastTouchInsideEffect =
              true;
          }

          else if (
            insideEffectNext
          )
          {
            touchTarget =
              TOUCH_TARGET_EFFECT_NEXT;

            lastTouchInsideEffect =
              true;
          }

          else if (
            insideColor
          )
          {
            touchTarget =
              TOUCH_TARGET_COLOR_OPEN;

            lastTouchInsideColor =
              true;
          }
        }

        else if (
          currentPage ==
          SCREEN_COLOR
        )
        {
          if (
            insideBack
          )
          {
            touchTarget =
              TOUCH_TARGET_BACK;

            lastTouchInsideBack =
              true;
          }

          else if (
            insideHueDown
          )
          {
            touchTarget =
              TOUCH_TARGET_HUE_DOWN;

            lastTouchInsideHue =
              true;

            huePressStartTime =
              now;

            lastHueRepeat =
              now;

            hueLongPressActive =
              false;

            beginHueEdit();
          }

          else if (
            insideHueUp
          )
          {
            touchTarget =
              TOUCH_TARGET_HUE_UP;

            lastTouchInsideHue =
              true;

            huePressStartTime =
              now;

            lastHueRepeat =
              now;

            hueLongPressActive =
              false;

            beginHueEdit();
          }
        }
      }

      // =====================================================
      // Power
      // =====================================================

      if (
        touchTarget ==
        TOUCH_TARGET_POWER
      )
      {
        lastTouchInsidePower =
          insidePower;

        if (
          insidePower !=
          powerButtonVisualPressed
        )
        {
          drawPowerButton(
            bri > 0,
            insidePower
          );
        }

        return;
      }

      // =====================================================
      // Brightness
      // =====================================================

      if (
        touchTarget ==
          TOUCH_TARGET_BRIGHTNESS_DOWN ||
        touchTarget ==
          TOUCH_TARGET_BRIGHTNESS_UP
      )
      {
        bool insideSelectedButton =
          isInsideSelectedBrightnessButton(
            touchX,
            touchY
          );

        lastTouchInsideBrightness =
          insideSelectedButton;

        if (
          insideSelectedButton !=
          brightnessButtonVisualPressed
        )
        {
          drawBrightness(
            bri,
            insideSelectedButton
              ? touchTarget
              : TOUCH_TARGET_NONE
          );

          brightnessButtonVisualPressed =
            insideSelectedButton;
        }

        if (!insideSelectedButton)
        {
          return;
        }

        if (
          !brightnessLongPressActive &&
          now -
          controlPressStartTime >=
          BRI_LONG_PRESS_MS
        )
        {
          brightnessLongPressActive =
            true;

          lastBrightnessRepeat =
            now;

          brightnessLongPressStep(
            touchTarget
          );

          return;
        }

        if (
          brightnessLongPressActive &&
          now -
          lastBrightnessRepeat >=
          BRI_REPEAT_MS
        )
        {
          lastBrightnessRepeat =
            now;

          brightnessLongPressStep(
            touchTarget
          );
        }

        return;
      }

      // =====================================================
      // Effect
      // =====================================================

      if (
        touchTarget ==
          TOUCH_TARGET_EFFECT_PREV ||
        touchTarget ==
          TOUCH_TARGET_EFFECT_NEXT
      )
      {
        bool insideSelectedButton =
          isInsideSelectedEffectButton(
            touchX,
            touchY
          );

        lastTouchInsideEffect =
          insideSelectedButton;

        if (
          insideSelectedButton !=
          effectButtonVisualPressed
        )
        {
          drawEffect(
            getCurrentEffectMode(),
            insideSelectedButton
              ? touchTarget
              : TOUCH_TARGET_NONE
          );

          effectButtonVisualPressed =
            insideSelectedButton;
        }

        return;
      }

      // =====================================================
      // Open COLOR
      // =====================================================

      if (
        touchTarget ==
        TOUCH_TARGET_COLOR_OPEN
      )
      {
        lastTouchInsideColor =
          insideColor;

        if (
          insideColor !=
          colorButtonVisualPressed
        )
        {
          drawColorButton(
            getPrimaryColor(),
            insideColor
          );
        }

        return;
      }

      // =====================================================
      // Back
      // =====================================================

      if (
        touchTarget ==
        TOUCH_TARGET_BACK
      )
      {
        lastTouchInsideBack =
          insideBack;

        if (
          insideBack !=
          backButtonVisualPressed
        )
        {
          drawBackButton(
            insideBack
          );
        }

        return;
      }

      // =====================================================
      // Hue
      // =====================================================

      if (
        touchTarget ==
          TOUCH_TARGET_HUE_DOWN ||
        touchTarget ==
          TOUCH_TARGET_HUE_UP
      )
      {
        bool insideSelectedButton =
          isInsideSelectedHueButton(
            touchX,
            touchY
          );

        lastTouchInsideHue =
          insideSelectedButton;

        if (
          insideSelectedButton !=
          hueButtonVisualPressed
        )
        {
          drawHue(
            logicalHueValue,
            insideSelectedButton
              ? touchTarget
              : TOUCH_TARGET_NONE
          );

          hueButtonVisualPressed =
            insideSelectedButton;
        }

        if (!insideSelectedButton)
        {
          return;
        }

        if (
          !hueLongPressActive &&
          now -
          huePressStartTime >=
          HUE_LONG_PRESS_MS
        )
        {
          hueLongPressActive =
            true;

          lastHueRepeat =
            now;

          hueLongPressStep(
            touchTarget
          );

          return;
        }

        if (
          hueLongPressActive &&
          now -
          lastHueRepeat >=
          HUE_REPEAT_MS
        )
        {
          lastHueRepeat =
            now;

          hueLongPressStep(
            touchTarget
          );
        }

        return;
      }

      return;
    }

    // =======================================================
    // No touch
    // =======================================================

    if (!touchActive)
    {
      return;
    }

    if (
      touchReleaseCandidate ==
      0
    )
    {
      touchReleaseCandidate =
        now;

      return;
    }

    if (
      now -
      touchReleaseCandidate <
      TOUCH_RELEASE_CONFIRM_MS
    )
    {
      return;
    }

    // =======================================================
    // Confirmed release
    // =======================================================

    TouchTarget releasedTarget =
      touchTarget;

    bool wasBrightnessLongPress =
      brightnessLongPressActive;

    bool wasHueLongPress =
      hueLongPressActive;

    bool executePowerAction =
      (
        releasedTarget ==
        TOUCH_TARGET_POWER
      ) &&
      lastTouchInsidePower &&
      (
        now -
        lastTouchAction >=
        TOUCH_ACTION_COOLDOWN_MS
      );

    bool executeBrightnessShortPress =
      (
        releasedTarget ==
          TOUCH_TARGET_BRIGHTNESS_DOWN ||
        releasedTarget ==
          TOUCH_TARGET_BRIGHTNESS_UP
      ) &&
      lastTouchInsideBrightness &&
      !wasBrightnessLongPress;

    bool executeEffectAction =
      (
        releasedTarget ==
          TOUCH_TARGET_EFFECT_PREV ||
        releasedTarget ==
          TOUCH_TARGET_EFFECT_NEXT
      ) &&
      lastTouchInsideEffect;

    bool executeColorOpen =
      (
        releasedTarget ==
        TOUCH_TARGET_COLOR_OPEN
      ) &&
      lastTouchInsideColor;

    bool executeBack =
      (
        releasedTarget ==
        TOUCH_TARGET_BACK
      ) &&
      lastTouchInsideBack;

    bool executeHueShortPress =
      (
        releasedTarget ==
          TOUCH_TARGET_HUE_DOWN ||
        releasedTarget ==
          TOUCH_TARGET_HUE_UP
      ) &&
      lastTouchInsideHue &&
      !wasHueLongPress;

    Serial.printf(
      "[CoreS3_Display] "
      "Touch release X=%d Y=%d "
      "Target=%d BLong=%s HLong=%s\n",
      lastTouchX,
      lastTouchY,
      (int)releasedTarget,
      wasBrightnessLongPress
        ? "YES"
        : "NO",
      wasHueLongPress
        ? "YES"
        : "NO"
    );

    // -------------------------------------------------------
    // Restore visuals
    // -------------------------------------------------------

    if (
      releasedTarget ==
        TOUCH_TARGET_POWER &&
      powerButtonVisualPressed
    )
    {
      drawPowerButton(
        bri > 0,
        false
      );
    }

    if (
      (
        releasedTarget ==
          TOUCH_TARGET_BRIGHTNESS_DOWN ||
        releasedTarget ==
          TOUCH_TARGET_BRIGHTNESS_UP
      ) &&
      brightnessButtonVisualPressed
    )
    {
      drawBrightness(
        bri,
        TOUCH_TARGET_NONE
      );
    }

    if (
      (
        releasedTarget ==
          TOUCH_TARGET_EFFECT_PREV ||
        releasedTarget ==
          TOUCH_TARGET_EFFECT_NEXT
      ) &&
      effectButtonVisualPressed
    )
    {
      drawEffect(
        getCurrentEffectMode(),
        TOUCH_TARGET_NONE
      );
    }

    if (
      releasedTarget ==
        TOUCH_TARGET_COLOR_OPEN &&
      colorButtonVisualPressed
    )
    {
      drawColorButton(
        getPrimaryColor(),
        false
      );
    }

    if (
      releasedTarget ==
        TOUCH_TARGET_BACK &&
      backButtonVisualPressed
    )
    {
      drawBackButton(
        false
      );
    }

    if (
      (
        releasedTarget ==
          TOUCH_TARGET_HUE_DOWN ||
        releasedTarget ==
          TOUCH_TARGET_HUE_UP
      ) &&
      hueButtonVisualPressed
    )
    {
      drawHue(
        logicalHueValue,
        TOUCH_TARGET_NONE
      );
    }

    // -------------------------------------------------------
    // Save Hue gesture state before common reset.
    // -------------------------------------------------------

    bool savedHueEditValid =
      hueEditValid;

    CHSV32 savedHueEditHsv =
      hueEditHsv;

    uint8_t savedHueEditValue =
      hueEditValue;

    uint8_t savedHueEditWhite =
      hueEditWhite;

    // -------------------------------------------------------
    // Reset current gesture.
    //
    // Persistent logicalColorHsv remains intact.
    // -------------------------------------------------------

    touchActive =
      false;

    touchTarget =
      TOUCH_TARGET_NONE;

    lastTouchInsidePower =
      false;

    lastTouchInsideBrightness =
      false;

    lastTouchInsideEffect =
      false;

    lastTouchInsideColor =
      false;

    lastTouchInsideBack =
      false;

    lastTouchInsideHue =
      false;

    powerButtonVisualPressed =
      false;

    brightnessButtonVisualPressed =
      false;

    effectButtonVisualPressed =
      false;

    colorButtonVisualPressed =
      false;

    backButtonVisualPressed =
      false;

    hueButtonVisualPressed =
      false;

    brightnessLongPressActive =
      false;

    hueLongPressActive =
      false;

    touchReleaseCandidate =
      0;

    controlPressStartTime =
      0;

    lastBrightnessRepeat =
      0;

    huePressStartTime =
      0;

    lastHueRepeat =
      0;

    lastTouchX =
      -1;

    lastTouchY =
      -1;

    // Restore Hue gesture data only if needed for the
    // short-press action below.
    hueEditValid =
      savedHueEditValid;

    hueEditHsv =
      savedHueEditHsv;

    hueEditValue =
      savedHueEditValue;

    hueEditWhite =
      savedHueEditWhite;

    // =======================================================
    // Execute action
    // =======================================================

    if (executePowerAction)
    {
      lastTouchAction =
        now;

      toggleLedPowerFromTouch();

      hueEditValid =
        false;

      return;
    }

    if (executeBrightnessShortPress)
    {
      brightnessShortPress(
        releasedTarget
      );

      drawBrightness(
        bri,
        TOUCH_TARGET_NONE
      );

      lastBrightnessValue =
        bri;

      hueEditValid =
        false;

      return;
    }

    if (executeEffectAction)
    {
      if (
        releasedTarget ==
        TOUCH_TARGET_EFFECT_PREV
      )
      {
        applyEffectStep(
          -1
        );
      }
      else
      {
        applyEffectStep(
          1
        );
      }

      hueEditValid =
        false;

      return;
    }

    if (executeColorOpen)
    {
      Serial.println(
        F(
          "[CoreS3_Display] "
          "Open COLOR screen"
        )
      );

      hueEditValid =
        false;

      drawColorScreen();

      return;
    }

    if (executeBack)
    {
      Serial.println(
        F(
          "[CoreS3_Display] "
          "Return MAIN screen"
        )
      );

      hueEditValid =
        false;

      drawMainScreen(
        WiFi.localIP().toString()
      );

      return;
    }

    if (executeHueShortPress)
    {
      hueShortPress(
        releasedTarget
      );

      drawHue(
        logicalHueValue,
        TOUCH_TARGET_NONE
      );

      lastHueValue =
        logicalHueValue;

      hueEditValid =
        false;

      return;
    }

    // Long press already applied its Hue changes.
    hueEditValid =
      false;
  }

public:

  // =========================================================
  // Setup
  // =========================================================

  void setup() override
  {
    Serial.println();

    Serial.println(
      F(
        "[CoreS3_Display] "
        "Phase 7.3.1 start"
      )
    );

    display.begin();

    display.setRotation(
      1
    );

    screenWidth =
      display.width();

    screenHeight =
      display.height();

    Serial.printf(
      "[CoreS3_Display] "
      "Display size: %d x %d\n",
      screenWidth,
      screenHeight
    );

    if (
      screenWidth <= 0 ||
      screenHeight <= 0
    )
    {
      Serial.println(
        F(
          "[CoreS3_Display] "
          "ERROR: Display not detected"
        )
      );

      return;
    }

    touchReady =
      (
        display.touch() !=
        nullptr
      );

    Serial.printf(
      "[CoreS3_Display] "
      "Touch: %s\n",
      touchReady
        ? "READY"
        : "NOT FOUND"
    );

    display.setBrightness(
      128
    );

    drawBootScreen();

    displayReady =
      true;

    Serial.println(
      F(
        "[CoreS3_Display] "
        "Display initialized"
      )
    );

    Serial.println(
      F(
        "[CoreS3_Display] "
        "Phase 7.3.1 setup complete"
      )
    );

    Serial.println();
  }

  // =========================================================
  // Main loop
  // =========================================================

  void loop() override
  {
    if (!displayReady)
    {
      return;
    }

    handleTouch();

    unsigned long now =
      millis();

    if (
      now -
      lastUpdate <
      250
    )
    {
      return;
    }

    lastUpdate =
      now;

    bool wifiConnected =
      (
        WiFi.status() ==
        WL_CONNECTED
      );

    // =======================================================
    // Wi-Fi disconnected
    // =======================================================

    if (!wifiConnected)
    {
      if (
        !connectingScreenShown
      )
      {
        drawConnectingScreen();
      }

      lastWiFiConnected =
        false;

      lastIPAddress =
        "";

      return;
    }

    // =======================================================
    // First connection / reconnect
    // =======================================================

    String currentIPAddress =
      WiFi.localIP().toString();

    if (
      !lastWiFiConnected ||
      !readyScreenShown
    )
    {
      drawMainScreen(
        currentIPAddress
      );

      Serial.printf(
        "[CoreS3_Display] "
        "Wi-Fi connected: %s\n",
        currentIPAddress.c_str()
      );

      lastIPAddress =
        currentIPAddress;

      lastWiFiConnected =
        true;

      return;
    }

    // =======================================================
    // IP changed
    // =======================================================

    if (
      currentIPAddress !=
      lastIPAddress
    )
    {
      lastIPAddress =
        currentIPAddress;

      if (
        currentPage ==
        SCREEN_MAIN
      )
      {
        drawMainScreen(
          currentIPAddress
        );

        return;
      }
    }

    lastWiFiConnected =
      true;

    // =======================================================
    // Power
    // =======================================================

    bool ledOn =
      (bri > 0);

    if (
      (int8_t)ledOn !=
      lastLedState
    )
    {
      if (
        touchTarget !=
        TOUCH_TARGET_POWER
      )
      {
        drawPowerButton(
          ledOn,
          false
        );
      }

      lastLedState =
        ledOn
          ? 1
          : 0;
    }

    // =======================================================
    // Primary Color
    // =======================================================

    uint32_t primaryColor =
      getPrimaryColor();

    bool primaryColorChanged =
      (
        !lastPrimaryColorValid ||
        primaryColor !=
          lastPrimaryColor
      );

    bool hueTouchActive =
      (
        touchTarget ==
          TOUCH_TARGET_HUE_DOWN ||
        touchTarget ==
          TOUCH_TARGET_HUE_UP
      );

    bool primaryColorChangeHandled =
      false;

    // =======================================================
    // MAIN
    // =======================================================

    if (
      currentPage ==
      SCREEN_MAIN
    )
    {
      bool brightnessTouchActive =
        (
          touchTarget ==
            TOUCH_TARGET_BRIGHTNESS_DOWN ||
          touchTarget ==
            TOUCH_TARGET_BRIGHTNESS_UP
        );

      if (
        !brightnessTouchActive &&
        (int)bri !=
          lastBrightnessValue
      )
      {
        drawBrightness(
          bri,
          TOUCH_TARGET_NONE
        );

        lastBrightnessValue =
          bri;
      }

      uint8_t effectMode =
        getCurrentEffectMode();

      bool effectTouchActive =
        (
          touchTarget ==
            TOUCH_TARGET_EFFECT_PREV ||
          touchTarget ==
            TOUCH_TARGET_EFFECT_NEXT
        );

      if (
        !effectTouchActive &&
        (int)effectMode !=
          lastEffectMode
      )
      {
        drawEffect(
          effectMode,
          TOUCH_TARGET_NONE
        );

        lastEffectMode =
          effectMode;
      }

      // -----------------------------------------------------
      // External Primary Color change on MAIN.
      // -----------------------------------------------------

      if (
        primaryColorChanged &&
        touchTarget !=
          TOUCH_TARGET_COLOR_OPEN
      )
      {
        syncLogicalColorFromRgb(
          primaryColor
        );

        drawColorButton(
          primaryColor,
          false
        );

        primaryColorChangeHandled =
          true;
      }
    }

    // =======================================================
    // COLOR
    // =======================================================

    else if (
      currentPage ==
      SCREEN_COLOR
    )
    {
      // -----------------------------------------------------
      // External Web UI color change.
      //
      // Re-synchronize logical Hue only when local Hue control
      // is not active.
      // -----------------------------------------------------

      if (
        primaryColorChanged &&
        !hueTouchActive
      )
      {
        syncLogicalColorFromRgb(
          primaryColor
        );

        drawColorDetails(
          primaryColor
        );

        drawHue(
          logicalHueValue,
          TOUCH_TARGET_NONE
        );

        lastHueValue =
          logicalHueValue;

        primaryColorChangeHandled =
          true;
      }
    }

    // =======================================================
    // Save external color cache only after it has actually
    // been handled.
    //
    // If an external change happens during a local Hue touch,
    // leave it pending. It will be detected after release.
    // =======================================================

    if (
      primaryColorChanged &&
      primaryColorChangeHandled
    )
    {
      lastPrimaryColor =
        primaryColor;

      lastPrimaryColorValid =
        true;
    }
  }

  // =========================================================
  // WLED Info
  // =========================================================

  void addToJsonInfo(
    JsonObject& root
  ) override
  {
    JsonObject user =
      root["u"];

    if (
      user.isNull()
    )
    {
      user =
        root.createNestedObject(
          "u"
        );
    }

    // -------------------------------------------------------
    // Display
    // -------------------------------------------------------

    JsonArray displayInfo =
      user.createNestedArray(
        "CoreS3 Display"
      );

    if (displayReady)
    {
      char text[32];

      snprintf(
        text,
        sizeof(text),
        "READY (%d x %d)",
        screenWidth,
        screenHeight
      );

      displayInfo.add(
        text
      );
    }
    else
    {
      displayInfo.add(
        "FAILED"
      );
    }

    // -------------------------------------------------------
    // Touch
    // -------------------------------------------------------

    JsonArray touchInfo =
      user.createNestedArray(
        "CoreS3 Display Touch"
      );

    touchInfo.add(
      touchReady
        ? "READY"
        : "NOT FOUND"
    );

    // -------------------------------------------------------
    // Wi-Fi
    // -------------------------------------------------------

    JsonArray wifiInfo =
      user.createNestedArray(
        "CoreS3 Display WiFi"
      );

    if (
      WiFi.status() ==
      WL_CONNECTED
    )
    {
      wifiInfo.add(
        WiFi.localIP().toString()
      );
    }
    else
    {
      wifiInfo.add(
        "Not connected"
      );
    }

    // -------------------------------------------------------
    // LED
    // -------------------------------------------------------

    JsonArray ledInfo =
      user.createNestedArray(
        "CoreS3 Display LED"
      );

    ledInfo.add(
      bri > 0
        ? "ON"
        : "OFF"
    );

    // -------------------------------------------------------
    // Brightness
    // -------------------------------------------------------

    JsonArray brightnessInfo =
      user.createNestedArray(
        "CoreS3 Display Brightness"
      );

    brightnessInfo.add(
      bri
    );

    // -------------------------------------------------------
    // Effect
    // -------------------------------------------------------

    JsonArray effectInfo =
      user.createNestedArray(
        "CoreS3 Display Effect"
      );

    if (
      strip.getSegmentsNum() >
      0
    )
    {
      uint8_t effectMode =
        strip.getMainSegment().mode;

      char effectName[64];

      getEffectName(
        effectMode,
        effectName,
        sizeof(effectName)
      );

      effectInfo.add(
        effectName
      );
    }
    else
    {
      effectInfo.add(
        "No segment"
      );
    }

    // -------------------------------------------------------
    // Primary Color
    // -------------------------------------------------------

    JsonArray colorInfo =
      user.createNestedArray(
        "CoreS3 Display Color"
      );

    if (
      strip.getSegmentsNum() >
      0
    )
    {
      uint32_t color =
        getPrimaryColor();

      char colorText[16];

      snprintf(
        colorText,
        sizeof(colorText),
        "#%02X%02X%02X",
        R(color),
        G(color),
        B(color)
      );

      colorInfo.add(
        colorText
      );
    }
    else
    {
      colorInfo.add(
        "No segment"
      );
    }

    // -------------------------------------------------------
    // Hue
    //
    // If the logical state matches the currently cached RGB,
    // report logical Hue instead of re-deriving Hue from RGB.
    // -------------------------------------------------------

    JsonArray hueInfo =
      user.createNestedArray(
        "CoreS3 Display Hue"
      );

    if (
      strip.getSegmentsNum() >
      0
    )
    {
      hueInfo.add(
        getDisplayedHue()
      );
    }
    else
    {
      hueInfo.add(
        "No segment"
      );
    }

    // -------------------------------------------------------
    // Page
    // -------------------------------------------------------

    JsonArray pageInfo =
      user.createNestedArray(
        "CoreS3 Display Page"
      );

    pageInfo.add(
      currentPage ==
        SCREEN_MAIN
        ? "MAIN"
        : "COLOR"
    );
  }
};

static CoreS3DisplayUsermod coreS3DisplayUsermod;

REGISTER_USERMOD(coreS3DisplayUsermod);
