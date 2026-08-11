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

  // ---------------------------------------------------------
  // Draw boot screen
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
  // Draw Wi-Fi connecting screen
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
  }

  // ---------------------------------------------------------
  // Draw ready screen
  // ---------------------------------------------------------

  void drawReadyScreen(const String& ipAddress)
  {
    display.fillScreen(TFT_BLACK);

    display.setTextDatum(textdatum_t::middle_center);
    display.setTextColor(TFT_WHITE, TFT_BLACK);

    // -------------------------------------------------------
    // Title
    // -------------------------------------------------------

    display.setTextSize(3);

    display.drawString(
      "WLED CoreS3",
      screenWidth / 2,
      45
    );

    // -------------------------------------------------------
    // Ready
    // -------------------------------------------------------

    display.setTextSize(2);

    display.drawString(
      "WLED Ready",
      screenWidth / 2,
      100
    );

    // -------------------------------------------------------
    // Wi-Fi
    // -------------------------------------------------------

    display.drawString(
      "Wi-Fi Connected",
      screenWidth / 2,
      140
    );

    // -------------------------------------------------------
    // IP address
    // -------------------------------------------------------

    display.setTextSize(2);

    display.drawString(
      ipAddress,
      screenWidth / 2,
      180
    );
  }

public:

  // ---------------------------------------------------------
  // Setup
  // ---------------------------------------------------------

  void setup() override
  {
    Serial.println();
    Serial.println(F("[CoreS3_Display] Phase 2 start"));

    // -------------------------------------------------------
    // Initialize CoreS3 display
    // -------------------------------------------------------

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

    // -------------------------------------------------------
    // Backlight
    // -------------------------------------------------------

    display.setBrightness(128);

    // -------------------------------------------------------
    // Initial screen
    // -------------------------------------------------------

    drawBootScreen();

    displayReady = true;

    Serial.println(
      F("[CoreS3_Display] Display initialized")
    );

    Serial.println(
      F("[CoreS3_Display] Phase 2 setup complete")
    );

    Serial.println();
  }

  // ---------------------------------------------------------
  // Loop
  // ---------------------------------------------------------

  void loop() override
  {
    if (!displayReady)
    {
      return;
    }

    // Update twice per second
    if (millis() - lastUpdate < 500)
    {
      return;
    }

    lastUpdate = millis();

    bool wifiConnected =
      (WiFi.status() == WL_CONNECTED);

    // -------------------------------------------------------
    // Wi-Fi connected
    // -------------------------------------------------------

    if (wifiConnected)
    {
      String currentIPAddress =
        WiFi.localIP().toString();

      // Redraw only if state/IP changed
      if (
        !lastWiFiConnected ||
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
    }

    // -------------------------------------------------------
    // Wi-Fi disconnected / connecting
    // -------------------------------------------------------

    else
    {
      if (lastWiFiConnected)
      {
        drawConnectingScreen();

        Serial.println(
          F("[CoreS3_Display] Wi-Fi disconnected")
        );
      }
    }

    lastWiFiConnected = wifiConnected;
  }

  // ---------------------------------------------------------
  // WLED Info
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
  }
};

static CoreS3DisplayUsermod coreS3DisplayUsermod;

REGISTER_USERMOD(coreS3DisplayUsermod);
