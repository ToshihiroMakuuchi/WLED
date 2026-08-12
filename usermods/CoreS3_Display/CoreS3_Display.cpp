#include "wled.h"
#include <WiFi.h>
#include <M5GFX.h>

#include "CoreS3_WLED_Logo.h"

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
  int lastSaturationValue = -1;

  uint32_t lastPrimaryColor = 0;
  bool lastPrimaryColorValid = false;

  // =========================================================
  // Display brightness
  // =========================================================

  static constexpr uint8_t DISPLAY_NORMAL_BRIGHTNESS = 128;

  uint8_t currentDisplayBrightness = 0;

  // =========================================================
  // Phase 8.3 - Display suspend
  //
  // 30 seconds without touch:
  //
  // ACTIVE
  //   -> Fade Out
  //   -> Brightness 0
  //   -> SLEEP
  //
  // First touch:
  //
  // SLEEP
  //   -> consume first touch
  //   -> redraw latest screen
  //   -> Fade In
  //   -> wait until finger is released
  //   -> ACTIVE
  // =========================================================

  static constexpr unsigned long DISPLAY_SLEEP_TIMEOUT_MS = 30000;

  static constexpr unsigned long DISPLAY_FADE_INTERVAL_MS = 8;
  static constexpr uint8_t DISPLAY_FADE_STEP = 4;

  enum DisplayPowerState : uint8_t
  {
    DISPLAY_POWER_ACTIVE = 0,
    DISPLAY_POWER_SLEEP_FADE_OUT,
    DISPLAY_POWER_SLEEPING,
    DISPLAY_POWER_WAKE_FADE_IN,
    DISPLAY_POWER_WAKE_WAIT_RELEASE
  };

  DisplayPowerState displayPowerState =
    DISPLAY_POWER_ACTIVE;

  unsigned long lastUserActivityMs = 0;
  unsigned long displayFadeLastStep = 0;

  unsigned long wakeTouchLastPoll = 0;
  bool wakeTouchState = false;

  unsigned long wakeReleaseCandidate = 0;

  // =========================================================
  // Startup animation
  // =========================================================

  enum StartupState : uint8_t
  {
    STARTUP_FADE_IN = 0,
    STARTUP_WAIT_WIFI,
    STARTUP_CONNECTED_HOLD,
    STARTUP_FADE_OUT,
    STARTUP_MAIN_FADE_IN,
    STARTUP_DONE
  };

  StartupState startupState =
    STARTUP_FADE_IN;

  unsigned long startupStateStart = 0;
  unsigned long startupLastFadeStep = 0;
  unsigned long startupLastDotsUpdate = 0;

  uint8_t startupDotCount = 0;

  String startupIPAddress = "";

  static constexpr unsigned long STARTUP_FADE_INTERVAL_MS = 8;
  static constexpr uint8_t STARTUP_FADE_STEP = 4;

  static constexpr unsigned long STARTUP_CONNECTED_HOLD_MS = 700;
  static constexpr unsigned long STARTUP_DOTS_INTERVAL_MS = 350;

  // =========================================================
  // Persistent logical HSV
  // =========================================================

  CHSV32 logicalColorHsv;

  bool logicalColorHsvValid = false;

  uint8_t logicalHueValue = 0;
  uint8_t logicalSaturationValue = 0;
  uint8_t logicalWhiteValue = 0;

  // =========================================================
  // Pages
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
    TOUCH_TARGET_HUE_UP,

    TOUCH_TARGET_SATURATION_DOWN,
    TOUCH_TARGET_SATURATION_UP
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
  bool lastTouchInsideSaturation = false;

  bool powerButtonVisualPressed = false;
  bool brightnessButtonVisualPressed = false;
  bool effectButtonVisualPressed = false;
  bool colorButtonVisualPressed = false;
  bool backButtonVisualPressed = false;
  bool hueButtonVisualPressed = false;
  bool saturationButtonVisualPressed = false;

  bool brightnessLongPressActive = false;

  // =========================================================
  // Phase 8.3.1
  // Effect long press
  // =========================================================

  bool effectLongPressActive = false;

  bool hueLongPressActive = false;
  bool saturationLongPressActive = false;

  int16_t lastTouchX = -1;
  int16_t lastTouchY = -1;

  unsigned long controlPressStartTime = 0;
  unsigned long lastBrightnessRepeat = 0;

  // =========================================================
  // Phase 8.3.1
  // Effect timing state
  // =========================================================

  unsigned long effectPressStartTime = 0;
  unsigned long lastEffectRepeat = 0;

  unsigned long huePressStartTime = 0;
  unsigned long lastHueRepeat = 0;

  unsigned long saturationPressStartTime = 0;
  unsigned long lastSaturationRepeat = 0;

  // =========================================================
  // Hue gesture
  // =========================================================

  CHSV32 hueEditHsv;

  bool hueEditValid = false;

  uint8_t hueEditValue = 0;
  uint8_t hueEditWhite = 0;

  // =========================================================
  // Saturation gesture
  // =========================================================

  CHSV32 saturationEditHsv;

  bool saturationEditValid = false;

  uint8_t saturationEditValue = 0;
  uint8_t saturationEditWhite = 0;

  // =========================================================
  // Layout
  // =========================================================

  static constexpr int16_t POWER_BUTTON_X = 8;
  static constexpr int16_t POWER_BUTTON_Y = 8;
  static constexpr int16_t POWER_BUTTON_W = 44;
  static constexpr int16_t POWER_BUTTON_H = 44;

  static constexpr int16_t HEADER_CONTENT_LEFT = 60;
  static constexpr int16_t HEADER_CONTENT_RIGHT = 312;

  static constexpr int16_t HEADER_CENTER_X =
    (
      HEADER_CONTENT_LEFT +
      HEADER_CONTENT_RIGHT
    ) / 2;

  static constexpr int16_t HEADER_TITLE_Y = 18;
  static constexpr int16_t HEADER_IP_Y = 41;

  static constexpr int16_t CONTROL_LEFT_X = 16;
  static constexpr int16_t CONTROL_RIGHT_X = 240;

  static constexpr int16_t CONTROL_BUTTON_W = 64;
  static constexpr int16_t CONTROL_BUTTON_H = 34;

  static constexpr int16_t BRI_BUTTON_Y = 82;
  static constexpr int16_t FX_BUTTON_Y = 138;

  static constexpr int16_t COLOR_BUTTON_X = 16;
  static constexpr int16_t COLOR_BUTTON_Y = 188;
  static constexpr int16_t COLOR_BUTTON_W = 288;
  static constexpr int16_t COLOR_BUTTON_H = 40;

  static constexpr int16_t BACK_BUTTON_X = 268;
  static constexpr int16_t BACK_BUTTON_Y = 8;
  static constexpr int16_t BACK_BUTTON_W = 44;
  static constexpr int16_t BACK_BUTTON_H = 44;

  static constexpr int16_t COLOR_PREVIEW_X = 92;
  static constexpr int16_t COLOR_PREVIEW_Y = 68;
  static constexpr int16_t COLOR_PREVIEW_W = 136;
  static constexpr int16_t COLOR_PREVIEW_H = 36;

  static constexpr int16_t HUE_BUTTON_Y = 151;

  static constexpr int16_t SATURATION_LABEL_Y = 198;
  static constexpr int16_t SATURATION_BUTTON_Y = 204;

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
  // Phase 8.3.1 - Effect behavior
  //
  // Short:
  //   +/- 1 Effect
  //
  // Long:
  //   start after 400ms
  //   then +/- 1 Effect every 250ms
  //
  // Effect wraps around first/last.
  // =========================================================

  static constexpr unsigned long EFFECT_LONG_PRESS_MS = 400;
  static constexpr unsigned long EFFECT_REPEAT_MS = 250;

  // =========================================================
  // Hue behavior
  // =========================================================

  static constexpr unsigned long HUE_LONG_PRESS_MS = 400;
  static constexpr unsigned long HUE_REPEAT_MS = 80;

  static constexpr int HUE_SHORT_STEP = 1;
  static constexpr int HUE_LONG_STEP = 5;

  // =========================================================
  // Saturation behavior
  // =========================================================

  static constexpr unsigned long SATURATION_LONG_PRESS_MS = 400;
  static constexpr unsigned long SATURATION_REPEAT_MS = 80;

  static constexpr int SATURATION_SHORT_STEP = 1;
  static constexpr int SATURATION_LONG_STEP = 5;

  // =========================================================
  // Display brightness
  // =========================================================

  void setDisplayBrightness(
    uint8_t value
  )
  {
    currentDisplayBrightness =
      value;

    display.setBrightness(
      value
    );
  }

  // =========================================================
  // Generic non-blocking fade
  // =========================================================

  bool updateFade(
    uint8_t targetBrightness,
    unsigned long now,
    unsigned long& lastFadeStep,
    unsigned long fadeInterval,
    uint8_t fadeStep
  )
  {
    if (
      currentDisplayBrightness ==
      targetBrightness
    )
    {
      return true;
    }

    if (
      now -
      lastFadeStep <
      fadeInterval
    )
    {
      return false;
    }

    lastFadeStep =
      now;

    if (
      currentDisplayBrightness <
      targetBrightness
    )
    {
      int nextValue =
        currentDisplayBrightness +
        fadeStep;

      if (
        nextValue >
        targetBrightness
      )
      {
        nextValue =
          targetBrightness;
      }

      setDisplayBrightness(
        (uint8_t)nextValue
      );
    }
    else
    {
      int nextValue =
        currentDisplayBrightness -
        fadeStep;

      if (
        nextValue <
        targetBrightness
      )
      {
        nextValue =
          targetBrightness;
      }

      setDisplayBrightness(
        (uint8_t)nextValue
      );
    }

    return
      (
        currentDisplayBrightness ==
        targetBrightness
      );
  }

  // =========================================================
  // Startup logo
  // =========================================================

  void drawStartupBase()
  {
    display.fillScreen(
      TFT_BLACK
    );

    bool logoResult =
      display.drawPng(
        CORES3_WLED_LOGO_PNG,
        CORES3_WLED_LOGO_PNG_LEN,
        8,
        20
      );

    if (!logoResult)
    {
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
        68
      );

      Serial.println(
        F(
          "[CoreS3_Display] "
          "WARNING: startup PNG draw failed"
        )
      );
    }
  }

  // =========================================================
  // Startup connecting
  // =========================================================

  void drawStartupConnectingStatus()
  {
    display.fillRect(
      0,
      125,
      screenWidth,
      100,
      TFT_BLACK
    );

    char dots[5];

    dots[0] = '\0';

    for (
      uint8_t i = 0;
      i < startupDotCount;
      i++
    )
    {
      strcat(
        dots,
        "."
      );
    }

    char statusText[32];

    snprintf(
      statusText,
      sizeof(statusText),
      "Wi-Fi Connecting%s",
      dots
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
      statusText,
      screenWidth / 2,
      153
    );

    display.setTextSize(
      1
    );

    display.setTextColor(
      TFT_DARKGREY,
      TFT_BLACK
    );

    display.drawString(
      "Starting WLED...",
      screenWidth / 2,
      185
    );
  }

  // =========================================================
  // Startup connected
  // =========================================================

  void drawStartupConnectedStatus(
    const String& ipAddress
  )
  {
    display.fillRect(
      0,
      125,
      screenWidth,
      100,
      TFT_BLACK
    );

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_GREEN,
      TFT_BLACK
    );

    display.setTextSize(
      2
    );

    display.drawString(
      "Wi-Fi Connected",
      screenWidth / 2,
      150
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(
      1
    );

    display.drawString(
      ipAddress,
      screenWidth / 2,
      181
    );
  }

  // =========================================================
  // Startup sequence
  // =========================================================

  void handleStartupSequence()
  {
    if (
      startupState ==
      STARTUP_DONE
    )
    {
      return;
    }

    unsigned long now =
      millis();

    // -------------------------------------------------------
    // Fade In
    // -------------------------------------------------------

    if (
      startupState ==
      STARTUP_FADE_IN
    )
    {
      if (
        updateFade(
          DISPLAY_NORMAL_BRIGHTNESS,
          now,
          startupLastFadeStep,
          STARTUP_FADE_INTERVAL_MS,
          STARTUP_FADE_STEP
        )
      )
      {
        startupState =
          STARTUP_WAIT_WIFI;

        startupStateStart =
          now;

        startupLastDotsUpdate =
          now;
      }

      return;
    }

    // -------------------------------------------------------
    // Wi-Fi wait
    // -------------------------------------------------------

    if (
      startupState ==
      STARTUP_WAIT_WIFI
    )
    {
      if (
        now -
        startupLastDotsUpdate >=
        STARTUP_DOTS_INTERVAL_MS
      )
      {
        startupLastDotsUpdate =
          now;

        startupDotCount++;

        if (
          startupDotCount >
          3
        )
        {
          startupDotCount =
            0;
        }

        drawStartupConnectingStatus();
      }

      if (
        WiFi.status() ==
        WL_CONNECTED
      )
      {
        startupIPAddress =
          WiFi.localIP().toString();

        drawStartupConnectedStatus(
          startupIPAddress
        );

        startupState =
          STARTUP_CONNECTED_HOLD;

        startupStateStart =
          now;
      }

      return;
    }

    // -------------------------------------------------------
    // Connected hold
    // -------------------------------------------------------

    if (
      startupState ==
      STARTUP_CONNECTED_HOLD
    )
    {
      if (
        now -
        startupStateStart >=
        STARTUP_CONNECTED_HOLD_MS
      )
      {
        startupState =
          STARTUP_FADE_OUT;

        startupLastFadeStep =
          now;
      }

      return;
    }

    // -------------------------------------------------------
    // Startup Fade Out
    // -------------------------------------------------------

    if (
      startupState ==
      STARTUP_FADE_OUT
    )
    {
      if (
        updateFade(
          0,
          now,
          startupLastFadeStep,
          STARTUP_FADE_INTERVAL_MS,
          STARTUP_FADE_STEP
        )
      )
      {
        drawMainScreen(
          startupIPAddress
        );

        setDisplayBrightness(
          0
        );

        startupState =
          STARTUP_MAIN_FADE_IN;

        startupLastFadeStep =
          now;
      }

      return;
    }

    // -------------------------------------------------------
    // MAIN Fade In
    // -------------------------------------------------------

    if (
      startupState ==
      STARTUP_MAIN_FADE_IN
    )
    {
      if (
        updateFade(
          DISPLAY_NORMAL_BRIGHTNESS,
          now,
          startupLastFadeStep,
          STARTUP_FADE_INTERVAL_MS,
          STARTUP_FADE_STEP
        )
      )
      {
        startupState =
          STARTUP_DONE;

        lastWiFiConnected =
          true;

        lastIPAddress =
          startupIPAddress;

        readyScreenShown =
          true;

        connectingScreenShown =
          false;

        lastUserActivityMs =
          now;

        displayPowerState =
          DISPLAY_POWER_ACTIVE;

        Serial.println(
          F(
            "[CoreS3_Display] "
            "Startup animation complete"
          )
        );
      }

      return;
    }
  }

  // =========================================================
  // Wake touch polling
  // =========================================================

  bool pollWakeTouch(
    unsigned long now
  )
  {
    if (
      now -
      wakeTouchLastPoll <
      TOUCH_POLL_MS
    )
    {
      return
        wakeTouchState;
    }

    wakeTouchLastPoll =
      now;

    int16_t x = -1;
    int16_t y = -1;

    wakeTouchState =
      (
        display.getTouch(
          &x,
          &y
        ) > 0
      );

    return
      wakeTouchState;
  }

  // =========================================================
  // Redraw latest page before Wake
  // =========================================================

  void redrawCurrentPageForWake()
  {
    if (
      WiFi.status() !=
      WL_CONNECTED
    )
    {
      drawConnectingScreen();

      return;
    }

    if (
      currentPage ==
      SCREEN_COLOR
    )
    {
      drawColorScreen();
    }
    else
    {
      drawMainScreen(
        WiFi.localIP().toString()
      );
    }
  }

  // =========================================================
  // Start display sleep
  // =========================================================

  void beginDisplaySleep(
    unsigned long now
  )
  {
    Serial.println(
      F(
        "[CoreS3_Display] "
        "Display sleep start"
      )
    );

    resetTouchGesture();

    displayPowerState =
      DISPLAY_POWER_SLEEP_FADE_OUT;

    displayFadeLastStep =
      now;

    wakeTouchLastPoll =
      0;

    wakeTouchState =
      false;

    wakeReleaseCandidate =
      0;
  }

  // =========================================================
  // Start display wake
  // =========================================================

  void beginDisplayWake(
    unsigned long now
  )
  {
    Serial.println(
      F(
        "[CoreS3_Display] "
        "Display wake start"
      )
    );

    resetTouchGesture();

    setDisplayBrightness(
      0
    );

    redrawCurrentPageForWake();

    setDisplayBrightness(
      0
    );

    displayPowerState =
      DISPLAY_POWER_WAKE_FADE_IN;

    displayFadeLastStep =
      now;

    wakeReleaseCandidate =
      0;

    lastUserActivityMs =
      now;
  }

  // =========================================================
  // Display power management
  // =========================================================

  bool handleDisplayPowerManagement()
  {
    unsigned long now =
      millis();

    // =======================================================
    // ACTIVE
    // =======================================================

    if (
      displayPowerState ==
      DISPLAY_POWER_ACTIVE
    )
    {
      if (
        !touchActive &&
        now -
        lastUserActivityMs >=
        DISPLAY_SLEEP_TIMEOUT_MS
      )
      {
        beginDisplaySleep(
          now
        );

        return true;
      }

      return false;
    }

    // =======================================================
    // Fade Out
    // =======================================================

    if (
      displayPowerState ==
      DISPLAY_POWER_SLEEP_FADE_OUT
    )
    {
      if (
        pollWakeTouch(
          now
        )
      )
      {
        beginDisplayWake(
          now
        );

        return true;
      }

      if (
        updateFade(
          0,
          now,
          displayFadeLastStep,
          DISPLAY_FADE_INTERVAL_MS,
          DISPLAY_FADE_STEP
        )
      )
      {
        displayPowerState =
          DISPLAY_POWER_SLEEPING;

        setDisplayBrightness(
          0
        );

        Serial.println(
          F(
            "[CoreS3_Display] "
            "Display sleeping"
          )
        );
      }

      return true;
    }

    // =======================================================
    // Sleeping
    // =======================================================

    if (
      displayPowerState ==
      DISPLAY_POWER_SLEEPING
    )
    {
      if (
        pollWakeTouch(
          now
        )
      )
      {
        beginDisplayWake(
          now
        );
      }

      return true;
    }

    // =======================================================
    // Wake Fade In
    // =======================================================

    if (
      displayPowerState ==
      DISPLAY_POWER_WAKE_FADE_IN
    )
    {
      pollWakeTouch(
        now
      );

      if (
        updateFade(
          DISPLAY_NORMAL_BRIGHTNESS,
          now,
          displayFadeLastStep,
          DISPLAY_FADE_INTERVAL_MS,
          DISPLAY_FADE_STEP
        )
      )
      {
        displayPowerState =
          DISPLAY_POWER_WAKE_WAIT_RELEASE;

        wakeReleaseCandidate =
          0;

        Serial.println(
          F(
            "[CoreS3_Display] "
            "Display wake fade complete"
          )
        );
      }

      return true;
    }

    // =======================================================
    // Wait Wake touch release
    // =======================================================

    if (
      displayPowerState ==
      DISPLAY_POWER_WAKE_WAIT_RELEASE
    )
    {
      bool touching =
        pollWakeTouch(
          now
        );

      if (touching)
      {
        wakeReleaseCandidate =
          0;

        return true;
      }

      if (
        wakeReleaseCandidate ==
        0
      )
      {
        wakeReleaseCandidate =
          now;

        return true;
      }

      if (
        now -
        wakeReleaseCandidate >=
        TOUCH_RELEASE_CONFIRM_MS
      )
      {
        displayPowerState =
          DISPLAY_POWER_ACTIVE;

        lastUserActivityMs =
          now;

        wakeReleaseCandidate =
          0;

        wakeTouchState =
          false;

        resetTouchGesture();

        Serial.println(
          F(
            "[CoreS3_Display] "
            "Display active"
          )
        );
      }

      return true;
    }

    return false;
  }

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
  // RGB -> Hue
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
  // RGB -> Saturation
  // =========================================================

  uint8_t getSaturationFromColor(
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
      hsv.s;
  }

  // =========================================================
  // Logical HSV sync
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

    logicalSaturationValue =
      logicalColorHsv.s;

    logicalWhiteValue =
      rgb.w;

    logicalColorHsvValid =
      true;

    lastHueValue =
      logicalHueValue;

    lastSaturationValue =
      logicalSaturationValue;
  }

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

  uint8_t getDisplayedSaturation()
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
        logicalSaturationValue;
    }

    return
      getSaturationFromColor(
        currentColor
      );
  }

  // =========================================================
  // Wi-Fi reconnect screen
  // =========================================================

  void drawConnectingScreen()
  {
    drawStartupBase();

    display.fillRect(
      0,
      125,
      screenWidth,
      100,
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
      "Wi-Fi Reconnecting...",
      screenWidth / 2,
      153
    );

    display.setTextSize(
      1
    );

    display.setTextColor(
      TFT_DARKGREY,
      TFT_BLACK
    );

    display.drawString(
      "WLED is running",
      screenWidth / 2,
      185
    );

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
  // Triangle button
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

    display.setTextSize(
      2
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
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
  // COLOR button
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

    uint16_t previewColor =
      rgbTo565(
        R(color),
        G(color),
        B(color)
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

    static constexpr int16_t SWATCH_X = 110;
    static constexpr int16_t SWATCH_W = 26;
    static constexpr int16_t SWATCH_H = 24;

    int16_t swatchY =
      COLOR_BUTTON_Y +
      (
        (
          COLOR_BUTTON_H -
          SWATCH_H
        ) / 2
      );

    display.fillRect(
      SWATCH_X,
      swatchY,
      SWATCH_W,
      SWATCH_H,
      previewColor
    );

    display.drawRect(
      SWATCH_X,
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
      180,
      COLOR_BUTTON_Y +
        (COLOR_BUTTON_H / 2)
    );

    colorButtonVisualPressed =
      pressed;
  }

  // =========================================================
  // Back
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
  // Color details
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

    uint16_t previewColor =
      rgbTo565(
        R(color),
        G(color),
        B(color)
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
      R(color),
      G(color),
      B(color)
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
  // Hue
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
  // Saturation
  // =========================================================

  void drawSaturation(
    uint8_t saturationValue,
    TouchTarget pressedTarget =
      TOUCH_TARGET_NONE
  )
  {
    display.fillRect(
      0,
      194,
      screenWidth,
      46,
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
      "Saturation",
      screenWidth / 2,
      SATURATION_LABEL_Y
    );

    drawTriangleButton(
      CONTROL_LEFT_X,
      SATURATION_BUTTON_Y,
      false,
      pressedTarget ==
        TOUCH_TARGET_SATURATION_DOWN
    );

    drawTriangleButton(
      CONTROL_RIGHT_X,
      SATURATION_BUTTON_Y,
      true,
      pressedTarget ==
        TOUCH_TARGET_SATURATION_UP
    );

    char valueText[8];

    snprintf(
      valueText,
      sizeof(valueText),
      "%u",
      saturationValue
    );

    display.setTextSize(
      2
    );

    display.drawString(
      valueText,
      screenWidth / 2,
      SATURATION_BUTTON_Y +
        (CONTROL_BUTTON_H / 2)
    );
  }

  // =========================================================
  // MAIN
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
  // COLOR
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

    drawSaturation(
      logicalSaturationValue,
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

    lastSaturationValue =
      logicalSaturationValue;
  }

  // =========================================================
  // Hit tests
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

  bool isSaturationDownTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      CONTROL_LEFT_X,
      SATURATION_BUTTON_Y,
      CONTROL_BUTTON_W,
      CONTROL_BUTTON_H
    );
  }

  bool isSaturationUpTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      CONTROL_RIGHT_X,
      SATURATION_BUTTON_Y,
      CONTROL_BUTTON_W,
      CONTROL_BUTTON_H
    );
  }

  // =========================================================
  // Begin Hue
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
  }

  // =========================================================
  // Begin Saturation
  // =========================================================

  void beginSaturationEdit()
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

    saturationEditHsv =
      logicalColorHsv;

    saturationEditValue =
      logicalSaturationValue;

    saturationEditWhite =
      logicalWhiteValue;

    saturationEditValid =
      true;
  }

  // =========================================================
  // Reset touch
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

    lastTouchInsideSaturation =
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

    saturationButtonVisualPressed =
      false;

    brightnessLongPressActive =
      false;

    effectLongPressActive =
      false;

    hueLongPressActive =
      false;

    saturationLongPressActive =
      false;

    hueEditValid =
      false;

    saturationEditValid =
      false;

    touchReleaseCandidate =
      0;

    controlPressStartTime =
      0;

    lastBrightnessRepeat =
      0;

    effectPressStartTime =
      0;

    lastEffectRepeat =
      0;

    huePressStartTime =
      0;

    lastHueRepeat =
      0;

    saturationPressStartTime =
      0;

    lastSaturationRepeat =
      0;

    lastTouchX =
      -1;

    lastTouchY =
      -1;
  }

  // =========================================================
  // Power action
  // =========================================================

  void toggleLedPowerFromTouch()
  {
    toggleOnOff();

    stateUpdated(
      CALL_MODE_BUTTON
    );

    lastLedState =
      -1;

    lastBrightnessValue =
      -1;
  }

  // =========================================================
  // Brightness actions
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

    return true;
  }

  void applyBrightnessStep(
    int step
  )
  {
    int newValue =
      constrain(
        (int)bri +
        step,
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
  // Effect actions
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

    int newMode =
      mainSegment.mode +
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
      mainSegment.mode
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
        touchTarget
      );
    }

    lastEffectMode =
      mainSegment.mode;
  }

  // =========================================================
  // Phase 8.3.1
  // Effect long press step
  // =========================================================

  void effectLongPressStep(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_EFFECT_PREV
    )
    {
      applyEffectStep(
        -1
      );
    }
    else if (
      target ==
      TOUCH_TARGET_EFFECT_NEXT
    )
    {
      applyEffectStep(
        1
      );
    }
  }

  // =========================================================
  // Hue actions
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

    hueEditValue =
      newHue;

    hueEditHsv.h =
      ((uint16_t)newHue) <<
      8;

    logicalHueValue =
      newHue;

    logicalColorHsv =
      hueEditHsv;

    logicalSaturationValue =
      logicalColorHsv.s;

    logicalWhiteValue =
      hueEditWhite;

    logicalColorHsvValid =
      true;

    CRGBW newRgb;

    hsv2rgb_spectrum(
      logicalColorHsv,
      newRgb
    );

    newRgb.w =
      logicalWhiteValue;

    uint32_t newColor =
      newRgb.color32;

    Segment& mainSegment =
      strip.getMainSegment();

    if (
      newColor !=
      mainSegment.colors[0]
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

      drawSaturation(
        logicalSaturationValue,
        TOUCH_TARGET_NONE
      );
    }

    lastPrimaryColor =
      newColor;

    lastPrimaryColorValid =
      true;

    lastHueValue =
      logicalHueValue;

    lastSaturationValue =
      logicalSaturationValue;

    return true;
  }

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
  // Saturation actions
  // =========================================================

  bool applySaturationValue(
    uint8_t newSaturation
  )
  {
    if (
      strip.getSegmentsNum() ==
      0
    )
    {
      return false;
    }

    if (!saturationEditValid)
    {
      beginSaturationEdit();
    }

    if (!saturationEditValid)
    {
      return false;
    }

    saturationEditValue =
      newSaturation;

    saturationEditHsv.s =
      newSaturation;

    logicalSaturationValue =
      newSaturation;

    logicalColorHsv =
      saturationEditHsv;

    logicalHueValue =
      (uint8_t)(
        logicalColorHsv.h >>
        8
      );

    logicalWhiteValue =
      saturationEditWhite;

    logicalColorHsvValid =
      true;

    CRGBW newRgb;

    hsv2rgb_spectrum(
      logicalColorHsv,
      newRgb
    );

    newRgb.w =
      logicalWhiteValue;

    uint32_t newColor =
      newRgb.color32;

    Segment& mainSegment =
      strip.getMainSegment();

    if (
      newColor !=
      mainSegment.colors[0]
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
        TOUCH_TARGET_NONE
      );

      drawSaturation(
        logicalSaturationValue,
        touchTarget
      );
    }

    lastPrimaryColor =
      newColor;

    lastPrimaryColorValid =
      true;

    lastHueValue =
      logicalHueValue;

    lastSaturationValue =
      logicalSaturationValue;

    return true;
  }

  void applySaturationStep(
    int step
  )
  {
    if (!saturationEditValid)
    {
      beginSaturationEdit();
    }

    if (!saturationEditValid)
    {
      return;
    }

    int newValue =
      constrain(
        (int)logicalSaturationValue +
        step,
        0,
        255
      );

    if (
      newValue ==
      logicalSaturationValue
    )
    {
      return;
    }

    applySaturationValue(
      (uint8_t)newValue
    );
  }

  void saturationShortPress(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_SATURATION_DOWN
    )
    {
      applySaturationStep(
        -SATURATION_SHORT_STEP
      );
    }
    else if (
      target ==
      TOUCH_TARGET_SATURATION_UP
    )
    {
      applySaturationStep(
        SATURATION_SHORT_STEP
      );
    }
  }

  void saturationLongPressStep(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_SATURATION_DOWN
    )
    {
      applySaturationStep(
        -SATURATION_LONG_STEP
      );
    }
    else if (
      target ==
      TOUCH_TARGET_SATURATION_UP
    )
    {
      applySaturationStep(
        SATURATION_LONG_STEP
      );
    }
  }

  // =========================================================
  // Selected button helpers
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

  bool isInsideSelectedSaturationButton(
    int16_t x,
    int16_t y
  )
  {
    if (
      touchTarget ==
      TOUCH_TARGET_SATURATION_DOWN
    )
    {
      return
        isSaturationDownTouched(
          x,
          y
        );
    }

    if (
      touchTarget ==
      TOUCH_TARGET_SATURATION_UP
    )
    {
      return
        isSaturationUpTouched(
          x,
          y
        );
    }

    return false;
  }

  // =========================================================
  // Normal touch processing
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
    // Touching
    // =======================================================

    if (touching)
    {
      // Any normal touch resets the 30 second sleep timer.
      lastUserActivityMs =
        now;

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

      bool insideBrightnessDown = false;
      bool insideBrightnessUp = false;
      bool insideEffectPrev = false;
      bool insideEffectNext = false;
      bool insideColor = false;

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

      bool insideBack = false;
      bool insideHueDown = false;
      bool insideHueUp = false;
      bool insideSaturationDown = false;
      bool insideSaturationUp = false;

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

        insideSaturationDown =
          isSaturationDownTouched(
            touchX,
            touchY
          );

        insideSaturationUp =
          isSaturationUpTouched(
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

        effectLongPressActive =
          false;

        hueLongPressActive =
          false;

        saturationLongPressActive =
          false;
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
          if (insideBrightnessDown)
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

          else if (insideBrightnessUp)
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

          // =================================================
          // Phase 8.3.1
          // Effect PREV touch start
          // =================================================

          else if (insideEffectPrev)
          {
            touchTarget =
              TOUCH_TARGET_EFFECT_PREV;

            lastTouchInsideEffect =
              true;

            effectPressStartTime =
              now;

            lastEffectRepeat =
              now;

            effectLongPressActive =
              false;
          }

          // =================================================
          // Phase 8.3.1
          // Effect NEXT touch start
          // =================================================

          else if (insideEffectNext)
          {
            touchTarget =
              TOUCH_TARGET_EFFECT_NEXT;

            lastTouchInsideEffect =
              true;

            effectPressStartTime =
              now;

            lastEffectRepeat =
              now;

            effectLongPressActive =
              false;
          }

          else if (insideColor)
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
          if (insideBack)
          {
            touchTarget =
              TOUCH_TARGET_BACK;

            lastTouchInsideBack =
              true;
          }

          else if (insideHueDown)
          {
            touchTarget =
              TOUCH_TARGET_HUE_DOWN;

            lastTouchInsideHue =
              true;

            huePressStartTime =
              now;

            lastHueRepeat =
              now;

            beginHueEdit();
          }

          else if (insideHueUp)
          {
            touchTarget =
              TOUCH_TARGET_HUE_UP;

            lastTouchInsideHue =
              true;

            huePressStartTime =
              now;

            lastHueRepeat =
              now;

            beginHueEdit();
          }

          else if (insideSaturationDown)
          {
            touchTarget =
              TOUCH_TARGET_SATURATION_DOWN;

            lastTouchInsideSaturation =
              true;

            saturationPressStartTime =
              now;

            lastSaturationRepeat =
              now;

            beginSaturationEdit();
          }

          else if (insideSaturationUp)
          {
            touchTarget =
              TOUCH_TARGET_SATURATION_UP;

            lastTouchInsideSaturation =
              true;

            saturationPressStartTime =
              now;

            lastSaturationRepeat =
              now;

            beginSaturationEdit();
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
      // Phase 8.3.1
      // Effect short / long press
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

        // Finger moved outside the visible Effect button.
        // Do not advance Effect while outside.
        if (!insideSelectedButton)
        {
          return;
        }

        // ---------------------------------------------------
        // Long press starts after 400 ms.
        //
        // Perform first step immediately at 400 ms.
        // ---------------------------------------------------

        if (
          !effectLongPressActive &&
          now -
          effectPressStartTime >=
          EFFECT_LONG_PRESS_MS
        )
        {
          effectLongPressActive =
            true;

          lastEffectRepeat =
            now;

          effectLongPressStep(
            touchTarget
          );

          return;
        }

        // ---------------------------------------------------
        // Once long press is active:
        // advance one Effect every 250 ms.
        // ---------------------------------------------------

        if (
          effectLongPressActive &&
          now -
          lastEffectRepeat >=
          EFFECT_REPEAT_MS
        )
        {
          lastEffectRepeat =
            now;

          effectLongPressStep(
            touchTarget
          );
        }

        return;
      }

      // =====================================================
      // COLOR open
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

      // =====================================================
      // Saturation
      // =====================================================

      if (
        touchTarget ==
          TOUCH_TARGET_SATURATION_DOWN ||
        touchTarget ==
          TOUCH_TARGET_SATURATION_UP
      )
      {
        bool insideSelectedButton =
          isInsideSelectedSaturationButton(
            touchX,
            touchY
          );

        lastTouchInsideSaturation =
          insideSelectedButton;

        if (
          insideSelectedButton !=
          saturationButtonVisualPressed
        )
        {
          drawSaturation(
            logicalSaturationValue,
            insideSelectedButton
              ? touchTarget
              : TOUCH_TARGET_NONE
          );

          saturationButtonVisualPressed =
            insideSelectedButton;
        }

        if (!insideSelectedButton)
        {
          return;
        }

        if (
          !saturationLongPressActive &&
          now -
          saturationPressStartTime >=
          SATURATION_LONG_PRESS_MS
        )
        {
          saturationLongPressActive =
            true;

          lastSaturationRepeat =
            now;

          saturationLongPressStep(
            touchTarget
          );

          return;
        }

        if (
          saturationLongPressActive &&
          now -
          lastSaturationRepeat >=
          SATURATION_REPEAT_MS
        )
        {
          lastSaturationRepeat =
            now;

          saturationLongPressStep(
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

    // =======================================================
    // Phase 8.3.1
    //
    // Remember whether Effect became a long press.
    // If true, release must NOT advance another Effect.
    // =======================================================

    bool wasEffectLongPress =
      effectLongPressActive;

    bool wasHueLongPress =
      hueLongPressActive;

    bool wasSaturationLongPress =
      saturationLongPressActive;

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

    // =======================================================
    // Phase 8.3.1
    //
    // Only execute Effect on release when it was a SHORT press.
    // =======================================================

    bool executeEffectAction =
      (
        releasedTarget ==
          TOUCH_TARGET_EFFECT_PREV ||
        releasedTarget ==
          TOUCH_TARGET_EFFECT_NEXT
      ) &&
      lastTouchInsideEffect &&
      !wasEffectLongPress;

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

    bool executeSaturationShortPress =
      (
        releasedTarget ==
          TOUCH_TARGET_SATURATION_DOWN ||
        releasedTarget ==
          TOUCH_TARGET_SATURATION_UP
      ) &&
      lastTouchInsideSaturation &&
      !wasSaturationLongPress;

    // -------------------------------------------------------
    // Restore Power visual
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
    // Restore Brightness visual
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
    // Restore Effect visual
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
    // Restore COLOR visual
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
    // Restore Back visual
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
    // Restore Hue visual
    // -------------------------------------------------------

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
    // Restore Saturation visual
    // -------------------------------------------------------

    if (
      (
        releasedTarget ==
          TOUCH_TARGET_SATURATION_DOWN ||
        releasedTarget ==
          TOUCH_TARGET_SATURATION_UP
      ) &&
      saturationButtonVisualPressed
    )
    {
      drawSaturation(
        logicalSaturationValue,
        TOUCH_TARGET_NONE
      );
    }

    // =======================================================
    // Save Hue edit state
    // =======================================================

    bool savedHueEditValid =
      hueEditValid;

    CHSV32 savedHueEditHsv =
      hueEditHsv;

    uint8_t savedHueEditValue =
      hueEditValue;

    uint8_t savedHueEditWhite =
      hueEditWhite;

    // =======================================================
    // Save Saturation edit state
    // =======================================================

    bool savedSaturationEditValid =
      saturationEditValid;

    CHSV32 savedSaturationEditHsv =
      saturationEditHsv;

    uint8_t savedSaturationEditValue =
      saturationEditValue;

    uint8_t savedSaturationEditWhite =
      saturationEditWhite;

    // =======================================================
    // Reset gesture
    // =======================================================

    resetTouchGesture();

    // =======================================================
    // Restore edit state for short-release action
    // =======================================================

    hueEditValid =
      savedHueEditValid;

    hueEditHsv =
      savedHueEditHsv;

    hueEditValue =
      savedHueEditValue;

    hueEditWhite =
      savedHueEditWhite;

    saturationEditValid =
      savedSaturationEditValid;

    saturationEditHsv =
      savedSaturationEditHsv;

    saturationEditValue =
      savedSaturationEditValue;

    saturationEditWhite =
      savedSaturationEditWhite;

    // =======================================================
    // Execute Power
    // =======================================================

    if (executePowerAction)
    {
      lastTouchAction =
        now;

      toggleLedPowerFromTouch();

      hueEditValid =
        false;

      saturationEditValid =
        false;

      return;
    }

    // =======================================================
    // Execute Brightness short press
    // =======================================================

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

      saturationEditValid =
        false;

      return;
    }

    // =======================================================
    // Execute Effect SHORT press only
    //
    // Long press already applied all Effect changes while
    // the finger was held, so release does nothing.
    // =======================================================

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

      // Restore unpressed visual after short press.
      drawEffect(
        getCurrentEffectMode(),
        TOUCH_TARGET_NONE
      );

      hueEditValid =
        false;

      saturationEditValid =
        false;

      return;
    }

    // =======================================================
    // Open COLOR
    // =======================================================

    if (executeColorOpen)
    {
      hueEditValid =
        false;

      saturationEditValid =
        false;

      drawColorScreen();

      return;
    }

    // =======================================================
    // Back
    // =======================================================

    if (executeBack)
    {
      hueEditValid =
        false;

      saturationEditValid =
        false;

      drawMainScreen(
        WiFi.localIP().toString()
      );

      return;
    }

    // =======================================================
    // Hue short press
    // =======================================================

    if (executeHueShortPress)
    {
      hueShortPress(
        releasedTarget
      );

      drawHue(
        logicalHueValue,
        TOUCH_TARGET_NONE
      );

      drawSaturation(
        logicalSaturationValue,
        TOUCH_TARGET_NONE
      );

      hueEditValid =
        false;

      saturationEditValid =
        false;

      return;
    }

    // =======================================================
    // Saturation short press
    // =======================================================

    if (executeSaturationShortPress)
    {
      saturationShortPress(
        releasedTarget
      );

      drawHue(
        logicalHueValue,
        TOUCH_TARGET_NONE
      );

      drawSaturation(
        logicalSaturationValue,
        TOUCH_TARGET_NONE
      );

      hueEditValid =
        false;

      saturationEditValid =
        false;

      return;
    }

    // Long press actions already completed while held.
    hueEditValid =
      false;

    saturationEditValid =
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
        "Phase 8.3.1 start"
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

    // -------------------------------------------------------
    // Startup begins completely dark
    // -------------------------------------------------------

    setDisplayBrightness(
      0
    );

    drawStartupBase();

    startupDotCount =
      3;

    drawStartupConnectingStatus();

    startupState =
      STARTUP_FADE_IN;

    startupStateStart =
      millis();

    startupLastFadeStep =
      startupStateStart;

    startupLastDotsUpdate =
      startupStateStart;

    displayPowerState =
      DISPLAY_POWER_ACTIVE;

    lastUserActivityMs =
      startupStateStart;

    displayReady =
      true;

    Serial.println(
      F(
        "[CoreS3_Display] "
        "Phase 8.3.1 setup complete"
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

    // =======================================================
    // Startup
    // =======================================================

    if (
      startupState !=
      STARTUP_DONE
    )
    {
      handleStartupSequence();

      return;
    }

    // =======================================================
    // ACTIVE display:
    // process normal touch first.
    // =======================================================

    if (
      displayPowerState ==
      DISPLAY_POWER_ACTIVE
    )
    {
      handleTouch();
    }

    // =======================================================
    // Sleep / Wake
    // =======================================================

    if (
      handleDisplayPowerManagement()
    )
    {
      return;
    }

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

    String currentIPAddress =
      WiFi.localIP().toString();

    // =======================================================
    // Reconnected
    // =======================================================

    if (
      !lastWiFiConnected ||
      !readyScreenShown
    )
    {
      drawMainScreen(
        currentIPAddress
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

    bool saturationTouchActive =
      (
        touchTarget ==
          TOUCH_TARGET_SATURATION_DOWN ||
        touchTarget ==
          TOUCH_TARGET_SATURATION_UP
      );

    bool colorControlTouchActive =
      (
        hueTouchActive ||
        saturationTouchActive
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

      // -----------------------------------------------------
      // Don't overwrite pressed Effect visual while finger
      // is on the Effect button.
      // -----------------------------------------------------

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
      if (
        primaryColorChanged &&
        !colorControlTouchActive
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

        drawSaturation(
          logicalSaturationValue,
          TOUCH_TARGET_NONE
        );

        lastHueValue =
          logicalHueValue;

        lastSaturationValue =
          logicalSaturationValue;

        primaryColorChangeHandled =
          true;
      }
    }

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
      char effectName[64];

      getEffectName(
        strip.getMainSegment().mode,
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
    // Saturation
    // -------------------------------------------------------

    JsonArray saturationInfo =
      user.createNestedArray(
        "CoreS3 Display Saturation"
      );

    if (
      strip.getSegmentsNum() >
      0
    )
    {
      saturationInfo.add(
        getDisplayedSaturation()
      );
    }
    else
    {
      saturationInfo.add(
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

    // -------------------------------------------------------
    // Startup
    // -------------------------------------------------------

    JsonArray startupInfo =
      user.createNestedArray(
        "CoreS3 Display Startup"
      );

    startupInfo.add(
      startupState ==
        STARTUP_DONE
        ? "DONE"
        : "ACTIVE"
    );

    // -------------------------------------------------------
    // Display Power State
    // -------------------------------------------------------

    JsonArray powerStateInfo =
      user.createNestedArray(
        "CoreS3 Display Power State"
      );

    switch (
      displayPowerState
    )
    {
      case DISPLAY_POWER_ACTIVE:
        powerStateInfo.add(
          "ACTIVE"
        );
        break;

      case DISPLAY_POWER_SLEEP_FADE_OUT:
        powerStateInfo.add(
          "SLEEP FADE OUT"
        );
        break;

      case DISPLAY_POWER_SLEEPING:
        powerStateInfo.add(
          "SLEEPING"
        );
        break;

      case DISPLAY_POWER_WAKE_FADE_IN:
        powerStateInfo.add(
          "WAKE FADE IN"
        );
        break;

      case DISPLAY_POWER_WAKE_WAIT_RELEASE:
        powerStateInfo.add(
          "WAKE WAIT RELEASE"
        );
        break;
    }

    // -------------------------------------------------------
    // Sleep timeout
    // -------------------------------------------------------

    JsonArray sleepInfo =
      user.createNestedArray(
        "CoreS3 Display Sleep"
      );

    sleepInfo.add(
      "30 sec"
    );

    // -------------------------------------------------------
    // Phase
    // -------------------------------------------------------

    JsonArray phaseInfo =
      user.createNestedArray(
        "CoreS3 Display Phase"
      );

    phaseInfo.add(
      "8.3.1"
    );
  }
};

static CoreS3DisplayUsermod coreS3DisplayUsermod;

REGISTER_USERMOD(coreS3DisplayUsermod);
