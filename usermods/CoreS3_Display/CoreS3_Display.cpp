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

  unsigned long lastBrightnessAction = 0;

  bool lastWiFiConnected = false;
  String lastIPAddress = "";

  bool readyScreenShown = false;
  bool connectingScreenShown = false;

  int8_t lastLedState = -1;
  int lastBrightnessValue = -1;
  int lastEffectMode = -1;

  // =========================================================
  // Touch state
  // =========================================================

  enum TouchTarget
  {
    TOUCH_TARGET_NONE = 0,
    TOUCH_TARGET_POWER,
    TOUCH_TARGET_BRIGHTNESS
  };

  TouchTarget touchTarget =
    TOUCH_TARGET_NONE;

  bool touchActive = false;

  bool lastTouchInsidePower = false;
  bool powerButtonVisualPressed = false;

  int16_t lastTouchX = -1;
  int16_t lastTouchY = -1;

  int pendingBrightnessValue = -1;

  // =========================================================
  // Power button
  // =========================================================

  static constexpr int16_t POWER_BUTTON_X = 30;
  static constexpr int16_t POWER_BUTTON_Y = 184;
  static constexpr int16_t POWER_BUTTON_W = 260;
  static constexpr int16_t POWER_BUTTON_H = 44;

  // =========================================================
  // Brightness slider
  //
  // Visible area and touch area are identical.
  // =========================================================

  static constexpr int16_t BRIGHTNESS_SLIDER_X = 30;
  static constexpr int16_t BRIGHTNESS_SLIDER_Y = 108;
  static constexpr int16_t BRIGHTNESS_SLIDER_W = 260;
  static constexpr int16_t BRIGHTNESS_SLIDER_H = 28;

  // =========================================================
  // Touch timing
  // =========================================================

  // Approximately 66 checks per second.
  static constexpr unsigned long TOUCH_POLL_MS = 15;

  // Confirm that the finger has actually left the panel.
  static constexpr unsigned long TOUCH_RELEASE_CONFIRM_MS = 70;

  // Power button double-action prevention.
  static constexpr unsigned long TOUCH_ACTION_COOLDOWN_MS = 250;

  // Limit WLED brightness updates while dragging.
  static constexpr unsigned long BRIGHTNESS_UPDATE_MS = 50;

  // =========================================================
  // Boot screen
  // =========================================================

  void drawBootScreen()
  {
    display.fillScreen(TFT_BLACK);

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

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
  // Wi-Fi connecting
  // =========================================================

  void drawConnectingScreen()
  {
    display.fillScreen(TFT_BLACK);

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

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

  void drawReadyScreen(
    const String& ipAddress
  )
  {
    display.fillScreen(TFT_BLACK);

    display.setTextDatum(
      textdatum_t::middle_center
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

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

  void drawLedPower(
    bool ledOn
  )
  {
    display.fillRect(
      0,
      58,
      screenWidth,
      30,
      TFT_BLACK
    );

    display.setTextSize(2);

    display.setTextDatum(
      textdatum_t::middle_left
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.drawString(
      "LED Power:",
      20,
      73
    );

    display.setTextDatum(
      textdatum_t::middle_right
    );

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
  // Brightness value + slider
  // =========================================================

  void drawBrightness(
    int brightnessValue,
    bool sliderActive = false
  )
  {
    // -------------------------------------------------------
    // Clear brightness area
    // -------------------------------------------------------

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

    display.setTextSize(1);

    display.setTextDatum(
      textdatum_t::middle_left
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.drawString(
      "Brightness:",
      30,
      98
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

    display.setTextDatum(
      textdatum_t::middle_right
    );

    display.drawString(
      valueText,
      screenWidth - 30,
      98
    );

    // -------------------------------------------------------
    // Slider frame
    // -------------------------------------------------------

    uint16_t sliderColor =
      sliderActive ?
      TFT_CYAN :
      TFT_WHITE;

    display.drawRect(
      BRIGHTNESS_SLIDER_X,
      BRIGHTNESS_SLIDER_Y,
      BRIGHTNESS_SLIDER_W,
      BRIGHTNESS_SLIDER_H,
      sliderColor
    );

    display.drawRect(
      BRIGHTNESS_SLIDER_X + 1,
      BRIGHTNESS_SLIDER_Y + 1,
      BRIGHTNESS_SLIDER_W - 2,
      BRIGHTNESS_SLIDER_H - 2,
      sliderColor
    );

    // -------------------------------------------------------
    // Slider interior
    // -------------------------------------------------------

    const int16_t innerX =
      BRIGHTNESS_SLIDER_X + 4;

    const int16_t innerY =
      BRIGHTNESS_SLIDER_Y + 7;

    const int16_t innerW =
      BRIGHTNESS_SLIDER_W - 8;

    const int16_t innerH =
      BRIGHTNESS_SLIDER_H - 14;

    display.fillRect(
      innerX,
      innerY,
      innerW,
      innerH,
      TFT_DARKGREY
    );

    // -------------------------------------------------------
    // Filled portion
    // -------------------------------------------------------

    int16_t filledWidth =
      (
        (int32_t)innerW *
        brightnessValue
      ) / 255;

    if (filledWidth > 0)
    {
      display.fillRect(
        innerX,
        innerY,
        filledWidth,
        innerH,
        sliderColor
      );
    }

    // -------------------------------------------------------
    // Position marker
    // -------------------------------------------------------

    int16_t markerX =
      innerX +
      (
        (int32_t)(innerW - 1) *
        brightnessValue
      ) / 255;

    display.drawFastVLine(
      markerX,
      BRIGHTNESS_SLIDER_Y + 4,
      BRIGHTNESS_SLIDER_H - 8,
      sliderColor
    );
  }

  // =========================================================
  // Effect
  // =========================================================

  void drawEffect(
    uint8_t effectMode
  )
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

    if (
      strlen(effectName) >
      24
    )
    {
      effectName[24] =
        '\0';
    }

    if (
      strlen(effectName) ==
      0
    )
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
      ledOn ?
      TFT_RED :
      TFT_GREEN;

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
      ledOn ?
        "TURN OFF" :
        "TURN ON",
      POWER_BUTTON_X +
        (POWER_BUTTON_W / 2),
      POWER_BUTTON_Y +
        (POWER_BUTTON_H / 2)
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
      x <
        POWER_BUTTON_X +
        POWER_BUTTON_W &&
      y >= POWER_BUTTON_Y &&
      y <
        POWER_BUTTON_Y +
        POWER_BUTTON_H
    );
  }

  bool isBrightnessSliderTouched(
    int16_t x,
    int16_t y
  )
  {
    return (
      x >= BRIGHTNESS_SLIDER_X &&
      x <
        BRIGHTNESS_SLIDER_X +
        BRIGHTNESS_SLIDER_W &&
      y >= BRIGHTNESS_SLIDER_Y &&
      y <
        BRIGHTNESS_SLIDER_Y +
        BRIGHTNESS_SLIDER_H
    );
  }

  // =========================================================
  // Convert slider X coordinate to WLED 0 - 255
  // =========================================================

  int brightnessFromTouchX(
    int16_t x
  )
  {
    if (
      x <=
      BRIGHTNESS_SLIDER_X
    )
    {
      return 0;
    }

    int16_t rightX =
      BRIGHTNESS_SLIDER_X +
      BRIGHTNESS_SLIDER_W -
      1;

    if (
      x >=
      rightX
    )
    {
      return 255;
    }

    int32_t relativeX =
      x -
      BRIGHTNESS_SLIDER_X;

    int32_t span =
      BRIGHTNESS_SLIDER_W -
      1;

    return (
      relativeX *
      255 +
      (span / 2)
    ) / span;
  }

  // =========================================================
  // Reset touch gesture
  // =========================================================

  void resetTouchGesture()
  {
    touchActive =
      false;

    touchTarget =
      TOUCH_TARGET_NONE;

    lastTouchInsidePower =
      false;

    powerButtonVisualPressed =
      false;

    touchReleaseCandidate =
      0;

    lastTouchX =
      -1;

    lastTouchY =
      -1;

    pendingBrightnessValue =
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
      "WLED Power -> %s, "
      "Brightness=%u\n",
      bri > 0 ?
        "ON" :
        "OFF",
      bri
    );
  }

  // =========================================================
  // Apply Brightness to WLED
  //
  // This follows WLED's normal brightness behavior:
  //
  // - 0 remembers the last non-zero brightness.
  // - Going from 0 to a non-zero value restarts effect runtime.
  // - stateUpdated() informs WLED interfaces.
  // =========================================================

  void applyBrightnessFromTouch(
    int brightnessValue
  )
  {
    brightnessValue =
      constrain(
        brightnessValue,
        0,
        255
      );

    if (
      brightnessValue ==
      bri
    )
    {
      return;
    }

    // -------------------------------------------------------
    // Move to OFF
    // -------------------------------------------------------

    if (
      brightnessValue ==
      0
    )
    {
      if (bri > 0)
      {
        briLast =
          bri;

        bri =
          0;
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
        brightnessValue;
    }

    stateUpdated(
      CALL_MODE_BUTTON
    );

    lastBrightnessValue =
      bri;

    lastLedState =
      (bri > 0) ?
      1 :
      0;

    Serial.printf(
      "[CoreS3_Display] "
      "Brightness -> %u\n",
      bri
    );
  }

  // =========================================================
  // Update brightness slider while finger is moving
  // =========================================================

  void updateBrightnessGesture(
    int16_t touchX,
    bool forceApply
  )
  {
    int value =
      brightnessFromTouchX(
        touchX
      );

    pendingBrightnessValue =
      value;

    // Immediate local screen response.
    drawBrightness(
      value,
      true
    );

    unsigned long now =
      millis();

    if (
      forceApply ||
      now -
        lastBrightnessAction >=
        BRIGHTNESS_UPDATE_MS
    )
    {
      lastBrightnessAction =
        now;

      applyBrightnessFromTouch(
        value
      );
    }
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

      bool insideBrightness =
        isBrightnessSliderTouched(
          touchX,
          touchY
        );

      // -----------------------------------------------------
      // New gesture
      // -----------------------------------------------------

      if (!touchActive)
      {
        touchActive =
          true;

        touchTarget =
          TOUCH_TARGET_NONE;

        Serial.printf(
          "[CoreS3_Display] "
          "Touch start X=%d Y=%d\n",
          touchX,
          touchY
        );
      }

      // -----------------------------------------------------
      // Select a control.
      //
      // Once a control is selected for this gesture,
      // it remains locked until release.
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
        }
        else if (
          insideBrightness
        )
        {
          touchTarget =
            TOUCH_TARGET_BRIGHTNESS;

          pendingBrightnessValue =
            bri;
        }
      }

      // =====================================================
      // Power gesture
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
      // Brightness gesture
      //
      // Once the slider has been selected, X continues to
      // control brightness even if the finger moves slightly
      // above or below the slider.
      // =====================================================

      if (
        touchTarget ==
        TOUCH_TARGET_BRIGHTNESS
      )
      {
        updateBrightnessGesture(
          touchX,
          false
        );

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
    // Confirm release
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

    int finalBrightness =
      pendingBrightnessValue;

    Serial.printf(
      "[CoreS3_Display] "
      "Touch release X=%d Y=%d "
      "Target=%d\n",
      lastTouchX,
      lastTouchY,
      (int)releasedTarget
    );

    // -------------------------------------------------------
    // Restore Power button appearance
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
    // Apply final brightness exactly
    // -------------------------------------------------------

    if (
      releasedTarget ==
        TOUCH_TARGET_BRIGHTNESS &&
      finalBrightness >= 0
    )
    {
      applyBrightnessFromTouch(
        finalBrightness
      );

      drawBrightness(
        bri,
        false
      );

      lastBrightnessValue =
        bri;
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

    powerButtonVisualPressed =
      false;

    touchReleaseCandidate =
      0;

    lastTouchX =
      -1;

    lastTouchY =
      -1;

    pendingBrightnessValue =
      -1;

    // -------------------------------------------------------
    // Execute Power action
    // -------------------------------------------------------

    if (executePowerAction)
    {
      lastTouchAction =
        now;

      toggleLedPowerFromTouch();
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
        "Phase 5 start"
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
      touchReady ?
        "READY" :
        "NOT FOUND"
    );

    // -------------------------------------------------------
    // Backlight
    // -------------------------------------------------------

    display.setBrightness(
      128
    );

    // -------------------------------------------------------
    // Boot
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
        "Phase 5 setup complete"
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

    // Fast touch processing.
    handleTouch();

    unsigned long now =
      millis();

    // -------------------------------------------------------
    // Normal status refresh
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
        ledOn ?
        1 :
        0;
    }

    // =======================================================
    // Brightness
    //
    // Do not overwrite the user's local slider feedback while
    // the finger is actively controlling the slider.
    // =======================================================

    int brightnessValue =
      bri;

    if (
      touchTarget !=
        TOUCH_TARGET_BRIGHTNESS &&
      brightnessValue !=
        lastBrightnessValue
    )
    {
      drawBrightness(
        brightnessValue,
        false
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

    // Display
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

    // Touch
    JsonArray touchInfo =
      user.createNestedArray(
        "CoreS3 Display Touch"
      );

    touchInfo.add(
      touchReady ?
        "READY" :
        "NOT FOUND"
    );

    // Wi-Fi
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

    // LED
    JsonArray ledInfo =
      user.createNestedArray(
        "CoreS3 Display LED"
      );

    ledInfo.add(
      bri > 0 ?
        "ON" :
        "OFF"
    );

    // Brightness
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
