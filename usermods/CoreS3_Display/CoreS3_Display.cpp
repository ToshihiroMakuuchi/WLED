#include "wled.h"
#include <WiFi.h>
#include <M5GFX.h>

class CoreS3DisplayUsermod : public Usermod
{
private:
  M5GFX display;

  bool displayReady = false;

  int16_t screenWidth = 0;
  int16_t screenHeight = 0;

  unsigned long lastUpdate = 0;

  bool lastWiFiConnected = false;
  String lastIPAddress = "";

  bool readyScreenShown = false;
  bool connectingScreenShown = false;

  int8_t lastLedState = -1;
  int lastBrightnessValue = -1;
  int lastEffectMode = -1;

  // ---------------------------------------------------------
  // Boot screen
  // ---------------------------------------------------------

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

  // ---------------------------------------------------------
  // Wi-Fi connecting screen
  // ---------------------------------------------------------

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
  }

  // ---------------------------------------------------------
  // Ready screen base
  // ---------------------------------------------------------

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
      22
    );

    // IP address
    display.setTextSize(1);

    display.drawString(
      ipAddress,
      screenWidth / 2,
      46
    );

    // Divider
    display.drawFastHLine(
      20,
      62,
      screenWidth - 40,
      TFT_DARKGREY
    );

    readyScreenShown = true;
    connectingScreenShown = false;

    // Force all status fields to redraw
    lastLedState = -1;
    lastBrightnessValue = -1;
    lastEffectMode = -1;
  }

  // ---------------------------------------------------------
  // LED Power
  // ---------------------------------------------------------

  void drawLedPower(bool ledOn)
  {
    display.fillRect(
      0,
      70,
      screenWidth,
      40,
      TFT_BLACK
    );

    display.setTextSize(2);

    display.setTextDatum(textdatum_t::middle_left);
    display.setTextColor(TFT_WHITE, TFT_BLACK);

    display.drawString(
      "LED Power:",
      20,
      90
    );

    display.setTextDatum(textdatum_t::middle_right);

    if (ledOn)
    {
      display.setTextColor(TFT_GREEN, TFT_BLACK);

      display.drawString(
        "ON",
        screenWidth - 20,
        90
      );
    }
    else
    {
      display.setTextColor(TFT_RED, TFT_BLACK);

      display.drawString(
        "OFF",
        screenWidth - 20,
        90
      );
    }
  }

  // ---------------------------------------------------------
  // Brightness
  //
  // WLED native brightness value:
  //   0   = OFF / minimum
  //   255 = maximum
  // ---------------------------------------------------------

  void drawBrightness(int brightnessValue)
  {
    display.fillRect(
      0,
      110,
      screenWidth,
      40,
      TFT_BLACK
    );

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(2);

    display.setTextDatum(textdatum_t::middle_left);

    display.drawString(
      "Brightness:",
      20,
      130
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
      130
    );
  }

  // ---------------------------------------------------------
  // Effect
  // ---------------------------------------------------------

  void drawEffect(uint8_t effectMode)
  {
    display.fillRect(
      0,
      150,
      screenWidth,
      85,
      TFT_BLACK
    );

    display.setTextColor(TFT_WHITE, TFT_BLACK);

    display.setTextSize(1);
    display.setTextDatum(textdatum_t::middle_center);

    display.drawString(
      "Effect",
      screenWidth / 2,
      168
    );

    char effectName[64];

    effectName[0] = '\0';

    extractModeName(
      effectMode,
      nullptr,
      effectName,
      sizeof(effectName) - 1
    );

    // Protect the display from extremely long effect names
    if (strlen(effectName) > 24)
    {
      effectName[24] = '\0';
    }

    if (strlen(effectName) == 0)
    {
      strcpy(effectName, "Unknown");
    }

    display.setTextSize(2);

    display.drawString(
      effectName,
      screenWidth / 2,
      205
    );
  }

public:

  // ---------------------------------------------------------
  // Setup
  // ---------------------------------------------------------

  void setup() override
  {
    Serial.println();
    Serial.println(F("[CoreS3_Display] Phase 3 start"));

    display.begin();

    display.setRotation(1);

    screenWidth = display.width();
    screenHeight = display.height();

    Serial.printf(
      "[CoreS3_Display] Display size: %d x %d\n",
      screenWidth,
      screenHeight
    );

    if (screenWidth <= 0 || screenHeight <= 0)
    {
      Serial.println(
        F("[CoreS3_Display] ERROR: Display not detected")
      );

      return;
    }

    display.setBrightness(128);

    drawBootScreen();

    displayReady = true;

    Serial.println(
      F("[CoreS3_Display] Display initialized")
    );

    Serial.println(
      F("[CoreS3_Display] Phase 3 setup complete")
    );

    Serial.println();
  }

  // ---------------------------------------------------------
  // Main loop
  // ---------------------------------------------------------

  void loop() override
  {
    if (!displayReady)
    {
      return;
    }

    // Check status 4 times per second
    if (millis() - lastUpdate < 250)
    {
      return;
    }

    lastUpdate = millis();

    bool wifiConnected =
      (WiFi.status() == WL_CONNECTED);

    // -------------------------------------------------------
    // Wi-Fi not connected
    // -------------------------------------------------------

    if (!wifiConnected)
    {
      if (!connectingScreenShown)
      {
        drawConnectingScreen();
      }

      lastWiFiConnected = false;
      lastIPAddress = "";

      return;
    }

    // -------------------------------------------------------
    // Wi-Fi connected
    // -------------------------------------------------------

    String currentIPAddress =
      WiFi.localIP().toString();

    if (
      !lastWiFiConnected ||
      !readyScreenShown ||
      currentIPAddress != lastIPAddress
    )
    {
      drawReadyScreen(currentIPAddress);

      Serial.printf(
        "[CoreS3_Display] Wi-Fi connected: %s\n",
        currentIPAddress.c_str()
      );

      lastIPAddress = currentIPAddress;
    }

    lastWiFiConnected = true;

    // -------------------------------------------------------
    // WLED Power state
    //
    // bri == 0 : OFF
    // bri > 0  : ON
    // -------------------------------------------------------

    bool ledOn = (bri > 0);

    if ((int8_t)ledOn != lastLedState)
    {
      drawLedPower(ledOn);

      lastLedState = ledOn ? 1 : 0;
    }

    // -------------------------------------------------------
    // Brightness
    //
    // Display the native WLED value directly:
    //   0 - 255
    // -------------------------------------------------------

    int brightnessValue = bri;

    if (brightnessValue != lastBrightnessValue)
    {
      drawBrightness(brightnessValue);

      lastBrightnessValue =
        brightnessValue;
    }

    // -------------------------------------------------------
    // Effect from the WLED main segment
    // -------------------------------------------------------

    uint8_t effectMode = 0;

    if (strip.getSegmentsNum() > 0)
    {
      effectMode =
        strip.getMainSegment().mode;
    }

    if ((int)effectMode != lastEffectMode)
    {
      drawEffect(effectMode);

      lastEffectMode = effectMode;
    }
  }

  // ---------------------------------------------------------
  // WLED Info page
  // ---------------------------------------------------------

  void addToJsonInfo(JsonObject& root) override
  {
    JsonObject user = root["u"];

    if (user.isNull())
    {
      user = root.createNestedObject("u");
    }

    JsonArray displayInfo =
      user.createNestedArray("CoreS3 Display");

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

      displayInfo.add(text);
    }
    else
    {
      displayInfo.add("FAILED");
    }

    JsonArray wifiInfo =
      user.createNestedArray("CoreS3 Display WiFi");

    if (WiFi.status() == WL_CONNECTED)
    {
      wifiInfo.add(
        WiFi.localIP().toString()
      );
    }
    else
    {
      wifiInfo.add("Not connected");
    }

    JsonArray ledInfo =
      user.createNestedArray("CoreS3 Display LED");

    ledInfo.add(
      bri > 0 ? "ON" : "OFF"
    );

    JsonArray brightnessInfo =
      user.createNestedArray("CoreS3 Display Brightness");

    brightnessInfo.add(bri);
  }
};

static CoreS3DisplayUsermod coreS3DisplayUsermod;

REGISTER_USERMOD(coreS3DisplayUsermod);
