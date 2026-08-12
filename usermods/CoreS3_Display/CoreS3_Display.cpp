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

  uint32_t lastPrimaryColor = 0;
  bool lastPrimaryColorValid = false;

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
    TOUCH_TARGET_BACK
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

  bool powerButtonVisualPressed = false;
  bool brightnessButtonVisualPressed = false;
  bool effectButtonVisualPressed = false;
  bool colorButtonVisualPressed = false;
  bool backButtonVisualPressed = false;

  bool brightnessLongPressActive = false;

  int16_t lastTouchX = -1;
  int16_t lastTouchY = -1;

  unsigned long controlPressStartTime = 0;
  unsigned long lastBrightnessRepeat = 0;

  // =========================================================
  // Power button
  // =========================================================

  static constexpr int16_t POWER_BUTTON_X = 8;
  static constexpr int16_t POWER_BUTTON_Y = 8;
  static constexpr int16_t POWER_BUTTON_W = 44;
  static constexpr int16_t POWER_BUTTON_H = 44;

  // =========================================================
  // MAIN header
  //
  // Power button occupies the left side.
  //
  // Header content:
  //   X = 60 .. 312
  //
  // Center:
  //   X = 186
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
  // COLOR screen Back button
  //
  // Symmetrical with Power button.
  // =========================================================

  static constexpr int16_t BACK_BUTTON_X = 268;
  static constexpr int16_t BACK_BUTTON_Y = 8;
  static constexpr int16_t BACK_BUTTON_W = 44;
  static constexpr int16_t BACK_BUTTON_H = 44;

  // =========================================================
  // COLOR preview
  //
  // Lower section is intentionally kept free for:
  //
  // Phase 7.3 -> Hue
  // Phase 7.4 -> Saturation
  // =========================================================

  static constexpr int16_t COLOR_PREVIEW_X = 92;
  static constexpr int16_t COLOR_PREVIEW_Y = 68;
  static constexpr int16_t COLOR_PREVIEW_W = 136;
  static constexpr int16_t COLOR_PREVIEW_H = 36;

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
  // RGB888 -> RGB565
  //
  // Used only for LCD color preview.
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
  // Current WLED Primary Color
  //
  // WLED Segment:
  //   colors[0] = Primary Color
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
  // Wi-Fi connecting screen
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

    // Opening at top
    display.fillRect(
      centerX - 4,
      centerY - 11,
      9,
      8,
      backgroundColor
    );

    // Power stem
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

    // Right
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

    // Left
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

    // Left
    drawTriangleButton(
      CONTROL_LEFT_X,
      BRI_BUTTON_Y,
      false,
      pressedTarget ==
        TOUCH_TARGET_BRIGHTNESS_DOWN
    );

    // Right
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
  // Effect name helper
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
      ] = '\0';
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

    // Previous
    drawTriangleButton(
      CONTROL_LEFT_X,
      FX_BUTTON_Y,
      false,
      pressedTarget ==
        TOUCH_TARGET_EFFECT_PREV
    );

    // Next
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
  // MAIN screen Color button
  //
  // Shows a small preview of the current Primary Color.
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

    // RGBW32 = 0xWWRRGGBB
    uint8_t r =
      (uint8_t)((color >> 16) & 0xFF);

    uint8_t g =
      (uint8_t)((color >> 8) & 0xFF);

    uint8_t b =
      (uint8_t)(color & 0xFF);

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

    // -------------------------------------------------------
    // Current Color swatch
    // -------------------------------------------------------

    static constexpr int16_t SWATCH_W = 26;
    static constexpr int16_t SWATCH_H = 24;

    int16_t swatchX =
      COLOR_BUTTON_X + 14;

    int16_t swatchY =
      COLOR_BUTTON_Y +
      ((COLOR_BUTTON_H - SWATCH_H) / 2);

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

    // -------------------------------------------------------
    // COLOR label
    // -------------------------------------------------------

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
      COLOR_BUTTON_X +
        115,
      COLOR_BUTTON_Y +
        (COLOR_BUTTON_H / 2)
    );

    colorButtonVisualPressed =
      pressed;
  }

  // =========================================================
  // Back button
  //
  // Right-top on COLOR screen.
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

    // Left-pointing arrow head
    display.fillTriangle(
      centerX - 11,
      centerY,

      centerX - 1,
      centerY - 9,

      centerX - 1,
      centerY + 9,

      iconColor
    );

    // Arrow tail
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
  // COLOR screen current Primary Color
  // =========================================================

  void drawColorDetails(
    uint32_t color
  )
  {
    // Clear only the preview area.
    // Lower screen remains available for Hue/Saturation.
    display.fillRect(
      0,
      60,
      screenWidth,
      78,
      TFT_BLACK
    );

    // RGBW32 = 0xWWRRGGBB
    uint8_t r =
      (uint8_t)((color >> 16) & 0xFF);

    uint8_t g =
      (uint8_t)((color >> 8) & 0xFF);

    uint8_t b =
      (uint8_t)(color & 0xFF);

    uint16_t previewColor =
      rgbTo565(
        r,
        g,
        b
      );

    // -------------------------------------------------------
    // Preview
    // -------------------------------------------------------

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

    // -------------------------------------------------------
    // #RRGGBB
    // -------------------------------------------------------

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

    // Divider for future controls
    display.drawFastHLine(
      32,
      136,
      screenWidth - 64,
      TFT_DARKGREY
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

    // -------------------------------------------------------
    // Header
    // -------------------------------------------------------

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

    // -------------------------------------------------------
    // Controls
    // -------------------------------------------------------

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

    // -------------------------------------------------------
    // Cache current values
    // -------------------------------------------------------

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

    // -------------------------------------------------------
    // Header
    //
    // Power and Back are symmetrical, so COLOR can use
    // physical center X=160.
    // -------------------------------------------------------

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

    // -------------------------------------------------------
    // Navigation
    // -------------------------------------------------------

    drawPowerButton(
      bri > 0,
      false
    );

    drawBackButton(
      false
    );

    // -------------------------------------------------------
    // Primary Color
    // -------------------------------------------------------

    uint32_t primaryColor =
      getPrimaryColor();

    drawColorDetails(
      primaryColor
    );

    lastLedState =
      bri > 0
        ? 1
        : 0;

    lastPrimaryColor =
      primaryColor;

    lastPrimaryColorValid =
      true;
  }

  // =========================================================
  // Generic hit test
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

  // =========================================================
  // Reset touch state
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

    brightnessLongPressActive =
      false;

    touchReleaseCandidate =
      0;

    controlPressStartTime =
      0;

    lastBrightnessRepeat =
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
  // Selected Brightness button check
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
  // Selected Effect button check
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

      // -----------------------------------------------------
      // Common control
      // -----------------------------------------------------

      bool insidePower =
        isPowerButtonTouched(
          touchX,
          touchY
        );

      // -----------------------------------------------------
      // MAIN controls
      // -----------------------------------------------------

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

      // -----------------------------------------------------
      // COLOR controls
      // -----------------------------------------------------

      bool insideBack =
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
        // Power is available on every page.
        if (insidePower)
        {
          touchTarget =
            TOUCH_TARGET_POWER;

          lastTouchInsidePower =
            true;
        }

        // MAIN
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

        // COLOR
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

        // Long press start
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

        // Repeat
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
      // Open COLOR screen
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

      return;
    }

    // =======================================================
    // No touch
    // =======================================================

    if (!touchActive)
    {
      return;
    }

    // -------------------------------------------------------
    // Start release confirmation
    // -------------------------------------------------------

    if (
      touchReleaseCandidate ==
      0
    )
    {
      touchReleaseCandidate =
        now;

      return;
    }

    // -------------------------------------------------------
    // Ignore temporary touch loss
    // -------------------------------------------------------

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

    bool wasLongPress =
      brightnessLongPressActive;

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
      !wasLongPress;

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

    Serial.printf(
      "[CoreS3_Display] "
      "Touch release X=%d Y=%d "
      "Target=%d Long=%s\n",
      lastTouchX,
      lastTouchY,
      (int)releasedTarget,
      wasLongPress
        ? "YES"
        : "NO"
    );

    // -------------------------------------------------------
    // Restore Power
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

    // -------------------------------------------------------
    // Restore Brightness
    // -------------------------------------------------------

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

    // -------------------------------------------------------
    // Restore Effect
    // -------------------------------------------------------

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

    // -------------------------------------------------------
    // Restore COLOR button
    // -------------------------------------------------------

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

    // -------------------------------------------------------
    // Restore Back
    // -------------------------------------------------------

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

    // -------------------------------------------------------
    // Reset gesture
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

    brightnessLongPressActive =
      false;

    touchReleaseCandidate =
      0;

    controlPressStartTime =
      0;

    lastBrightnessRepeat =
      0;

    lastTouchX =
      -1;

    lastTouchY =
      -1;

    // =======================================================
    // Execute action
    // =======================================================

    if (executePowerAction)
    {
      lastTouchAction =
        now;

      toggleLedPowerFromTouch();

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

      drawMainScreen(
        WiFi.localIP().toString()
      );

      return;
    }
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
        "Phase 7.1 + 7.2 start"
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
        "Phase 7.1 + 7.2 setup complete"
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

    // -------------------------------------------------------
    // Fast touch processing
    // -------------------------------------------------------

    handleTouch();

    unsigned long now =
      millis();

    // -------------------------------------------------------
    // Normal LCD state update
    // -------------------------------------------------------

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
    // First connection / reconnection
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
    // IP address changed
    // =======================================================

    if (
      currentIPAddress !=
      lastIPAddress
    )
    {
      lastIPAddress =
        currentIPAddress;

      // IP is only shown on MAIN screen.
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
    //
    // Available on both MAIN and COLOR screens.
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
    // Current Primary Color
    // =======================================================

    uint32_t primaryColor =
      getPrimaryColor();

    bool primaryColorChanged =
      (
        !lastPrimaryColorValid ||
        primaryColor !=
          lastPrimaryColor
      );

    // =======================================================
    // MAIN page updates
    // =======================================================

    if (
      currentPage ==
      SCREEN_MAIN
    )
    {
      // -----------------------------------------------------
      // Brightness
      // -----------------------------------------------------

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

      // -----------------------------------------------------
      // Effect
      // -----------------------------------------------------

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
      // Primary Color preview button
      // -----------------------------------------------------

      if (
        primaryColorChanged &&
        touchTarget !=
          TOUCH_TARGET_COLOR_OPEN
      )
      {
        drawColorButton(
          primaryColor,
          false
        );
      }
    }

    // =======================================================
    // COLOR page updates
    // =======================================================

    else if (
      currentPage ==
      SCREEN_COLOR
    )
    {
      if (primaryColorChanged)
      {
        drawColorDetails(
          primaryColor
        );
      }
    }

    // -------------------------------------------------------
    // Save Primary Color cache
    // -------------------------------------------------------

    if (primaryColorChanged)
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

      uint8_t r =
        (uint8_t)((color >> 16) & 0xFF);

      uint8_t g =
        (uint8_t)((color >> 8) & 0xFF);

      uint8_t b =
        (uint8_t)(color & 0xFF);

      char colorText[16];

      snprintf(
        colorText,
        sizeof(colorText),
        "#%02X%02X%02X",
        r,
        g,
        b
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
    // Current display page
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
