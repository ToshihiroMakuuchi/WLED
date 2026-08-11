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

  // ---------------------------------------------------------
  // Touch state
  // ---------------------------------------------------------

  bool touchActive = false;

  // Becomes true if the finger enters the Power button
  // during the current touch gesture.
  bool powerGestureArmed = false;

  // Last valid touch sample was inside the Power hit area.
  bool lastTouchInsidePower = false;

  // Current visual pressed state of the button.
  bool powerButtonVisualPressed = false;

  int16_t lastTouchX = -1;
  int16_t lastTouchY = -1;

  // ---------------------------------------------------------
  // Visible Power button
  // ---------------------------------------------------------

  static constexpr int16_t POWER_BUTTON_X = 30;
  static constexpr int16_t POWER_BUTTON_Y = 184;
  static constexpr int16_t POWER_BUTTON_W = 260;
  static constexpr int16_t POWER_BUTTON_H = 44;

  // ---------------------------------------------------------
  // Invisible enlarged touch area
  //
  // The visible button is:
  //   X = 30 .. 289
  //   Y = 184 .. 227
  //
  // Touch detection is intentionally larger:
  //   X = 10 .. 309
  //   Y = 168 .. 239
  // ---------------------------------------------------------

  static constexpr int16_t POWER_TOUCH_X1 = 10;
  static constexpr int16_t POWER_TOUCH_Y1 = 168;
  static constexpr int16_t POWER_TOUCH_X2 = 309;
  static constexpr int16_t POWER_TOUCH_Y2 = 239;

  // ---------------------------------------------------------
  // Touch timing
  // ---------------------------------------------------------

  // About 66 touch checks per second
  static constexpr unsigned long TOUCH_POLL_MS = 15;

  // A short temporary loss of touch should not immediately
  // be interpreted as finger release.
  static constexpr unsigned long TOUCH_RELEASE_CONFIRM_MS = 70;

  // Prevent an accidental immediate second activation.
  static constexpr unsigned long TOUCH_ACTION_COOLDOWN_MS = 250;

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
  // Ready screen base
  // =========================================================

  void drawReadyScreen(const String& ipAddress)
  {
    display.fillScreen(TFT_BLACK);

    display.setTextDatum(textdatum_t::middle_center);
    display.setTextColor(TFT_WHITE, TFT_BLACK);

    // -------------------------------------------------------
    // Title
    // -------------------------------------------------------

    display.setTextSize(2);

    display.drawString(
      "WLED CoreS3",
      screenWidth / 2,
      18
    );

    // -------------------------------------------------------
    // IP address
    // -------------------------------------------------------

    display.setTextSize(1);

    display.drawString(
      ipAddress,
      screenWidth / 2,
      40
    );

    // -------------------------------------------------------
    // Divider
    // -------------------------------------------------------

    display.drawFastHLine(
      20,
      54,
      screenWidth - 40,
      TFT_DARKGREY
    );

    readyScreenShown = true;
    connectingScreenShown = false;

    // Force status fields to redraw
    lastLedState = -1;
    lastBrightnessValue = -1;
    lastEffectMode = -1;

    resetTouchGesture();
  }

  // =========================================================
  // LED Power status
  // =========================================================

  void drawLedPower(bool ledOn)
  {
    display.fillRect(
      0,
      58,
      screenWidth,
      36,
      TFT_BLACK
    );

    display.setTextSize(2);

    display.setTextDatum(textdatum_t::middle_left);
    display.setTextColor(TFT_WHITE, TFT_BLACK);

    display.drawString(
      "LED Power:",
      20,
      76
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
        76
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
        76
      );
    }
  }

  // =========================================================
  // Brightness
  //
  // Native WLED value:
  //   0 - 255
  // =========================================================

  void drawBrightness(int brightnessValue)
  {
    display.fillRect(
      0,
      94,
      screenWidth,
      36,
      TFT_BLACK
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(2);
    display.setTextDatum(textdatum_t::middle_left);

    display.drawString(
      "Brightness:",
      20,
      112
    );

    char text[16];

    snprintf(
      text,
      sizeof(text),
      "%d",
      brightnessValue
    );

    display.setTextDatum(textdatum_t::middle_right);

    display.drawString(
      text,
      screenWidth - 20,
      112
    );
  }

  // =========================================================
  // Effect
  // =========================================================

  void drawEffect(uint8_t effectMode)
  {
    display.fillRect(
      0,
      132,
      screenWidth,
      48,
      TFT_BLACK
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(1);
    display.setTextDatum(textdatum_t::middle_center);

    display.drawString(
      "Effect",
      screenWidth / 2,
      140
    );

    char effectName[64];

    effectName[0] = '\0';

    extractModeName(
      effectMode,
      nullptr,
      effectName,
      sizeof(effectName) - 1
    );

    // Avoid overflowing the screen
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
      161
    );
  }

  // =========================================================
  // Power button
  //
  // pressed == false:
  //   black background / colored border
  //
  // pressed == true:
  //   colored background / black text
  //
  // This gives immediate visual feedback that the touch
  // controller has recognized the finger.
  // =========================================================

  void drawPowerButton(
    bool ledOn,
    bool pressed
  )
  {
    uint16_t actionColor =
      ledOn ? TFT_RED : TFT_GREEN;

    // Clear button area first
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
  // Enlarged Power touch area
  // =========================================================

  bool isPowerButtonTouched(
    int16_t x,
    int16_t y
  )
  {
    return (
      x >= POWER_TOUCH_X1 &&
      x <= POWER_TOUCH_X2 &&
      y >= POWER_TOUCH_Y1 &&
      y <= POWER_TOUCH_Y2
    );
  }

  // =========================================================
  // Reset touch gesture state
  // =========================================================

  void resetTouchGesture()
  {
    touchActive = false;

    powerGestureArmed = false;
    lastTouchInsidePower = false;

    powerButtonVisualPressed = false;

    touchReleaseCandidate = 0;

    lastTouchX = -1;
    lastTouchY = -1;
  }

  // =========================================================
  // Execute WLED Power toggle
  // =========================================================

  void toggleLedPowerFromTouch()
  {
    Serial.println(
      F("[CoreS3_Display] Touch POWER action")
    );

    // Same WLED power state mechanism used by
    // the normal WLED button handling.
    toggleOnOff();

    stateUpdated(
      CALL_MODE_BUTTON
    );

    // Force LCD values to refresh
    lastLedState = -1;
    lastBrightnessValue = -1;

    Serial.printf(
      "[CoreS3_Display] WLED Power -> %s, Brightness=%u\n",
      bri > 0 ? "ON" : "OFF",
      bri
    );
  }

  // =========================================================
  // Touch processing
  //
  // Improvements over Phase 4:
  //
  // 1. getTouch() directly returns display coordinates.
  //
  // 2. Finger position is tracked continuously.
  //
  // 3. A touch may START slightly outside the button.
  //    If the finger enters the enlarged hit area,
  //    the button becomes armed.
  //
  // 4. Temporary touch loss is ignored for 70 ms.
  //
  // 5. Action happens after confirmed release.
  //
  // 6. Pressed visual feedback is shown immediately.
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

    // -------------------------------------------------------
    // M5GFX getTouch()
    //
    // Unlike getTouchRaw(), this gives coordinates already
    // converted to the current display orientation.
    // -------------------------------------------------------

    int16_t touchX = -1;
    int16_t touchY = -1;

    bool touching =
      display.getTouch(
        &touchX,
        &touchY
      ) > 0;

    // =======================================================
    // Finger is touching
    // =======================================================

    if (touching)
    {
      // Cancel an in-progress release decision.
      touchReleaseCandidate = 0;

      lastTouchX =
        touchX;

      lastTouchY =
        touchY;

      bool insidePower =
        isPowerButtonTouched(
          touchX,
          touchY
        );

      // -----------------------------------------------------
      // Start of a new gesture
      // -----------------------------------------------------

      if (!touchActive)
      {
        touchActive = true;

        powerGestureArmed = false;
        lastTouchInsidePower = false;

        Serial.printf(
          "[CoreS3_Display] Touch start X=%d Y=%d\n",
          touchX,
          touchY
        );
      }

      // -----------------------------------------------------
      // Important improvement:
      //
      // The first touch point does NOT need to be inside the
      // button.
      //
      // If the finger moves into the button area while still
      // touching, the gesture becomes armed.
      // -----------------------------------------------------

      if (insidePower)
      {
        powerGestureArmed = true;
      }

      lastTouchInsidePower =
        insidePower;

      // -----------------------------------------------------
      // Visual feedback
      // -----------------------------------------------------

      bool shouldLookPressed =
        powerGestureArmed &&
        insidePower;

      if (
        shouldLookPressed !=
        powerButtonVisualPressed
      )
      {
        drawPowerButton(
          bri > 0,
          shouldLookPressed
        );
      }

      return;
    }

    // =======================================================
    // No touch currently detected
    // =======================================================

    if (!touchActive)
    {
      return;
    }

    // -------------------------------------------------------
    // First no-touch sample:
    // Start release confirmation timer.
    //
    // This prevents one brief missed sample from being
    // treated as an actual finger release.
    // -------------------------------------------------------

    if (touchReleaseCandidate == 0)
    {
      touchReleaseCandidate =
        now;

      return;
    }

    // -------------------------------------------------------
    // Wait until release has remained stable.
    // -------------------------------------------------------

    if (
      now - touchReleaseCandidate <
      TOUCH_RELEASE_CONFIRM_MS
    )
    {
      return;
    }

    // =======================================================
    // Confirmed finger release
    // =======================================================

    bool executePowerAction =
      powerGestureArmed &&
      lastTouchInsidePower &&
      (
        now - lastTouchAction >=
        TOUCH_ACTION_COOLDOWN_MS
      );

    Serial.printf(
      "[CoreS3_Display] Touch release X=%d Y=%d Armed=%s Inside=%s\n",
      lastTouchX,
      lastTouchY,
      powerGestureArmed ? "YES" : "NO",
      lastTouchInsidePower ? "YES" : "NO"
    );

    // -------------------------------------------------------
    // Return button to normal appearance first.
    // -------------------------------------------------------

    if (powerButtonVisualPressed)
    {
      drawPowerButton(
        bri > 0,
        false
      );
    }

    // -------------------------------------------------------
    // Reset gesture before executing the WLED action.
    // -------------------------------------------------------

    touchActive = false;

    powerGestureArmed = false;
    lastTouchInsidePower = false;

    touchReleaseCandidate = 0;

    lastTouchX = -1;
    lastTouchY = -1;

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
      F("[CoreS3_Display] Phase 4.1 start")
    );

    // -------------------------------------------------------
    // Display
    // -------------------------------------------------------

    display.begin();

    display.setRotation(1);

    screenWidth =
      display.width();

    screenHeight =
      display.height();

    Serial.printf(
      "[CoreS3_Display] Display size: %d x %d\n",
      screenWidth,
      screenHeight
    );

    if (
      screenWidth <= 0 ||
      screenHeight <= 0
    )
    {
      Serial.println(
        F("[CoreS3_Display] ERROR: Display not detected")
      );

      return;
    }

    // -------------------------------------------------------
    // Touch controller
    // -------------------------------------------------------

    touchReady =
      (display.touch() != nullptr);

    Serial.printf(
      "[CoreS3_Display] Touch: %s\n",
      touchReady ? "READY" : "NOT FOUND"
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
      F("[CoreS3_Display] Display initialized")
    );

    Serial.println(
      F("[CoreS3_Display] Phase 4.1 setup complete")
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
    // Touch runs independently at a much faster interval
    // than the status screen refresh.
    // -------------------------------------------------------

    handleTouch();

    // -------------------------------------------------------
    // Status update every 250 ms
    // -------------------------------------------------------

    unsigned long now =
      millis();

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
        "[CoreS3_Display] Wi-Fi connected: %s\n",
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

      // Do not overwrite the pressed feedback while
      // the user is actively touching the button.
      if (!powerButtonVisualPressed)
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
    // =======================================================

    int brightnessValue =
      bri;

    if (
      brightnessValue !=
      lastBrightnessValue
    )
    {
      drawBrightness(
        brightnessValue
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
  // WLED Info page
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
        root.createNestedObject("u");
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
