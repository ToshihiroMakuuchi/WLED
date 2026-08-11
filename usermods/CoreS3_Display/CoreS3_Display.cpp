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

  // =========================================================
  // Touch targets
  // =========================================================

  enum TouchTarget : uint8_t
  {
    TOUCH_TARGET_NONE = 0,
    TOUCH_TARGET_POWER,
    TOUCH_TARGET_BRIGHTNESS_DOWN,
    TOUCH_TARGET_BRIGHTNESS_UP
  };

  TouchTarget touchTarget = TOUCH_TARGET_NONE;

  // =========================================================
  // Touch state
  // =========================================================

  bool touchActive = false;

  bool lastTouchInsidePower = false;
  bool lastTouchInsideBrightness = false;

  bool powerButtonVisualPressed = false;
  bool brightnessButtonVisualPressed = false;

  bool brightnessLongPressActive = false;

  int16_t lastTouchX = -1;
  int16_t lastTouchY = -1;

  unsigned long controlPressStartTime = 0;
  unsigned long lastBrightnessRepeat = 0;

  // =========================================================
  // Power button
  // =========================================================

  static constexpr int16_t POWER_BUTTON_X = 30;
  static constexpr int16_t POWER_BUTTON_Y = 184;
  static constexpr int16_t POWER_BUTTON_W = 260;
  static constexpr int16_t POWER_BUTTON_H = 44;

  // =========================================================
  // Brightness buttons
  //
  // Visible area = Touch area
  //
  // LEFT:
  //   X = 30 .. 101
  //
  // RIGHT:
  //   X = 218 .. 289
  //
  // =========================================================

  static constexpr int16_t BRI_LEFT_X = 30;
  static constexpr int16_t BRI_LEFT_Y = 106;
  static constexpr int16_t BRI_LEFT_W = 72;
  static constexpr int16_t BRI_LEFT_H = 30;

  static constexpr int16_t BRI_RIGHT_X = 218;
  static constexpr int16_t BRI_RIGHT_Y = 106;
  static constexpr int16_t BRI_RIGHT_W = 72;
  static constexpr int16_t BRI_RIGHT_H = 30;

  // =========================================================
  // Touch timing
  // =========================================================

  // About 66 touch checks per second
  static constexpr unsigned long TOUCH_POLL_MS = 15;

  // Temporary touch loss protection
  static constexpr unsigned long TOUCH_RELEASE_CONFIRM_MS = 70;

  // Power button double-action prevention
  static constexpr unsigned long TOUCH_ACTION_COOLDOWN_MS = 250;

  // ---------------------------------------------------------
  // Brightness button behavior
  //
  // Short press:
  //   +/- 1
  //
  // Long press:
  //   starts after 400 ms
  //   +/- 5 every 80 ms
  // ---------------------------------------------------------

  static constexpr unsigned long BRI_LONG_PRESS_MS = 400;
  static constexpr unsigned long BRI_REPEAT_MS = 80;

  static constexpr int BRI_SHORT_STEP = 1;
  static constexpr int BRI_LONG_STEP = 5;

  // =========================================================
  // Boot screen
  // =========================================================

  void drawBootScreen()
  {
    display.fillScreen(TFT_BLACK);

    display.setTextDatum(textdatum_t::middle_center);
    display.setTextColor(TFT_WHITE, TFT_BLACK);

    display.setTextSize(3);

    display.drawString(
      "WLED CoreS3",
      screenWidth / 2,
      80
    );

    display.setTextSize(2);

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
    display.fillScreen(TFT_BLACK);

    display.setTextDatum(textdatum_t::middle_center);
    display.setTextColor(TFT_WHITE, TFT_BLACK);

    display.setTextSize(3);

    display.drawString(
      "WLED CoreS3",
      screenWidth / 2,
      65
    );

    display.setTextSize(2);

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

    connectingScreenShown = true;
    readyScreenShown = false;

    resetTouchGesture();
  }

  // =========================================================
  // Ready screen
  // =========================================================

  void drawReadyScreen(const String& ipAddress)
  {
    display.fillScreen(TFT_BLACK);

    display.setTextDatum(textdatum_t::middle_center);
    display.setTextColor(TFT_WHITE, TFT_BLACK);

    // Title
    display.setTextSize(2);

    display.drawString(
      "WLED CoreS3",
      screenWidth / 2,
      18
    );

    // IP address
    display.setTextSize(1);

    display.drawString(
      ipAddress,
      screenWidth / 2,
      40
    );

    // Divider
    display.drawFastHLine(
      20,
      54,
      screenWidth - 40,
      TFT_DARKGREY
    );

    readyScreenShown = true;
    connectingScreenShown = false;

    lastLedState = -1;
    lastBrightnessValue = -1;
    lastEffectMode = -1;

    resetTouchGesture();
  }

  // =========================================================
  // LED Power
  // =========================================================

  void drawLedPower(bool ledOn)
  {
    display.fillRect(
      0,
      58,
      screenWidth,
      30,
      TFT_BLACK
    );

    display.setTextSize(2);

    display.setTextDatum(textdatum_t::middle_left);
    display.setTextColor(TFT_WHITE, TFT_BLACK);

    display.drawString(
      "LED Power:",
      20,
      73
    );

    display.setTextDatum(textdatum_t::middle_right);

    if (ledOn)
    {
      display.setTextColor(
        TFT_GREEN,
        TFT_BLACK
      );

      display.drawString(
        "ON",
        screenWidth - 20,
        73
      );
    }
    else
    {
      display.setTextColor(
        TFT_RED,
        TFT_BLACK
      );

      display.drawString(
        "OFF",
        screenWidth - 20,
        73
      );
    }
  }

  // =========================================================
  // Draw one Brightness triangle button
  // =========================================================

  void drawBrightnessButton(
    int16_t x,
    int16_t y,
    int16_t w,
    int16_t h,
    bool increase,
    bool pressed
  )
  {
    const uint16_t buttonColor = TFT_CYAN;

    // Clear around button
    display.fillRect(
      x - 2,
      y - 2,
      w + 4,
      h + 4,
      TFT_BLACK
    );

    // -------------------------------------------------------
    // Pressed
    // -------------------------------------------------------

    if (pressed)
    {
      display.fillRect(
        x,
        y,
        w,
        h,
        buttonColor
      );

      display.drawRect(
        x,
        y,
        w,
        h,
        buttonColor
      );
    }

    // -------------------------------------------------------
    // Normal
    // -------------------------------------------------------

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

    int16_t centerX = x + (w / 2);
    int16_t centerY = y + (h / 2);

    uint16_t triangleColor =
      pressed ? TFT_BLACK : buttonColor;

    // -------------------------------------------------------
    // Right triangle
    // -------------------------------------------------------

    if (increase)
    {
      display.fillTriangle(
        centerX + 11,
        centerY,

        centerX - 8,
        centerY - 10,

        centerX - 8,
        centerY + 10,

        triangleColor
      );
    }

    // -------------------------------------------------------
    // Left triangle
    // -------------------------------------------------------

    else
    {
      display.fillTriangle(
        centerX - 11,
        centerY,

        centerX + 8,
        centerY - 10,

        centerX + 8,
        centerY + 10,

        triangleColor
      );
    }
  }

  // =========================================================
  // Brightness display
  //
  //     Brightness
  //
  //   [ < ]   128   [ > ]
  // =========================================================

  void drawBrightness(
    int brightnessValue,
    TouchTarget pressedTarget = TOUCH_TARGET_NONE
  )
  {
    // Clear whole Brightness area
    display.fillRect(
      0,
      88,
      screenWidth,
      50,
      TFT_BLACK
    );

    // -------------------------------------------------------
    // Label
    // -------------------------------------------------------

    display.setTextDatum(textdatum_t::middle_center);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(1);

    display.drawString(
      "Brightness",
      screenWidth / 2,
      96
    );

    // -------------------------------------------------------
    // Left button
    // -------------------------------------------------------

    drawBrightnessButton(
      BRI_LEFT_X,
      BRI_LEFT_Y,
      BRI_LEFT_W,
      BRI_LEFT_H,
      false,
      pressedTarget == TOUCH_TARGET_BRIGHTNESS_DOWN
    );

    // -------------------------------------------------------
    // Right button
    // -------------------------------------------------------

    drawBrightnessButton(
      BRI_RIGHT_X,
      BRI_RIGHT_Y,
      BRI_RIGHT_W,
      BRI_RIGHT_H,
      true,
      pressedTarget == TOUCH_TARGET_BRIGHTNESS_UP
    );

    // -------------------------------------------------------
    // Numeric value
    // -------------------------------------------------------

    char valueText[8];

    snprintf(
      valueText,
      sizeof(valueText),
      "%d",
      brightnessValue
    );

    display.setTextDatum(textdatum_t::middle_center);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(2);

    display.drawString(
      valueText,
      screenWidth / 2,
      121
    );
  }

  // =========================================================
  // Effect
  // =========================================================

  void drawEffect(uint8_t effectMode)
  {
    display.fillRect(
      0,
      138,
      screenWidth,
      42,
      TFT_BLACK
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextSize(1);

    display.drawString(
      "Effect",
      screenWidth / 2,
      146
    );

    char effectName[64];

    effectName[0] = '\0';

    extractModeName(
      effectMode,
      nullptr,
      effectName,
      sizeof(effectName) - 1
    );

    if (strlen(effectName) > 24)
    {
      effectName[24] = '\0';
    }

    if (strlen(effectName) == 0)
    {
      strcpy(
        effectName,
        "Unknown"
      );
    }

    display.setTextSize(2);

    display.drawString(
      effectName,
      screenWidth / 2,
      165
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
    uint16_t actionColor =
      ledOn ? TFT_RED : TFT_GREEN;

    display.fillRect(
      POWER_BUTTON_X - 2,
      POWER_BUTTON_Y - 2,
      POWER_BUTTON_W + 4,
      POWER_BUTTON_H + 4,
      TFT_BLACK
    );

    // -------------------------------------------------------
    // Pressed
    // -------------------------------------------------------

    if (pressed)
    {
      display.fillRect(
        POWER_BUTTON_X,
        POWER_BUTTON_Y,
        POWER_BUTTON_W,
        POWER_BUTTON_H,
        actionColor
      );

      display.setTextColor(
        TFT_BLACK,
        actionColor
      );
    }

    // -------------------------------------------------------
    // Normal
    // -------------------------------------------------------

    else
    {
      display.fillRect(
        POWER_BUTTON_X,
        POWER_BUTTON_Y,
        POWER_BUTTON_W,
        POWER_BUTTON_H,
        TFT_BLACK
      );

      display.drawRect(
        POWER_BUTTON_X,
        POWER_BUTTON_Y,
        POWER_BUTTON_W,
        POWER_BUTTON_H,
        actionColor
      );

      display.drawRect(
        POWER_BUTTON_X + 1,
        POWER_BUTTON_Y + 1,
        POWER_BUTTON_W - 2,
        POWER_BUTTON_H - 2,
        actionColor
      );

      display.setTextColor(
        actionColor,
        TFT_BLACK
      );
    }

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextSize(2);

    display.drawString(
      ledOn ? "TURN OFF" : "TURN ON",
      POWER_BUTTON_X + (POWER_BUTTON_W / 2),
      POWER_BUTTON_Y + (POWER_BUTTON_H / 2)
    );

    powerButtonVisualPressed =
      pressed;
  }

  // =========================================================
  // Hit testing
  // =========================================================

  bool isPowerButtonTouched(
    int16_t x,
    int16_t y
  )
  {
    return (
      x >= POWER_BUTTON_X &&
      x <  POWER_BUTTON_X + POWER_BUTTON_W &&
      y >= POWER_BUTTON_Y &&
      y <  POWER_BUTTON_Y + POWER_BUTTON_H
    );
  }

  bool isBrightnessDownTouched(
    int16_t x,
    int16_t y
  )
  {
    return (
      x >= BRI_LEFT_X &&
      x <  BRI_LEFT_X + BRI_LEFT_W &&
      y >= BRI_LEFT_Y &&
      y <  BRI_LEFT_Y + BRI_LEFT_H
    );
  }

  bool isBrightnessUpTouched(
    int16_t x,
    int16_t y
  )
  {
    return (
      x >= BRI_RIGHT_X &&
      x <  BRI_RIGHT_X + BRI_RIGHT_W &&
      y >= BRI_RIGHT_Y &&
      y <  BRI_RIGHT_Y + BRI_RIGHT_H
    );
  }

  // =========================================================
  // Reset gesture
  // =========================================================

  void resetTouchGesture()
  {
    touchActive = false;

    touchTarget =
      TOUCH_TARGET_NONE;

    lastTouchInsidePower =
      false;

    lastTouchInsideBrightness =
      false;

    powerButtonVisualPressed =
      false;

    brightnessButtonVisualPressed =
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
  // WLED Power toggle
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
      bri > 0 ? "ON" : "OFF",
      bri
    );
  }

  // =========================================================
  // Apply exact Brightness value
  //
  // WLED uses the same basic 0 / non-zero handling for its
  // analog brightness input:
  //
  // - save previous brightness when moving to zero
  // - restart effect runtime when moving from zero to non-zero
  // - notify WLED interfaces through stateUpdated()
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

    if (newValue == bri)
    {
      return false;
    }

    // -------------------------------------------------------
    // Move to OFF
    // -------------------------------------------------------

    if (newValue == 0)
    {
      if (bri > 0)
      {
        briLast = bri;
        bri = 0;
      }
    }

    // -------------------------------------------------------
    // Move to ON / change brightness
    // -------------------------------------------------------

    else
    {
      if (bri == 0)
      {
        strip.restartRuntime();
      }

      bri =
        (uint8_t)newValue;
    }

    stateUpdated(
      CALL_MODE_BUTTON
    );

    // Power may have changed if we crossed zero.
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
  // Apply Brightness step
  // =========================================================

  void applyBrightnessStep(
    int step
  )
  {
    int newValue =
      (int)bri + step;

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
      // Update LCD immediately.
      drawBrightness(
        bri,
        touchTarget
      );

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
  // Brightness long press step
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
  // Is finger still inside selected Brightness button?
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
      now - lastTouchPoll <
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
      // Cancel temporary release candidate.
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
        isBrightnessDownTouched(
          touchX,
          touchY
        );

      bool insideBrightnessUp =
        isBrightnessUpTouched(
          touchX,
          touchY
        );

      // -----------------------------------------------------
      // Start new gesture
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
          "Touch start X=%d Y=%d\n",
          touchX,
          touchY
        );
      }

      // -----------------------------------------------------
      // Select control
      //
      // If the first touch point is just outside a control,
      // the user may still move into the visible control and
      // select it.
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

          brightnessLongPressActive =
            false;
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

          brightnessLongPressActive =
            false;
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
      // Brightness LEFT / RIGHT
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

        // ---------------------------------------------------
        // Pressed visual feedback
        // ---------------------------------------------------

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

        // ---------------------------------------------------
        // Only repeat while the finger remains inside the
        // selected button.
        // ---------------------------------------------------

        if (!insideSelectedButton)
        {
          return;
        }

        // ---------------------------------------------------
        // Enter long-press mode
        // ---------------------------------------------------

        if (
          !brightnessLongPressActive &&
          now - controlPressStartTime >=
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

        // ---------------------------------------------------
        // Repeated long-press action
        // ---------------------------------------------------

        if (
          brightnessLongPressActive &&
          now - lastBrightnessRepeat >=
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
    // Ignore very short touch-loss samples
    // -------------------------------------------------------

    if (
      now - touchReleaseCandidate <
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
        now - lastTouchAction >=
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

    Serial.printf(
      "[CoreS3_Display] "
      "Touch release X=%d Y=%d "
      "Target=%d Long=%s\n",
      lastTouchX,
      lastTouchY,
      (int)releasedTarget,
      wasLongPress ? "YES" : "NO"
    );

    // -------------------------------------------------------
    // Restore visuals before applying release action
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

    // -------------------------------------------------------
    // Reset touch state
    // -------------------------------------------------------

    touchActive =
      false;

    touchTarget =
      TOUCH_TARGET_NONE;

    lastTouchInsidePower =
      false;

    lastTouchInsideBrightness =
      false;

    powerButtonVisualPressed =
      false;

    brightnessButtonVisualPressed =
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

    // -------------------------------------------------------
    // Power action
    // -------------------------------------------------------

    if (executePowerAction)
    {
      lastTouchAction =
        now;

      toggleLedPowerFromTouch();

      return;
    }

    // -------------------------------------------------------
    // Brightness short press
    //
    // A long press has already changed the brightness while
    // the finger was held, so no additional +/-1 is applied
    // when a long press is released.
    // -------------------------------------------------------

    if (executeBrightnessShortPress)
    {
      brightnessShortPress(
        releasedTarget
      );

      // Return to normal button appearance.
      drawBrightness(
        bri,
        TOUCH_TARGET_NONE
      );

      lastBrightnessValue =
        bri;
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
        "Phase 5.1 start"
      )
    );

    // -------------------------------------------------------
    // Display
    // -------------------------------------------------------

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

    // -------------------------------------------------------
    // Touch
    // -------------------------------------------------------

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
    // Backlight
    // -------------------------------------------------------

    display.setBrightness(
      128
    );

    // -------------------------------------------------------
    // Boot screen
    // -------------------------------------------------------

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
        "Phase 5.1 setup complete"
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

    // Fast touch handling
    handleTouch();

    unsigned long now =
      millis();

    // -------------------------------------------------------
    // Normal LCD status refresh
    // -------------------------------------------------------

    if (
      now - lastUpdate <
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
      if (!connectingScreenShown)
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
    // Wi-Fi connected
    // =======================================================

    String currentIPAddress =
      WiFi.localIP().toString();

    if (
      !lastWiFiConnected ||
      !readyScreenShown ||
      currentIPAddress !=
        lastIPAddress
    )
    {
      drawReadyScreen(
        currentIPAddress
      );

      Serial.printf(
        "[CoreS3_Display] "
        "Wi-Fi connected: %s\n",
        currentIPAddress.c_str()
      );

      lastIPAddress =
        currentIPAddress;
    }

    lastWiFiConnected =
      true;

    // =======================================================
    // LED Power
    // =======================================================

    bool ledOn =
      (bri > 0);

    if (
      (int8_t)ledOn !=
      lastLedState
    )
    {
      drawLedPower(
        ledOn
      );

      // Do not erase Power button pressed feedback.
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
        ledOn ? 1 : 0;
    }

    // =======================================================
    // Brightness
    //
    // Web UI changes are reflected here too.
    //
    // While one of the local Brightness buttons is being
    // pressed, the touch handler owns this display area.
    // =======================================================

    int brightnessValue =
      bri;

    bool brightnessTouchActive =
      (
        touchTarget ==
          TOUCH_TARGET_BRIGHTNESS_DOWN ||
        touchTarget ==
          TOUCH_TARGET_BRIGHTNESS_UP
      );

    if (
      !brightnessTouchActive &&
      brightnessValue !=
        lastBrightnessValue
    )
    {
      drawBrightness(
        brightnessValue,
        TOUCH_TARGET_NONE
      );

      lastBrightnessValue =
        brightnessValue;
    }

    // =======================================================
    // Effect
    // =======================================================

    uint8_t effectMode =
      0;

    if (
      strip.getSegmentsNum() >
      0
    )
    {
      effectMode =
        strip.getMainSegment().mode;
    }

    if (
      (int)effectMode !=
      lastEffectMode
    )
    {
      drawEffect(
        effectMode
      );

      lastEffectMode =
        effectMode;
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

    if (user.isNull())
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
  }
};

static CoreS3DisplayUsermod coreS3DisplayUsermod;

REGISTER_USERMOD(coreS3DisplayUsermod);
