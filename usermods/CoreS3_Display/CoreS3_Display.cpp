#include "wled.h"
#include <WiFi.h>
#include <M5GFX.h>

#include "CoreS3_WLED_Logo.h"

// ===========================================================
// CoreS3 Display Usermod
//
// Phase 10.1.1
//
// MAIN
//   Power
//   Brightness
//   Effect Prev / Detail / Next
//   Color
//   Preset
//
// COLOR
//   Hue
//   Saturation
//
// EFFECT
//   Speed
//   Intensity
//   Palette
//
// PRESET
//   Previous / Next saved preset
//   Missing preset IDs are skipped
//   Presets wrap at first / last
//   Preset is applied immediately
//
// Phase 9.2.1
//   Palette visible button size unchanged
//   Expanded invisible touch area
//
// Phase 10.1
//   MAIN Color / Preset visible sizes unchanged
//   Expanded invisible touch areas
//
//   PRESET Prev / Next visible sizes unchanged
//   Expanded invisible touch areas
//
// Phase 10.1.1
//   Saved Preset IDs / names are cached in RAM.
//   Arrow operation no longer scans IDs 1..250.
//   Cache is rebuilt in the background.
//   presetsModifiedTime is used to detect changes.
//
// Common
//   Startup animation
//   Auto suspend
//   Touch wake
//   Configurable LCD settings
// ===========================================================

static const char CORES3_DISPLAY_CONFIG_NAME[] PROGMEM =
  "CoreS3_Display";

class CoreS3DisplayUsermod : public Usermod
{
private:

  // =========================================================
  // Display
  // =========================================================

  M5GFX display;

  bool displayReady = false;
  bool touchReady = false;
  bool initDone = false;

  int16_t screenWidth = 0;
  int16_t screenHeight = 0;

  unsigned long lastUpdate = 0;
  unsigned long lastTouchPoll = 0;
  unsigned long lastTouchAction = 0;
  unsigned long touchReleaseCandidate = 0;

  // =========================================================
  // Wi-Fi state
  // =========================================================

  bool lastWiFiConnected = false;
  String lastIPAddress = "";

  bool readyScreenShown = false;
  bool connectingScreenShown = false;

  // =========================================================
  // Cached WLED state
  // =========================================================

  int8_t lastLedState = -1;

  int lastBrightnessValue = -1;
  int lastEffectMode = -1;

  int lastSpeedValue = -1;
  int lastIntensityValue = -1;

  int lastPaletteValue = -1;

  int lastHueValue = -1;
  int lastSaturationValue = -1;

  uint32_t lastPrimaryColor = 0;
  bool lastPrimaryColorValid = false;

  // =========================================================
  // Phase 10.1
  // Preset state
  // =========================================================

  int lastPresetValue = -1;

  uint8_t pendingPresetId = 0;
  String pendingPresetName = "";

  unsigned long pendingPresetRequestMs = 0;

  unsigned long lastPresetsModifiedTime = 0;

  bool presetNoEntries = false;

  static constexpr unsigned long PRESET_APPLY_PENDING_MS = 1500;

  // =========================================================
  // Phase 10.1.1
  // Preset RAM cache
  //
  // WLED Preset names are limited to 32 characters.
  //
  // Cache is intentionally a fixed array:
  //   - no repeated heap allocation during navigation
  //   - predictable memory use
  //   - maximum WLED persistent Presets = 250
  //
  // Approximate RAM:
  //   250 x 34 bytes = about 8.5KB
  // =========================================================

  struct PresetCacheEntry
  {
    uint8_t id;
    char name[33];
  };

  PresetCacheEntry presetCache[250];

  uint16_t presetCacheCount = 0;

  bool presetCacheReady = false;
  bool presetCacheBuilding = false;

  uint16_t presetCacheScanId = 1;

  unsigned long presetCacheLastScanMs = 0;

  unsigned long presetCacheSourceModifiedTime = 0;
  unsigned long presetCacheBuildSourceModifiedTime = 0;

  static constexpr unsigned long PRESET_CACHE_SCAN_INTERVAL_MS = 5;

  // =========================================================
  // Phase 8.4
  // Persistent Display Settings
  // =========================================================

  uint16_t lcdBrightness = 128;

  // 0 = Never
  uint16_t sleepTimeoutSec = 30;

  bool fadeEnabled = true;

  uint16_t fadeDurationMs = 250;

  // =========================================================
  // Current physical LCD brightness
  // =========================================================

  uint8_t currentDisplayBrightness = 0;

  // =========================================================
  // Display suspend
  // =========================================================

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
    SCREEN_COLOR,
    SCREEN_EFFECT,
    SCREEN_PRESET
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
    TOUCH_TARGET_EFFECT_DETAIL,
    TOUCH_TARGET_EFFECT_NEXT,

    TOUCH_TARGET_COLOR_OPEN,
    TOUCH_TARGET_PRESET_OPEN,

    TOUCH_TARGET_BACK,

    TOUCH_TARGET_HUE_DOWN,
    TOUCH_TARGET_HUE_UP,

    TOUCH_TARGET_SATURATION_DOWN,
    TOUCH_TARGET_SATURATION_UP,

    TOUCH_TARGET_SPEED_DOWN,
    TOUCH_TARGET_SPEED_UP,

    TOUCH_TARGET_INTENSITY_DOWN,
    TOUCH_TARGET_INTENSITY_UP,

    TOUCH_TARGET_PALETTE_PREV,
    TOUCH_TARGET_PALETTE_NEXT,

    TOUCH_TARGET_PRESET_PREV,
    TOUCH_TARGET_PRESET_NEXT
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
  bool lastTouchInsideEffectDetail = false;

  bool lastTouchInsideColor = false;
  bool lastTouchInsidePresetOpen = false;

  bool lastTouchInsideBack = false;

  bool lastTouchInsideHue = false;
  bool lastTouchInsideSaturation = false;

  bool lastTouchInsideSpeed = false;
  bool lastTouchInsideIntensity = false;

  bool lastTouchInsidePalette = false;

  bool lastTouchInsidePresetNav = false;

  // =========================================================
  // Visual pressed state
  // =========================================================

  bool powerButtonVisualPressed = false;

  bool brightnessButtonVisualPressed = false;

  bool effectButtonVisualPressed = false;
  bool effectDetailVisualPressed = false;

  bool colorButtonVisualPressed = false;
  bool presetOpenButtonVisualPressed = false;

  bool backButtonVisualPressed = false;

  bool hueButtonVisualPressed = false;
  bool saturationButtonVisualPressed = false;

  bool speedButtonVisualPressed = false;
  bool intensityButtonVisualPressed = false;

  bool paletteButtonVisualPressed = false;

  bool presetNavButtonVisualPressed = false;

  // =========================================================
  // Long press state
  // =========================================================

  bool brightnessLongPressActive = false;
  bool effectLongPressActive = false;

  bool hueLongPressActive = false;
  bool saturationLongPressActive = false;

  bool speedLongPressActive = false;
  bool intensityLongPressActive = false;

  bool paletteLongPressActive = false;

  bool presetLongPressActive = false;

  int16_t lastTouchX = -1;
  int16_t lastTouchY = -1;

  // =========================================================
  // Brightness timing
  // =========================================================

  unsigned long controlPressStartTime = 0;
  unsigned long lastBrightnessRepeat = 0;

  // =========================================================
  // Effect timing
  // =========================================================

  unsigned long effectPressStartTime = 0;
  unsigned long lastEffectRepeat = 0;

  // =========================================================
  // Hue timing
  // =========================================================

  unsigned long huePressStartTime = 0;
  unsigned long lastHueRepeat = 0;

  // =========================================================
  // Saturation timing
  // =========================================================

  unsigned long saturationPressStartTime = 0;
  unsigned long lastSaturationRepeat = 0;

  // =========================================================
  // Speed timing
  // =========================================================

  unsigned long speedPressStartTime = 0;
  unsigned long lastSpeedRepeat = 0;

  // =========================================================
  // Intensity timing
  // =========================================================

  unsigned long intensityPressStartTime = 0;
  unsigned long lastIntensityRepeat = 0;

  // =========================================================
  // Palette timing
  // =========================================================

  unsigned long palettePressStartTime = 0;
  unsigned long lastPaletteRepeat = 0;

  // =========================================================
  // Phase 10.1
  // Preset timing
  // =========================================================

  unsigned long presetPressStartTime = 0;
  unsigned long lastPresetRepeat = 0;

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

  // =========================================================
  // MAIN layout
  // =========================================================

  static constexpr int16_t BRI_BUTTON_Y = 82;
  static constexpr int16_t FX_BUTTON_Y = 138;

  static constexpr int16_t EFFECT_DETAIL_X = 88;
  static constexpr int16_t EFFECT_DETAIL_Y = 138;
  static constexpr int16_t EFFECT_DETAIL_W = 144;
  static constexpr int16_t EFFECT_DETAIL_H = 34;

  // =========================================================
  // Phase 10.1
  // MAIN bottom visible buttons
  // =========================================================

  static constexpr int16_t MAIN_BOTTOM_BUTTON_Y = 188;
  static constexpr int16_t MAIN_BOTTOM_BUTTON_H = 40;

  static constexpr int16_t COLOR_BUTTON_X = 16;
  static constexpr int16_t COLOR_BUTTON_W = 140;

  static constexpr int16_t PRESET_OPEN_BUTTON_X = 164;
  static constexpr int16_t PRESET_OPEN_BUTTON_W = 140;

  // =========================================================
  // Phase 10.1
  // MAIN bottom invisible touch areas
  // =========================================================

  static constexpr int16_t MAIN_BOTTOM_TOUCH_Y = 180;
  static constexpr int16_t MAIN_BOTTOM_TOUCH_H = 60;

  static constexpr int16_t COLOR_TOUCH_X = 8;
  static constexpr int16_t COLOR_TOUCH_W = 152;

  static constexpr int16_t PRESET_OPEN_TOUCH_X = 160;
  static constexpr int16_t PRESET_OPEN_TOUCH_W = 152;

  // =========================================================
  // Back button
  // =========================================================

  static constexpr int16_t BACK_BUTTON_X = 268;
  static constexpr int16_t BACK_BUTTON_Y = 8;
  static constexpr int16_t BACK_BUTTON_W = 44;
  static constexpr int16_t BACK_BUTTON_H = 44;

  // =========================================================
  // COLOR layout
  // =========================================================

  static constexpr int16_t COLOR_PREVIEW_X = 92;
  static constexpr int16_t COLOR_PREVIEW_Y = 68;
  static constexpr int16_t COLOR_PREVIEW_W = 136;
  static constexpr int16_t COLOR_PREVIEW_H = 36;

  static constexpr int16_t HUE_BUTTON_Y = 151;

  static constexpr int16_t SATURATION_LABEL_Y = 198;
  static constexpr int16_t SATURATION_BUTTON_Y = 204;

  // =========================================================
  // EFFECT detail layout
  // =========================================================

  static constexpr int16_t SPEED_BUTTON_Y = 82;

  static constexpr int16_t INTENSITY_BUTTON_Y = 140;

  // =========================================================
  // Palette visible layout
  // =========================================================

  static constexpr int16_t PALETTE_LABEL_Y = 188;
  static constexpr int16_t PALETTE_BUTTON_Y = 198;

  // =========================================================
  // Phase 9.2.1
  // Palette invisible touch areas
  // =========================================================

  static constexpr int16_t PALETTE_TOUCH_LEFT_X = 8;
  static constexpr int16_t PALETTE_TOUCH_RIGHT_X = 232;

  static constexpr int16_t PALETTE_TOUCH_Y = 188;

  static constexpr int16_t PALETTE_TOUCH_W = 80;
  static constexpr int16_t PALETTE_TOUCH_H = 52;

  // =========================================================
  // Phase 10.1
  // PRESET screen layout
  // =========================================================

  static constexpr int16_t PRESET_NAME_Y = 92;
  static constexpr int16_t PRESET_ID_Y = 128;
  static constexpr int16_t PRESET_STATUS_Y = 154;

  static constexpr int16_t PRESET_NAV_LABEL_Y = 188;
  static constexpr int16_t PRESET_NAV_BUTTON_Y = 198;

  // =========================================================
  // Phase 10.1
  // PRESET bottom invisible touch areas
  // =========================================================

  static constexpr int16_t PRESET_NAV_TOUCH_LEFT_X = 8;
  static constexpr int16_t PRESET_NAV_TOUCH_RIGHT_X = 232;

  static constexpr int16_t PRESET_NAV_TOUCH_Y = 188;

  static constexpr int16_t PRESET_NAV_TOUCH_W = 80;
  static constexpr int16_t PRESET_NAV_TOUCH_H = 52;

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
  // Effect behavior
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
  // Speed behavior
  // =========================================================

  static constexpr unsigned long SPEED_LONG_PRESS_MS = 400;
  static constexpr unsigned long SPEED_REPEAT_MS = 80;

  static constexpr int SPEED_SHORT_STEP = 1;
  static constexpr int SPEED_LONG_STEP = 5;

  // =========================================================
  // Intensity behavior
  // =========================================================

  static constexpr unsigned long INTENSITY_LONG_PRESS_MS = 400;
  static constexpr unsigned long INTENSITY_REPEAT_MS = 80;

  static constexpr int INTENSITY_SHORT_STEP = 1;
  static constexpr int INTENSITY_LONG_STEP = 5;

  // =========================================================
  // Palette behavior
  // =========================================================

  static constexpr unsigned long PALETTE_LONG_PRESS_MS = 400;
  static constexpr unsigned long PALETTE_REPEAT_MS = 250;

  // =========================================================
  // Preset behavior
  // =========================================================

  static constexpr unsigned long PRESET_LONG_PRESS_MS = 400;
  static constexpr unsigned long PRESET_REPEAT_MS = 600;

  // =========================================================
  // Normal LCD brightness
  // =========================================================

  uint8_t getNormalDisplayBrightness()
  {
    return
      (uint8_t)constrain(
        (int)lcdBrightness,
        1,
        255
      );
  }

  // =========================================================
  // Sleep timeout
  // =========================================================

  unsigned long getSleepTimeoutMs()
  {
    return
      (unsigned long)sleepTimeoutSec *
      1000UL;
  }

  // =========================================================
  // Fade interval calculation
  // =========================================================

  unsigned long getFadeIntervalMs()
  {
    uint16_t normalBrightness =
      getNormalDisplayBrightness();

    uint16_t steps =
      (
        normalBrightness +
        DISPLAY_FADE_STEP -
        1
      ) /
      DISPLAY_FADE_STEP;

    if (
      steps ==
      0
    )
    {
      steps =
        1;
    }

    unsigned long interval =
      (unsigned long)fadeDurationMs /
      steps;

    if (
      interval <
      1
    )
    {
      interval =
        1;
    }

    return
      interval;
  }

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
  // Generic Fade
  // =========================================================

  bool updateFade(
    uint8_t targetBrightness,
    unsigned long now,
    unsigned long& lastFadeStep
  )
  {
    if (
      currentDisplayBrightness ==
      targetBrightness
    )
    {
      return true;
    }

    if (!fadeEnabled)
    {
      setDisplayBrightness(
        targetBrightness
      );

      return true;
    }

    unsigned long fadeInterval =
      getFadeIntervalMs();

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
        DISPLAY_FADE_STEP;

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
        DISPLAY_FADE_STEP;

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
  // Phase 10.1.1
  // Preset cache rebuild start
  //
  // IMPORTANT:
  // No filesystem scan is performed here.
  //
  // servicePresetCache() processes only one ID at a time.
  // =========================================================

  void startPresetCacheRebuild()
  {
    presetCacheCount =
      0;

    presetCacheScanId =
      1;

    presetCacheLastScanMs =
      0;

    presetCacheReady =
      false;

    presetCacheBuilding =
      true;

    presetNoEntries =
      false;

    presetCacheBuildSourceModifiedTime =
      presetsModifiedTime;

    Serial.printf(
      "[CoreS3_Display] "
      "Preset cache rebuild start "
      "(modified=%lu)\n",
      presetCacheBuildSourceModifiedTime
    );
  }

  // =========================================================
  // Phase 10.1.1
  // Preset cache rebuild complete
  // =========================================================

  void finishPresetCacheRebuild()
  {
    presetCacheBuilding =
      false;

    presetCacheReady =
      true;

    presetCacheSourceModifiedTime =
      presetCacheBuildSourceModifiedTime;

    presetNoEntries =
      (
        presetCacheCount ==
        0
      );

    Serial.printf(
      "[CoreS3_Display] "
      "Preset cache ready: %u preset(s)\n",
      (unsigned)presetCacheCount
    );

    // -------------------------------------------------------
    // Presets changed again while the cache was being built.
    //
    // Start again rather than publishing stale data.
    // -------------------------------------------------------

    if (
      presetsModifiedTime !=
      presetCacheSourceModifiedTime
    )
    {
      Serial.println(
        F(
          "[CoreS3_Display] "
          "Preset changed during cache build. Rebuilding."
        )
      );

      startPresetCacheRebuild();

      return;
    }

    // -------------------------------------------------------
    // If PRESET screen is currently visible, refresh it.
    // -------------------------------------------------------

    if (
      currentPage ==
        SCREEN_PRESET &&
      displayPowerState ==
        DISPLAY_POWER_ACTIVE &&
      touchTarget ==
        TOUCH_TARGET_NONE
    )
    {
      drawPresetDetails(
        getDisplayedPresetId(),
        pendingPresetId > 0
      );

      drawPresetNavigation(
        TOUCH_TARGET_NONE
      );

      lastPresetValue =
        getDisplayedPresetId();
    }
  }

  // =========================================================
  // Phase 10.1.1
  // Background Preset cache service
  //
  // One Preset ID is checked per service interval.
  //
  // This means:
  //
  // Phase 10.1
  //   Arrow:
  //     ID 3 -> scan 4..250 -> 1
  //
  // Phase 10.1.1
  //   Background:
  //     scan once
  //
  //   Arrow:
  //     RAM cache index only
  // =========================================================

  void servicePresetCache()
  {
    // -------------------------------------------------------
    // If WLED Presets changed after a completed build,
    // start a new background rebuild.
    // -------------------------------------------------------

    if (
      presetCacheReady &&
      !presetCacheBuilding &&
      presetsModifiedTime !=
        presetCacheSourceModifiedTime
    )
    {
      startPresetCacheRebuild();
    }

    if (!presetCacheBuilding)
    {
      return;
    }

    // -------------------------------------------------------
    // Do not query presets.json while a Preset is currently
    // waiting for WLED's asynchronous Preset load.
    //
    // This reduces contention for WLED's shared JSON buffer.
    // -------------------------------------------------------

    if (
      pendingPresetId >
      0
    )
    {
      return;
    }

    unsigned long now =
      millis();

    if (
      now -
      presetCacheLastScanMs <
      PRESET_CACHE_SCAN_INTERVAL_MS
    )
    {
      return;
    }

    presetCacheLastScanMs =
      now;

    if (
      presetCacheScanId >
      250
    )
    {
      finishPresetCacheRebuild();

      return;
    }

    String presetName;

    uint8_t scanId =
      (uint8_t)presetCacheScanId;

    if (
      getPresetName(
        scanId,
        presetName
      )
    )
    {
      if (
        presetCacheCount <
        250
      )
      {
        presetCache[
          presetCacheCount
        ].id =
          scanId;

        strlcpy(
          presetCache[
            presetCacheCount
          ].name,
          presetName.c_str(),
          sizeof(
            presetCache[
              presetCacheCount
            ].name
          )
        );

        presetCacheCount++;
      }
    }

    presetCacheScanId++;

    if (
      presetCacheScanId >
      250
    )
    {
      finishPresetCacheRebuild();
    }
  }

  // =========================================================
  // Phase 10.1.1
  // Find Preset in RAM cache
  // =========================================================

  int findPresetCacheIndex(
    uint8_t presetId
  )
  {
    for (
      uint16_t i = 0;
      i < presetCacheCount;
      i++
    )
    {
      if (
        presetCache[i].id ==
        presetId
      )
      {
        return
          (int)i;
      }
    }

    return -1;
  }

  // =========================================================
  // Phase 10.1.1
  // Get name from RAM cache
  // =========================================================

  bool getCachedPresetName(
    uint8_t presetId,
    String& name
  )
  {
    int index =
      findPresetCacheIndex(
        presetId
      );

    if (
      index <
      0
    )
    {
      return false;
    }

    name =
      presetCache[
        index
      ].name;

    return true;
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

    if (
      startupState ==
      STARTUP_FADE_IN
    )
    {
      if (
        updateFade(
          getNormalDisplayBrightness(),
          now,
          startupLastFadeStep
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

    if (
      startupState ==
      STARTUP_FADE_OUT
    )
    {
      if (
        updateFade(
          0,
          now,
          startupLastFadeStep
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

    if (
      startupState ==
      STARTUP_MAIN_FADE_IN
    )
    {
      if (
        updateFade(
          getNormalDisplayBrightness(),
          now,
          startupLastFadeStep
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
  // Pending Preset helper
  // =========================================================

  bool settlePendingPreset()
  {
    if (
      pendingPresetId ==
      0
    )
    {
      return false;
    }

    bool completed =
      (
        currentPreset ==
        pendingPresetId
      );

    bool timedOut =
      (
        millis() -
        pendingPresetRequestMs >=
        PRESET_APPLY_PENDING_MS
      );

    if (
      !completed &&
      !timedOut
    )
    {
      return false;
    }

    pendingPresetId =
      0;

    pendingPresetName =
      "";

    pendingPresetRequestMs =
      0;

    return true;
  }

  // =========================================================
  // Preset ID displayed by CoreS3
  // =========================================================

  uint8_t getDisplayedPresetId()
  {
    if (
      pendingPresetId >
      0
    )
    {
      if (
        millis() -
        pendingPresetRequestMs <
        PRESET_APPLY_PENDING_MS
      )
      {
        return
          pendingPresetId;
      }
    }

    return
      currentPreset;
  }

  // =========================================================
  // Redraw current page before Wake
  // =========================================================

  void redrawCurrentPageForWake()
  {
    settlePendingPreset();

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

      return;
    }

    if (
      currentPage ==
      SCREEN_EFFECT
    )
    {
      drawEffectDetailScreen();

      return;
    }

    if (
      currentPage ==
      SCREEN_PRESET
    )
    {
      drawPresetScreen();

      return;
    }

    drawMainScreen(
      WiFi.localIP().toString()
    );
  }

  // =========================================================
  // Begin display sleep
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
  // Begin display wake
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

    if (
      displayPowerState ==
      DISPLAY_POWER_ACTIVE
    )
    {
      if (
        sleepTimeoutSec >
          0 &&
        !touchActive &&
        now -
          lastUserActivityMs >=
          getSleepTimeoutMs()
      )
      {
        beginDisplaySleep(
          now
        );

        return true;
      }

      return false;
    }

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
          displayFadeLastStep
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
          getNormalDisplayBrightness(),
          now,
          displayFadeLastStep
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
  // Effect mode
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
  // Effect Speed
  // =========================================================

  uint8_t getCurrentSpeed()
  {
    if (
      strip.getSegmentsNum() >
      0
    )
    {
      return
        strip.getMainSegment().speed;
    }

    return 0;
  }

  // =========================================================
  // Effect Intensity
  // =========================================================

  uint8_t getCurrentIntensity()
  {
    if (
      strip.getSegmentsNum() >
      0
    )
    {
      return
        strip.getMainSegment().intensity;
    }

    return 0;
  }

  // =========================================================
  // Current Palette
  // =========================================================

  uint8_t getCurrentPalette()
  {
    if (
      strip.getSegmentsNum() >
      0
    )
    {
      return
        strip.getMainSegment().palette;
    }

    return 0;
  }

  // =========================================================
  // Palette count
  // =========================================================

  size_t getSelectablePaletteCount()
  {
    return
      FIXED_PALETTE_COUNT +
      customPalettes.size() +
      usermodPalettes.size();
  }

  // =========================================================
  // Logical Palette index -> WLED Palette ID
  // =========================================================

  uint8_t paletteIdFromSequenceIndex(
    size_t sequenceIndex
  )
  {
    if (
      sequenceIndex <
      FIXED_PALETTE_COUNT
    )
    {
      return
        (uint8_t)sequenceIndex;
    }

    sequenceIndex -=
      FIXED_PALETTE_COUNT;

    if (
      sequenceIndex <
      customPalettes.size()
    )
    {
      return
        (uint8_t)(
          WLED_CUSTOM_PALETTE_ID_BASE -
          sequenceIndex
        );
    }

    sequenceIndex -=
      customPalettes.size();

    if (
      sequenceIndex <
      usermodPalettes.size()
    )
    {
      return
        (uint8_t)(
          WLED_USERMOD_PALETTE_ID_BASE -
          sequenceIndex
        );
    }

    return 0;
  }

  // =========================================================
  // WLED Palette ID -> Logical Palette index
  // =========================================================

  int findPaletteSequenceIndex(
    uint8_t paletteId
  )
  {
    if (
      paletteId <
      FIXED_PALETTE_COUNT
    )
    {
      return
        paletteId;
    }

    if (
      paletteId >
      WLED_CUSTOM_PALETTE_ID_BASE
    )
    {
      size_t usermodIndex =
        WLED_USERMOD_PALETTE_ID_BASE -
        paletteId;

      if (
        usermodIndex <
        usermodPalettes.size()
      )
      {
        return
          (int)(
            FIXED_PALETTE_COUNT +
            customPalettes.size() +
            usermodIndex
          );
      }

      return -1;
    }

    if (
      paletteId >=
      FIXED_PALETTE_COUNT &&
      paletteId <=
      WLED_CUSTOM_PALETTE_ID_BASE
    )
    {
      size_t customIndex =
        WLED_CUSTOM_PALETTE_ID_BASE -
        paletteId;

      if (
        customIndex <
        customPalettes.size()
      )
      {
        return
          (int)(
            FIXED_PALETTE_COUNT +
            customIndex
          );
      }

      return -1;
    }

    return -1;
  }

  // =========================================================
  // Palette name
  // =========================================================

  void getPaletteName(
    uint8_t paletteId,
    char* paletteName,
    size_t paletteNameSize
  )
  {
    if (
      paletteName == nullptr ||
      paletteNameSize == 0
    )
    {
      return;
    }

    paletteName[0] =
      '\0';

    extractModeName(
      paletteId,
      JSON_palette_names,
      paletteName,
      paletteNameSize - 1
    );

    if (
      strlen(paletteName) ==
      0
    )
    {
      snprintf(
        paletteName,
        paletteNameSize,
        "Palette %u",
        paletteId
      );
    }
  }

  // =========================================================
  // Phase 10.1.1
  // Find next / previous existing Preset
  //
  // IMPORTANT:
  //
  // Phase 10.1:
  //   getPresetName() was repeatedly called while navigating.
  //
  // Phase 10.1.1:
  //   ONLY presetCache[] is searched here.
  //
  // No filesystem access occurs during arrow operation.
  // =========================================================

  uint8_t findAdjacentPreset(
    uint8_t startPreset,
    int direction,
    String* foundName = nullptr
  )
  {
    if (
      direction ==
      0
    )
    {
      return 0;
    }

    if (
      !presetCacheReady ||
      presetCacheCount ==
        0
    )
    {
      return 0;
    }

    int currentIndex =
      findPresetCacheIndex(
        startPreset
      );

    int newIndex;

    // -------------------------------------------------------
    // Custom State / active Preset is not in cache.
    //
    // NEXT:
    //   First saved Preset
    //
    // PREVIOUS:
    //   Last saved Preset
    // -------------------------------------------------------

    if (
      currentIndex <
      0
    )
    {
      if (
        direction >
        0
      )
      {
        newIndex =
          0;
      }
      else
      {
        newIndex =
          presetCacheCount -
          1;
      }
    }
    else
    {
      newIndex =
        currentIndex +
        (
          direction >
          0
            ? 1
            : -1
        );

      if (
        newIndex >=
        presetCacheCount
      )
      {
        newIndex =
          0;
      }

      if (
        newIndex <
        0
      )
      {
        newIndex =
          presetCacheCount -
          1;
      }
    }

    if (
      foundName !=
      nullptr
    )
    {
      *foundName =
        presetCache[
          newIndex
        ].name;
    }

    return
      presetCache[
        newIndex
      ].id;
  }

  // =========================================================
  // Preset navigation base
  // =========================================================

  uint8_t getPresetNavigationBaseId()
  {
    if (
      pendingPresetId >
      0 &&
      millis() -
        pendingPresetRequestMs <
        PRESET_APPLY_PENDING_MS
    )
    {
      return
        pendingPresetId;
    }

    return
      currentPreset;
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
  // Effect Detail button
  // =========================================================

  void drawEffectDetailButton(
    uint8_t effectMode,
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

    display.fillRect(
      EFFECT_DETAIL_X - 2,
      EFFECT_DETAIL_Y - 2,
      EFFECT_DETAIL_W + 4,
      EFFECT_DETAIL_H + 4,
      TFT_BLACK
    );

    display.fillRect(
      EFFECT_DETAIL_X,
      EFFECT_DETAIL_Y,
      EFFECT_DETAIL_W,
      EFFECT_DETAIL_H,
      backgroundColor
    );

    display.drawRect(
      EFFECT_DETAIL_X,
      EFFECT_DETAIL_Y,
      EFFECT_DETAIL_W,
      EFFECT_DETAIL_H,
      buttonColor
    );

    display.drawRect(
      EFFECT_DETAIL_X + 1,
      EFFECT_DETAIL_Y + 1,
      EFFECT_DETAIL_W - 2,
      EFFECT_DETAIL_H - 2,
      buttonColor
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
      textColor,
      backgroundColor
    );

    if (
      strlen(effectName) <=
      10
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
      EFFECT_DETAIL_X +
        (EFFECT_DETAIL_W / 2),
      EFFECT_DETAIL_Y +
        (EFFECT_DETAIL_H / 2)
    );

    effectDetailVisualPressed =
      pressed;
  }

  // =========================================================
  // Effect row
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

    drawEffectDetailButton(
      effectMode,
      pressedTarget ==
        TOUCH_TARGET_EFFECT_DETAIL
    );

    drawTriangleButton(
      CONTROL_RIGHT_X,
      FX_BUTTON_Y,
      true,
      pressedTarget ==
        TOUCH_TARGET_EFFECT_NEXT
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

    uint16_t previewColor =
      rgbTo565(
        R(color),
        G(color),
        B(color)
      );

    display.fillRect(
      COLOR_BUTTON_X - 2,
      MAIN_BOTTOM_BUTTON_Y - 2,
      COLOR_BUTTON_W + 4,
      MAIN_BOTTOM_BUTTON_H + 4,
      TFT_BLACK
    );

    display.fillRect(
      COLOR_BUTTON_X,
      MAIN_BOTTOM_BUTTON_Y,
      COLOR_BUTTON_W,
      MAIN_BOTTOM_BUTTON_H,
      backgroundColor
    );

    display.drawRect(
      COLOR_BUTTON_X,
      MAIN_BOTTOM_BUTTON_Y,
      COLOR_BUTTON_W,
      MAIN_BOTTOM_BUTTON_H,
      buttonColor
    );

    display.drawRect(
      COLOR_BUTTON_X + 1,
      MAIN_BOTTOM_BUTTON_Y + 1,
      COLOR_BUTTON_W - 2,
      MAIN_BOTTOM_BUTTON_H - 2,
      buttonColor
    );

    static constexpr int16_t SWATCH_X = 28;
    static constexpr int16_t SWATCH_W = 24;
    static constexpr int16_t SWATCH_H = 24;

    int16_t swatchY =
      MAIN_BOTTOM_BUTTON_Y +
      (
        (
          MAIN_BOTTOM_BUTTON_H -
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
      105,
      MAIN_BOTTOM_BUTTON_Y +
        (MAIN_BOTTOM_BUTTON_H / 2)
    );

    colorButtonVisualPressed =
      pressed;
  }

  // =========================================================
  // MAIN Preset button
  // =========================================================

  void drawPresetOpenButton(
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

    display.fillRect(
      PRESET_OPEN_BUTTON_X - 2,
      MAIN_BOTTOM_BUTTON_Y - 2,
      PRESET_OPEN_BUTTON_W + 4,
      MAIN_BOTTOM_BUTTON_H + 4,
      TFT_BLACK
    );

    display.fillRect(
      PRESET_OPEN_BUTTON_X,
      MAIN_BOTTOM_BUTTON_Y,
      PRESET_OPEN_BUTTON_W,
      MAIN_BOTTOM_BUTTON_H,
      backgroundColor
    );

    display.drawRect(
      PRESET_OPEN_BUTTON_X,
      MAIN_BOTTOM_BUTTON_Y,
      PRESET_OPEN_BUTTON_W,
      MAIN_BOTTOM_BUTTON_H,
      buttonColor
    );

    display.drawRect(
      PRESET_OPEN_BUTTON_X + 1,
      MAIN_BOTTOM_BUTTON_Y + 1,
      PRESET_OPEN_BUTTON_W - 2,
      MAIN_BOTTOM_BUTTON_H - 2,
      buttonColor
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
      "PRESET",
      PRESET_OPEN_BUTTON_X +
        (PRESET_OPEN_BUTTON_W / 2),
      MAIN_BOTTOM_BUTTON_Y +
        (MAIN_BOTTOM_BUTTON_H / 2)
    );

    presetOpenButtonVisualPressed =
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
  // Speed row
  // =========================================================

  void drawSpeed(
    uint8_t speedValue,
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
      "Speed",
      screenWidth / 2,
      70
    );

    drawTriangleButton(
      CONTROL_LEFT_X,
      SPEED_BUTTON_Y,
      false,
      pressedTarget ==
        TOUCH_TARGET_SPEED_DOWN
    );

    drawTriangleButton(
      CONTROL_RIGHT_X,
      SPEED_BUTTON_Y,
      true,
      pressedTarget ==
        TOUCH_TARGET_SPEED_UP
    );

    char valueText[8];

    snprintf(
      valueText,
      sizeof(valueText),
      "%u",
      speedValue
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
  // Intensity row
  // =========================================================

  void drawIntensity(
    uint8_t intensityValue,
    TouchTarget pressedTarget =
      TOUCH_TARGET_NONE
  )
  {
    display.fillRect(
      0,
      120,
      screenWidth,
      60,
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
      "Intensity",
      screenWidth / 2,
      128
    );

    drawTriangleButton(
      CONTROL_LEFT_X,
      INTENSITY_BUTTON_Y,
      false,
      pressedTarget ==
        TOUCH_TARGET_INTENSITY_DOWN
    );

    drawTriangleButton(
      CONTROL_RIGHT_X,
      INTENSITY_BUTTON_Y,
      true,
      pressedTarget ==
        TOUCH_TARGET_INTENSITY_UP
    );

    char valueText[8];

    snprintf(
      valueText,
      sizeof(valueText),
      "%u",
      intensityValue
    );

    display.setTextSize(
      2
    );

    display.drawString(
      valueText,
      screenWidth / 2,
      157
    );
  }

  // =========================================================
  // Palette row
  // =========================================================

  void drawPalette(
    uint8_t paletteId,
    TouchTarget pressedTarget =
      TOUCH_TARGET_NONE
  )
  {
    display.fillRect(
      0,
      180,
      screenWidth,
      60,
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
      "Palette",
      screenWidth / 2,
      PALETTE_LABEL_Y
    );

    drawTriangleButton(
      CONTROL_LEFT_X,
      PALETTE_BUTTON_Y,
      false,
      pressedTarget ==
        TOUCH_TARGET_PALETTE_PREV
    );

    drawTriangleButton(
      CONTROL_RIGHT_X,
      PALETTE_BUTTON_Y,
      true,
      pressedTarget ==
        TOUCH_TARGET_PALETTE_NEXT
    );

    char paletteName[64];

    getPaletteName(
      paletteId,
      paletteName,
      sizeof(paletteName)
    );

    if (
      strlen(paletteName) >
      22
    )
    {
      paletteName[22] =
        '\0';
    }

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    if (
      strlen(paletteName) <=
      10
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
      paletteName,
      screenWidth / 2,
      PALETTE_BUTTON_Y +
        (CONTROL_BUTTON_H / 2)
    );
  }

  // =========================================================
  // EFFECT page effect name
  // =========================================================

  void drawEffectPageName(
    uint8_t effectMode
  )
  {
    display.fillRect(
      56,
      32,
      208,
      22,
      TFT_BLACK
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

    display.setTextSize(
      1
    );

    display.drawString(
      effectName,
      screenWidth / 2,
      42
    );
  }

  // =========================================================
  // Phase 10.1.1
  // Preset name helper
  //
  // IMPORTANT:
  // Do not call getPresetName() here.
  //
  // Navigation/display uses RAM cache.
  // =========================================================

  String getPresetDisplayName(
    uint8_t presetId
  )
  {
    if (
      presetId ==
      0
    )
    {
      return
        "Custom State";
    }

    if (
      pendingPresetId ==
        presetId &&
      pendingPresetName.length() >
        0
    )
    {
      return
        pendingPresetName;
    }

    String name;

    if (
      getCachedPresetName(
        presetId,
        name
      )
    )
    {
      return
        name;
    }

    char fallback[24];

    snprintf(
      fallback,
      sizeof(fallback),
      "Preset %u",
      presetId
    );

    return
      String(fallback);
  }

  // =========================================================
  // Preset details
  // =========================================================

  void drawPresetDetails(
    uint8_t presetId,
    bool applying = false
  )
  {
    display.fillRect(
      0,
      60,
      screenWidth,
      118,
      TFT_BLACK
    );

    display.setTextDatum(
      textdatum_t::middle_center
    );

    // -------------------------------------------------------
    // Phase 10.1.1
    // First cache build is still running.
    // -------------------------------------------------------

    if (
      !presetCacheReady
    )
    {
      display.setTextColor(
        TFT_YELLOW,
        TFT_BLACK
      );

      display.setTextSize(
        2
      );

      display.drawString(
        "Loading Presets",
        screenWidth / 2,
        PRESET_NAME_Y
      );

      display.setTextColor(
        TFT_DARKGREY,
        TFT_BLACK
      );

      display.setTextSize(
        1
      );

      char progressText[32];

      uint16_t displayScanId =
        presetCacheScanId;

      if (
        displayScanId >
        250
      )
      {
        displayScanId =
          250;
      }

      snprintf(
        progressText,
        sizeof(progressText),
        "Scanning ID: %u / 250",
        (unsigned)displayScanId
      );

      display.drawString(
        progressText,
        screenWidth / 2,
        PRESET_ID_Y
      );

      display.drawString(
        "WLED remains active",
        screenWidth / 2,
        PRESET_STATUS_Y
      );

      return;
    }

    // -------------------------------------------------------
    // No Presets found
    // -------------------------------------------------------

    if (
      presetNoEntries
    )
    {
      display.setTextColor(
        TFT_RED,
        TFT_BLACK
      );

      display.setTextSize(
        2
      );

      display.drawString(
        "No Presets",
        screenWidth / 2,
        PRESET_NAME_Y
      );

      display.setTextColor(
        TFT_DARKGREY,
        TFT_BLACK
      );

      display.setTextSize(
        1
      );

      display.drawString(
        "No saved preset found",
        screenWidth / 2,
        PRESET_ID_Y
      );

      display.drawString(
        "Create presets in WLED",
        screenWidth / 2,
        PRESET_STATUS_Y
      );

      return;
    }

    // -------------------------------------------------------
    // Custom State
    // -------------------------------------------------------

    if (
      presetId ==
      0
    )
    {
      display.setTextColor(
        TFT_WHITE,
        TFT_BLACK
      );

      display.setTextSize(
        2
      );

      display.drawString(
        "Custom State",
        screenWidth / 2,
        PRESET_NAME_Y
      );

      display.setTextColor(
        TFT_DARKGREY,
        TFT_BLACK
      );

      display.setTextSize(
        1
      );

      display.drawString(
        "No active preset",
        screenWidth / 2,
        PRESET_ID_Y
      );

      display.drawString(
        "Use arrows to apply",
        screenWidth / 2,
        PRESET_STATUS_Y
      );

      return;
    }

    // -------------------------------------------------------
    // Active / pending Preset
    // -------------------------------------------------------

    String presetName =
      getPresetDisplayName(
        presetId
      );

    if (
      presetName.length() >
      28
    )
    {
      presetName =
        presetName.substring(
          0,
          25
        ) +
        "...";
    }

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    if (
      presetName.length() <=
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
      presetName,
      screenWidth / 2,
      PRESET_NAME_Y
    );

    char idText[24];

    snprintf(
      idText,
      sizeof(idText),
      "Preset ID: %u",
      presetId
    );

    display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    display.setTextSize(
      1
    );

    display.drawString(
      idText,
      screenWidth / 2,
      PRESET_ID_Y
    );

    if (applying)
    {
      display.setTextColor(
        TFT_YELLOW,
        TFT_BLACK
      );

      display.drawString(
        "Applying...",
        screenWidth / 2,
        PRESET_STATUS_Y
      );
    }
    else if (
      currentPreset ==
      presetId
    )
    {
      display.setTextColor(
        TFT_GREEN,
        TFT_BLACK
      );

      display.drawString(
        "Active Preset",
        screenWidth / 2,
        PRESET_STATUS_Y
      );
    }
    else
    {
      display.setTextColor(
        TFT_DARKGREY,
        TFT_BLACK
      );

      display.drawString(
        "Saved WLED Preset",
        screenWidth / 2,
        PRESET_STATUS_Y
      );
    }
  }

  // =========================================================
  // Preset bottom navigation
  // =========================================================

  void drawPresetNavigation(
    TouchTarget pressedTarget =
      TOUCH_TARGET_NONE
  )
  {
    display.fillRect(
      0,
      178,
      screenWidth,
      62,
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
      "Preset",
      screenWidth / 2,
      PRESET_NAV_LABEL_Y
    );

    drawTriangleButton(
      CONTROL_LEFT_X,
      PRESET_NAV_BUTTON_Y,
      false,
      pressedTarget ==
        TOUCH_TARGET_PRESET_PREV
    );

    drawTriangleButton(
      CONTROL_RIGHT_X,
      PRESET_NAV_BUTTON_Y,
      true,
      pressedTarget ==
        TOUCH_TARGET_PRESET_NEXT
    );

    display.setTextColor(
      TFT_DARKGREY,
      TFT_BLACK
    );

    display.setTextSize(
      1
    );

    if (
      presetCacheReady
    )
    {
      display.drawString(
        "APPLY",
        screenWidth / 2,
        PRESET_NAV_BUTTON_Y +
          (CONTROL_BUTTON_H / 2)
      );
    }
    else
    {
      display.drawString(
        "LOADING",
        screenWidth / 2,
        PRESET_NAV_BUTTON_Y +
          (CONTROL_BUTTON_H / 2)
      );
    }
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

    drawPresetOpenButton(
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

    lastSpeedValue =
      getCurrentSpeed();

    lastIntensityValue =
      getCurrentIntensity();

    lastPaletteValue =
      getCurrentPalette();

    lastPresetValue =
      currentPreset;

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
  // EFFECT detail screen
  // =========================================================

  void drawEffectDetailScreen()
  {
    display.fillScreen(
      TFT_BLACK
    );

    currentPage =
      SCREEN_EFFECT;

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
      "EFFECT",
      screenWidth / 2,
      18
    );

    uint8_t effectMode =
      getCurrentEffectMode();

    drawEffectPageName(
      effectMode
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

    uint8_t speedValue =
      getCurrentSpeed();

    uint8_t intensityValue =
      getCurrentIntensity();

    uint8_t paletteValue =
      getCurrentPalette();

    drawSpeed(
      speedValue,
      TOUCH_TARGET_NONE
    );

    drawIntensity(
      intensityValue,
      TOUCH_TARGET_NONE
    );

    drawPalette(
      paletteValue,
      TOUCH_TARGET_NONE
    );

    lastLedState =
      bri > 0
        ? 1
        : 0;

    lastEffectMode =
      effectMode;

    lastSpeedValue =
      speedValue;

    lastIntensityValue =
      intensityValue;

    lastPaletteValue =
      paletteValue;
  }

  // =========================================================
  // PRESET screen
  // =========================================================

  void drawPresetScreen()
  {
    display.fillScreen(
      TFT_BLACK
    );

    currentPage =
      SCREEN_PRESET;

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
      "PRESET",
      screenWidth / 2,
      18
    );

    display.setTextSize(
      1
    );

    display.drawString(
      "Saved WLED Preset",
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

    uint8_t presetId =
      getDisplayedPresetId();

    bool applying =
      (
        pendingPresetId >
        0
      );

    drawPresetDetails(
      presetId,
      applying
    );

    drawPresetNavigation(
      TOUCH_TARGET_NONE
    );

    lastLedState =
      bri > 0
        ? 1
        : 0;

    lastPresetValue =
      presetId;

    lastPresetsModifiedTime =
      presetsModifiedTime;
  }

  // =========================================================
  // Hit test helper
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

  bool isEffectDetailTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      EFFECT_DETAIL_X,
      EFFECT_DETAIL_Y,
      EFFECT_DETAIL_W,
      EFFECT_DETAIL_H
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

  // =========================================================
  // MAIN Color expanded touch area
  // =========================================================

  bool isColorButtonTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      COLOR_TOUCH_X,
      MAIN_BOTTOM_TOUCH_Y,
      COLOR_TOUCH_W,
      MAIN_BOTTOM_TOUCH_H
    );
  }

  // =========================================================
  // MAIN Preset expanded touch area
  // =========================================================

  bool isPresetOpenButtonTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      PRESET_OPEN_TOUCH_X,
      MAIN_BOTTOM_TOUCH_Y,
      PRESET_OPEN_TOUCH_W,
      MAIN_BOTTOM_TOUCH_H
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

  bool isSpeedDownTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      CONTROL_LEFT_X,
      SPEED_BUTTON_Y,
      CONTROL_BUTTON_W,
      CONTROL_BUTTON_H
    );
  }

  bool isSpeedUpTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      CONTROL_RIGHT_X,
      SPEED_BUTTON_Y,
      CONTROL_BUTTON_W,
      CONTROL_BUTTON_H
    );
  }

  bool isIntensityDownTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      CONTROL_LEFT_X,
      INTENSITY_BUTTON_Y,
      CONTROL_BUTTON_W,
      CONTROL_BUTTON_H
    );
  }

  bool isIntensityUpTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      CONTROL_RIGHT_X,
      INTENSITY_BUTTON_Y,
      CONTROL_BUTTON_W,
      CONTROL_BUTTON_H
    );
  }

  // =========================================================
  // Palette expanded hit tests
  // =========================================================

  bool isPalettePrevTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      PALETTE_TOUCH_LEFT_X,
      PALETTE_TOUCH_Y,
      PALETTE_TOUCH_W,
      PALETTE_TOUCH_H
    );
  }

  bool isPaletteNextTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      PALETTE_TOUCH_RIGHT_X,
      PALETTE_TOUCH_Y,
      PALETTE_TOUCH_W,
      PALETTE_TOUCH_H
    );
  }

  // =========================================================
  // Preset expanded hit tests
  // =========================================================

  bool isPresetPrevTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      PRESET_NAV_TOUCH_LEFT_X,
      PRESET_NAV_TOUCH_Y,
      PRESET_NAV_TOUCH_W,
      PRESET_NAV_TOUCH_H
    );
  }

  bool isPresetNextTouched(
    int16_t x,
    int16_t y
  )
  {
    return pointInsideRect(
      x,
      y,
      PRESET_NAV_TOUCH_RIGHT_X,
      PRESET_NAV_TOUCH_Y,
      PRESET_NAV_TOUCH_W,
      PRESET_NAV_TOUCH_H
    );
  }

  // =========================================================
  // Begin Hue edit
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
  // Begin Saturation edit
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

    lastTouchInsideBrightness =
      false;

    lastTouchInsideEffect =
      false;

    lastTouchInsideEffectDetail =
      false;

    lastTouchInsideColor =
      false;

    lastTouchInsidePresetOpen =
      false;

    lastTouchInsideBack =
      false;

    lastTouchInsideHue =
      false;

    lastTouchInsideSaturation =
      false;

    lastTouchInsideSpeed =
      false;

    lastTouchInsideIntensity =
      false;

    lastTouchInsidePalette =
      false;

    lastTouchInsidePresetNav =
      false;

    powerButtonVisualPressed =
      false;

    brightnessButtonVisualPressed =
      false;

    effectButtonVisualPressed =
      false;

    effectDetailVisualPressed =
      false;

    colorButtonVisualPressed =
      false;

    presetOpenButtonVisualPressed =
      false;

    backButtonVisualPressed =
      false;

    hueButtonVisualPressed =
      false;

    saturationButtonVisualPressed =
      false;

    speedButtonVisualPressed =
      false;

    intensityButtonVisualPressed =
      false;

    paletteButtonVisualPressed =
      false;

    presetNavButtonVisualPressed =
      false;

    brightnessLongPressActive =
      false;

    effectLongPressActive =
      false;

    hueLongPressActive =
      false;

    saturationLongPressActive =
      false;

    speedLongPressActive =
      false;

    intensityLongPressActive =
      false;

    paletteLongPressActive =
      false;

    presetLongPressActive =
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

    speedPressStartTime =
      0;

    lastSpeedRepeat =
      0;

    intensityPressStartTime =
      0;

    lastIntensityRepeat =
      0;

    palettePressStartTime =
      0;

    lastPaletteRepeat =
      0;

    presetPressStartTime =
      0;

    lastPresetRepeat =
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

    lastSpeedValue =
      mainSegment.speed;

    lastIntensityValue =
      mainSegment.intensity;

    lastPaletteValue =
      mainSegment.palette;
  }

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
  // Speed actions
  // =========================================================

  bool applySpeedValue(
    int newValue
  )
  {
    if (
      strip.getSegmentsNum() ==
      0
    )
    {
      return false;
    }

    newValue =
      constrain(
        newValue,
        0,
        255
      );

    Segment& mainSegment =
      strip.getMainSegment();

    if (
      newValue ==
      mainSegment.speed
    )
    {
      return false;
    }

    mainSegment.speed =
      (uint8_t)newValue;

    stateUpdated(
      CALL_MODE_BUTTON
    );

    if (
      currentPage ==
      SCREEN_EFFECT
    )
    {
      drawSpeed(
        mainSegment.speed,
        touchTarget
      );
    }

    lastSpeedValue =
      mainSegment.speed;

    return true;
  }

  void applySpeedStep(
    int step
  )
  {
    int newValue =
      constrain(
        (int)getCurrentSpeed() +
        step,
        0,
        255
      );

    applySpeedValue(
      newValue
    );
  }

  void speedShortPress(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_SPEED_DOWN
    )
    {
      applySpeedStep(
        -SPEED_SHORT_STEP
      );
    }
    else if (
      target ==
      TOUCH_TARGET_SPEED_UP
    )
    {
      applySpeedStep(
        SPEED_SHORT_STEP
      );
    }
  }

  void speedLongPressStep(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_SPEED_DOWN
    )
    {
      applySpeedStep(
        -SPEED_LONG_STEP
      );
    }
    else if (
      target ==
      TOUCH_TARGET_SPEED_UP
    )
    {
      applySpeedStep(
        SPEED_LONG_STEP
      );
    }
  }

  // =========================================================
  // Intensity actions
  // =========================================================

  bool applyIntensityValue(
    int newValue
  )
  {
    if (
      strip.getSegmentsNum() ==
      0
    )
    {
      return false;
    }

    newValue =
      constrain(
        newValue,
        0,
        255
      );

    Segment& mainSegment =
      strip.getMainSegment();

    if (
      newValue ==
      mainSegment.intensity
    )
    {
      return false;
    }

    mainSegment.intensity =
      (uint8_t)newValue;

    stateUpdated(
      CALL_MODE_BUTTON
    );

    if (
      currentPage ==
      SCREEN_EFFECT
    )
    {
      drawIntensity(
        mainSegment.intensity,
        touchTarget
      );
    }

    lastIntensityValue =
      mainSegment.intensity;

    return true;
  }

  void applyIntensityStep(
    int step
  )
  {
    int newValue =
      constrain(
        (int)getCurrentIntensity() +
        step,
        0,
        255
      );

    applyIntensityValue(
      newValue
    );
  }

  void intensityShortPress(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_INTENSITY_DOWN
    )
    {
      applyIntensityStep(
        -INTENSITY_SHORT_STEP
      );
    }
    else if (
      target ==
      TOUCH_TARGET_INTENSITY_UP
    )
    {
      applyIntensityStep(
        INTENSITY_SHORT_STEP
      );
    }
  }

  void intensityLongPressStep(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_INTENSITY_DOWN
    )
    {
      applyIntensityStep(
        -INTENSITY_LONG_STEP
      );
    }
    else if (
      target ==
      TOUCH_TARGET_INTENSITY_UP
    )
    {
      applyIntensityStep(
        INTENSITY_LONG_STEP
      );
    }
  }

  // =========================================================
  // Palette actions
  // =========================================================

  bool applyPaletteValue(
    uint8_t newPalette
  )
  {
    if (
      strip.getSegmentsNum() ==
      0
    )
    {
      return false;
    }

    Segment& mainSegment =
      strip.getMainSegment();

    if (
      newPalette ==
      mainSegment.palette
    )
    {
      return false;
    }

    mainSegment.setPalette(
      newPalette
    );

    stateUpdated(
      CALL_MODE_BUTTON
    );

    if (
      currentPage ==
      SCREEN_EFFECT
    )
    {
      drawPalette(
        mainSegment.palette,
        touchTarget
      );
    }

    lastPaletteValue =
      mainSegment.palette;

    return true;
  }

  // =========================================================
  // Move Palette by logical sequence
  // =========================================================

  void applyPaletteStep(
    int step
  )
  {
    size_t paletteCount =
      getSelectablePaletteCount();

    if (
      paletteCount ==
      0
    )
    {
      return;
    }

    uint8_t currentPalette =
      getCurrentPalette();

    int currentIndex =
      findPaletteSequenceIndex(
        currentPalette
      );

    if (
      currentIndex <
      0
    )
    {
      currentIndex =
        0;
    }

    int newIndex =
      currentIndex +
      step;

    while (
      newIndex <
      0
    )
    {
      newIndex +=
        (int)paletteCount;
    }

    while (
      newIndex >=
      (int)paletteCount
    )
    {
      newIndex -=
        (int)paletteCount;
    }

    uint8_t newPalette =
      paletteIdFromSequenceIndex(
        (size_t)newIndex
      );

    applyPaletteValue(
      newPalette
    );
  }

  void paletteShortPress(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_PALETTE_PREV
    )
    {
      applyPaletteStep(
        -1
      );
    }
    else if (
      target ==
      TOUCH_TARGET_PALETTE_NEXT
    )
    {
      applyPaletteStep(
        1
      );
    }
  }

  void paletteLongPressStep(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_PALETTE_PREV
    )
    {
      applyPaletteStep(
        -1
      );
    }
    else if (
      target ==
      TOUCH_TARGET_PALETTE_NEXT
    )
    {
      applyPaletteStep(
        1
      );
    }
  }

  // =========================================================
  // Phase 10.1.1
  // Apply adjacent Preset
  //
  // No getPresetName() call occurs here.
  // Only cached IDs / names are used.
  // =========================================================

  bool applyPresetStep(
    int direction
  )
  {
    if (
      direction ==
      0
    )
    {
      return false;
    }

    // -------------------------------------------------------
    // Initial cache has not completed yet.
    // -------------------------------------------------------

    if (
      !presetCacheReady
    )
    {
      if (
        currentPage ==
        SCREEN_PRESET
      )
      {
        drawPresetDetails(
          getDisplayedPresetId(),
          false
        );

        drawPresetNavigation(
          TOUCH_TARGET_NONE
        );
      }

      Serial.println(
        F(
          "[CoreS3_Display] "
          "Preset cache not ready"
        )
      );

      return false;
    }

    uint8_t basePreset =
      getPresetNavigationBaseId();

    String presetName;

    uint8_t newPreset =
      findAdjacentPreset(
        basePreset,
        direction,
        &presetName
      );

    if (
      newPreset ==
      0
    )
    {
      presetNoEntries =
        true;

      pendingPresetId =
        0;

      pendingPresetName =
        "";

      pendingPresetRequestMs =
        0;

      if (
        currentPage ==
        SCREEN_PRESET
      )
      {
        drawPresetDetails(
          0,
          false
        );

        drawPresetNavigation(
          touchTarget
        );
      }

      lastPresetValue =
        0;

      return false;
    }

    presetNoEntries =
      false;

    pendingPresetId =
      newPreset;

    pendingPresetName =
      presetName;

    pendingPresetRequestMs =
      millis();

    applyPreset(
      newPreset,
      CALL_MODE_BUTTON_PRESET
    );

    if (
      currentPage ==
      SCREEN_PRESET
    )
    {
      drawPresetDetails(
        newPreset,
        true
      );

      drawPresetNavigation(
        touchTarget
      );
    }

    lastPresetValue =
      newPreset;

    Serial.printf(
      "[CoreS3_Display] "
      "Preset cache request: %u (%s)\n",
      newPreset,
      presetName.c_str()
    );

    return true;
  }

  void presetShortPress(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_PRESET_PREV
    )
    {
      applyPresetStep(
        -1
      );
    }
    else if (
      target ==
      TOUCH_TARGET_PRESET_NEXT
    )
    {
      applyPresetStep(
        1
      );
    }
  }

  void presetLongPressStep(
    TouchTarget target
  )
  {
    if (
      target ==
      TOUCH_TARGET_PRESET_PREV
    )
    {
      applyPresetStep(
        -1
      );
    }
    else if (
      target ==
      TOUCH_TARGET_PRESET_NEXT
    )
    {
      applyPresetStep(
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
  // Selected Saturation button
  // =========================================================

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
  // Selected Speed button
  // =========================================================

  bool isInsideSelectedSpeedButton(
    int16_t x,
    int16_t y
  )
  {
    if (
      touchTarget ==
      TOUCH_TARGET_SPEED_DOWN
    )
    {
      return
        isSpeedDownTouched(
          x,
          y
        );
    }

    if (
      touchTarget ==
      TOUCH_TARGET_SPEED_UP
    )
    {
      return
        isSpeedUpTouched(
          x,
          y
        );
    }

    return false;
  }

  // =========================================================
  // Selected Intensity button
  // =========================================================

  bool isInsideSelectedIntensityButton(
    int16_t x,
    int16_t y
  )
  {
    if (
      touchTarget ==
      TOUCH_TARGET_INTENSITY_DOWN
    )
    {
      return
        isIntensityDownTouched(
          x,
          y
        );
    }

    if (
      touchTarget ==
      TOUCH_TARGET_INTENSITY_UP
    )
    {
      return
        isIntensityUpTouched(
          x,
          y
        );
    }

    return false;
  }

  // =========================================================
  // Selected Palette button
  // =========================================================

  bool isInsideSelectedPaletteButton(
    int16_t x,
    int16_t y
  )
  {
    if (
      touchTarget ==
      TOUCH_TARGET_PALETTE_PREV
    )
    {
      return
        isPalettePrevTouched(
          x,
          y
        );
    }

    if (
      touchTarget ==
      TOUCH_TARGET_PALETTE_NEXT
    )
    {
      return
        isPaletteNextTouched(
          x,
          y
        );
    }

    return false;
  }

  // =========================================================
  // Selected Preset button
  // =========================================================

  bool isInsideSelectedPresetButton(
    int16_t x,
    int16_t y
  )
  {
    if (
      touchTarget ==
      TOUCH_TARGET_PRESET_PREV
    )
    {
      return
        isPresetPrevTouched(
          x,
          y
        );
    }

    if (
      touchTarget ==
      TOUCH_TARGET_PRESET_NEXT
    )
    {
      return
        isPresetNextTouched(
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
    // Touching
    // =======================================================

    if (touching)
    {
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

      // =====================================================
      // MAIN page hit tests
      // =====================================================

      bool insideBrightnessDown = false;
      bool insideBrightnessUp = false;

      bool insideEffectPrev = false;
      bool insideEffectDetail = false;
      bool insideEffectNext = false;

      bool insideColor = false;
      bool insidePresetOpen = false;

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

        insideEffectDetail =
          isEffectDetailTouched(
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

        insidePresetOpen =
          isPresetOpenButtonTouched(
            touchX,
            touchY
          );
      }

      // =====================================================
      // COLOR page hit tests
      // =====================================================

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

      // =====================================================
      // EFFECT page hit tests
      // =====================================================

      bool insideSpeedDown = false;
      bool insideSpeedUp = false;

      bool insideIntensityDown = false;
      bool insideIntensityUp = false;

      bool insidePalettePrev = false;
      bool insidePaletteNext = false;

      if (
        currentPage ==
        SCREEN_EFFECT
      )
      {
        insideBack =
          isBackButtonTouched(
            touchX,
            touchY
          );

        insideSpeedDown =
          isSpeedDownTouched(
            touchX,
            touchY
          );

        insideSpeedUp =
          isSpeedUpTouched(
            touchX,
            touchY
          );

        insideIntensityDown =
          isIntensityDownTouched(
            touchX,
            touchY
          );

        insideIntensityUp =
          isIntensityUpTouched(
            touchX,
            touchY
          );

        insidePalettePrev =
          isPalettePrevTouched(
            touchX,
            touchY
          );

        insidePaletteNext =
          isPaletteNextTouched(
            touchX,
            touchY
          );
      }

      // =====================================================
      // PRESET page hit tests
      // =====================================================

      bool insidePresetPrev = false;
      bool insidePresetNext = false;

      if (
        currentPage ==
        SCREEN_PRESET
      )
      {
        insideBack =
          isBackButtonTouched(
            touchX,
            touchY
          );

        insidePresetPrev =
          isPresetPrevTouched(
            touchX,
            touchY
          );

        insidePresetNext =
          isPresetNextTouched(
            touchX,
            touchY
          );
      }

      // =====================================================
      // New gesture
      // =====================================================

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

        speedLongPressActive =
          false;

        intensityLongPressActive =
          false;

        paletteLongPressActive =
          false;

        presetLongPressActive =
          false;
      }

      // =====================================================
      // Select touch target
      // =====================================================

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

        // ===================================================
        // MAIN
        // ===================================================

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

          else if (insideEffectDetail)
          {
            touchTarget =
              TOUCH_TARGET_EFFECT_DETAIL;

            lastTouchInsideEffectDetail =
              true;
          }

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

          else if (insidePresetOpen)
          {
            touchTarget =
              TOUCH_TARGET_PRESET_OPEN;

            lastTouchInsidePresetOpen =
              true;
          }
        }

        // ===================================================
        // COLOR
        // ===================================================

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

        // ===================================================
        // EFFECT
        // ===================================================

        else if (
          currentPage ==
          SCREEN_EFFECT
        )
        {
          if (insideBack)
          {
            touchTarget =
              TOUCH_TARGET_BACK;

            lastTouchInsideBack =
              true;
          }

          else if (insideSpeedDown)
          {
            touchTarget =
              TOUCH_TARGET_SPEED_DOWN;

            lastTouchInsideSpeed =
              true;

            speedPressStartTime =
              now;

            lastSpeedRepeat =
              now;
          }

          else if (insideSpeedUp)
          {
            touchTarget =
              TOUCH_TARGET_SPEED_UP;

            lastTouchInsideSpeed =
              true;

            speedPressStartTime =
              now;

            lastSpeedRepeat =
              now;
          }

          else if (insideIntensityDown)
          {
            touchTarget =
              TOUCH_TARGET_INTENSITY_DOWN;

            lastTouchInsideIntensity =
              true;

            intensityPressStartTime =
              now;

            lastIntensityRepeat =
              now;
          }

          else if (insideIntensityUp)
          {
            touchTarget =
              TOUCH_TARGET_INTENSITY_UP;

            lastTouchInsideIntensity =
              true;

            intensityPressStartTime =
              now;

            lastIntensityRepeat =
              now;
          }

          else if (insidePalettePrev)
          {
            touchTarget =
              TOUCH_TARGET_PALETTE_PREV;

            lastTouchInsidePalette =
              true;

            palettePressStartTime =
              now;

            lastPaletteRepeat =
              now;

            paletteLongPressActive =
              false;
          }

          else if (insidePaletteNext)
          {
            touchTarget =
              TOUCH_TARGET_PALETTE_NEXT;

            lastTouchInsidePalette =
              true;

            palettePressStartTime =
              now;

            lastPaletteRepeat =
              now;

            paletteLongPressActive =
              false;
          }
        }

        // ===================================================
        // PRESET
        // ===================================================

        else if (
          currentPage ==
          SCREEN_PRESET
        )
        {
          if (insideBack)
          {
            touchTarget =
              TOUCH_TARGET_BACK;

            lastTouchInsideBack =
              true;
          }

          else if (insidePresetPrev)
          {
            touchTarget =
              TOUCH_TARGET_PRESET_PREV;

            lastTouchInsidePresetNav =
              true;

            presetPressStartTime =
              now;

            lastPresetRepeat =
              now;

            presetLongPressActive =
              false;
          }

          else if (insidePresetNext)
          {
            touchTarget =
              TOUCH_TARGET_PRESET_NEXT;

            lastTouchInsidePresetNav =
              true;

            presetPressStartTime =
              now;

            lastPresetRepeat =
              now;

            presetLongPressActive =
              false;
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
      // Effect Prev / Next
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

        if (!insideSelectedButton)
        {
          return;
        }

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
      // Effect Detail
      // =====================================================

      if (
        touchTarget ==
        TOUCH_TARGET_EFFECT_DETAIL
      )
      {
        bool insideSelectedButton =
          isEffectDetailTouched(
            touchX,
            touchY
          );

        lastTouchInsideEffectDetail =
          insideSelectedButton;

        if (
          insideSelectedButton !=
          effectDetailVisualPressed
        )
        {
          drawEffectDetailButton(
            getCurrentEffectMode(),
            insideSelectedButton
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
      // PRESET open
      // =====================================================

      if (
        touchTarget ==
        TOUCH_TARGET_PRESET_OPEN
      )
      {
        lastTouchInsidePresetOpen =
          insidePresetOpen;

        if (
          insidePresetOpen !=
          presetOpenButtonVisualPressed
        )
        {
          drawPresetOpenButton(
            insidePresetOpen
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

      // =====================================================
      // Speed
      // =====================================================

      if (
        touchTarget ==
          TOUCH_TARGET_SPEED_DOWN ||
        touchTarget ==
          TOUCH_TARGET_SPEED_UP
      )
      {
        bool insideSelectedButton =
          isInsideSelectedSpeedButton(
            touchX,
            touchY
          );

        lastTouchInsideSpeed =
          insideSelectedButton;

        if (
          insideSelectedButton !=
          speedButtonVisualPressed
        )
        {
          drawSpeed(
            getCurrentSpeed(),
            insideSelectedButton
              ? touchTarget
              : TOUCH_TARGET_NONE
          );

          speedButtonVisualPressed =
            insideSelectedButton;
        }

        if (!insideSelectedButton)
        {
          return;
        }

        if (
          !speedLongPressActive &&
          now -
          speedPressStartTime >=
          SPEED_LONG_PRESS_MS
        )
        {
          speedLongPressActive =
            true;

          lastSpeedRepeat =
            now;

          speedLongPressStep(
            touchTarget
          );

          return;
        }

        if (
          speedLongPressActive &&
          now -
          lastSpeedRepeat >=
          SPEED_REPEAT_MS
        )
        {
          lastSpeedRepeat =
            now;

          speedLongPressStep(
            touchTarget
          );
        }

        return;
      }

      // =====================================================
      // Intensity
      // =====================================================

      if (
        touchTarget ==
          TOUCH_TARGET_INTENSITY_DOWN ||
        touchTarget ==
          TOUCH_TARGET_INTENSITY_UP
      )
      {
        bool insideSelectedButton =
          isInsideSelectedIntensityButton(
            touchX,
            touchY
          );

        lastTouchInsideIntensity =
          insideSelectedButton;

        if (
          insideSelectedButton !=
          intensityButtonVisualPressed
        )
        {
          drawIntensity(
            getCurrentIntensity(),
            insideSelectedButton
              ? touchTarget
              : TOUCH_TARGET_NONE
          );

          intensityButtonVisualPressed =
            insideSelectedButton;
        }

        if (!insideSelectedButton)
        {
          return;
        }

        if (
          !intensityLongPressActive &&
          now -
          intensityPressStartTime >=
          INTENSITY_LONG_PRESS_MS
        )
        {
          intensityLongPressActive =
            true;

          lastIntensityRepeat =
            now;

          intensityLongPressStep(
            touchTarget
          );

          return;
        }

        if (
          intensityLongPressActive &&
          now -
          lastIntensityRepeat >=
          INTENSITY_REPEAT_MS
        )
        {
          lastIntensityRepeat =
            now;

          intensityLongPressStep(
            touchTarget
          );
        }

        return;
      }

      // =====================================================
      // Palette
      // =====================================================

      if (
        touchTarget ==
          TOUCH_TARGET_PALETTE_PREV ||
        touchTarget ==
          TOUCH_TARGET_PALETTE_NEXT
      )
      {
        bool insideSelectedButton =
          isInsideSelectedPaletteButton(
            touchX,
            touchY
          );

        lastTouchInsidePalette =
          insideSelectedButton;

        if (
          insideSelectedButton !=
          paletteButtonVisualPressed
        )
        {
          drawPalette(
            getCurrentPalette(),
            insideSelectedButton
              ? touchTarget
              : TOUCH_TARGET_NONE
          );

          paletteButtonVisualPressed =
            insideSelectedButton;
        }

        if (!insideSelectedButton)
        {
          return;
        }

        if (
          !paletteLongPressActive &&
          now -
          palettePressStartTime >=
          PALETTE_LONG_PRESS_MS
        )
        {
          paletteLongPressActive =
            true;

          lastPaletteRepeat =
            now;

          paletteLongPressStep(
            touchTarget
          );

          return;
        }

        if (
          paletteLongPressActive &&
          now -
          lastPaletteRepeat >=
          PALETTE_REPEAT_MS
        )
        {
          lastPaletteRepeat =
            now;

          paletteLongPressStep(
            touchTarget
          );
        }

        return;
      }

      // =====================================================
      // Preset Prev / Next
      // =====================================================

      if (
        touchTarget ==
          TOUCH_TARGET_PRESET_PREV ||
        touchTarget ==
          TOUCH_TARGET_PRESET_NEXT
      )
      {
        bool insideSelectedButton =
          isInsideSelectedPresetButton(
            touchX,
            touchY
          );

        lastTouchInsidePresetNav =
          insideSelectedButton;

        if (
          insideSelectedButton !=
          presetNavButtonVisualPressed
        )
        {
          drawPresetNavigation(
            insideSelectedButton
              ? touchTarget
              : TOUCH_TARGET_NONE
          );

          presetNavButtonVisualPressed =
            insideSelectedButton;
        }

        if (!insideSelectedButton)
        {
          return;
        }

        if (
          !presetLongPressActive &&
          now -
          presetPressStartTime >=
          PRESET_LONG_PRESS_MS
        )
        {
          presetLongPressActive =
            true;

          lastPresetRepeat =
            now;

          presetLongPressStep(
            touchTarget
          );

          return;
        }

        if (
          presetLongPressActive &&
          now -
          lastPresetRepeat >=
          PRESET_REPEAT_MS
        )
        {
          lastPresetRepeat =
            now;

          presetLongPressStep(
            touchTarget
          );
        }

        return;
      }

      return;
    }

    // =======================================================
    // No Touch
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

    bool wasEffectLongPress =
      effectLongPressActive;

    bool wasHueLongPress =
      hueLongPressActive;

    bool wasSaturationLongPress =
      saturationLongPressActive;

    bool wasSpeedLongPress =
      speedLongPressActive;

    bool wasIntensityLongPress =
      intensityLongPressActive;

    bool wasPaletteLongPress =
      paletteLongPressActive;

    bool wasPresetLongPress =
      presetLongPressActive;

    // =======================================================
    // Determine actions
    // =======================================================

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
      lastTouchInsideEffect &&
      !wasEffectLongPress;

    bool executeEffectDetail =
      (
        releasedTarget ==
        TOUCH_TARGET_EFFECT_DETAIL
      ) &&
      lastTouchInsideEffectDetail;

    bool executeColorOpen =
      (
        releasedTarget ==
        TOUCH_TARGET_COLOR_OPEN
      ) &&
      lastTouchInsideColor;

    bool executePresetOpen =
      (
        releasedTarget ==
        TOUCH_TARGET_PRESET_OPEN
      ) &&
      lastTouchInsidePresetOpen;

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

    bool executeSpeedShortPress =
      (
        releasedTarget ==
          TOUCH_TARGET_SPEED_DOWN ||
        releasedTarget ==
          TOUCH_TARGET_SPEED_UP
      ) &&
      lastTouchInsideSpeed &&
      !wasSpeedLongPress;

    bool executeIntensityShortPress =
      (
        releasedTarget ==
          TOUCH_TARGET_INTENSITY_DOWN ||
        releasedTarget ==
          TOUCH_TARGET_INTENSITY_UP
      ) &&
      lastTouchInsideIntensity &&
      !wasIntensityLongPress;

    bool executePaletteShortPress =
      (
        releasedTarget ==
          TOUCH_TARGET_PALETTE_PREV ||
        releasedTarget ==
          TOUCH_TARGET_PALETTE_NEXT
      ) &&
      lastTouchInsidePalette &&
      !wasPaletteLongPress;

    bool executePresetShortPress =
      (
        releasedTarget ==
          TOUCH_TARGET_PRESET_PREV ||
        releasedTarget ==
          TOUCH_TARGET_PRESET_NEXT
      ) &&
      lastTouchInsidePresetNav &&
      !wasPresetLongPress;

    // =======================================================
    // Restore visuals
    // =======================================================

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
        TOUCH_TARGET_EFFECT_DETAIL &&
      effectDetailVisualPressed
    )
    {
      drawEffectDetailButton(
        getCurrentEffectMode(),
        false
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
        TOUCH_TARGET_PRESET_OPEN &&
      presetOpenButtonVisualPressed
    )
    {
      drawPresetOpenButton(
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

    if (
      (
        releasedTarget ==
          TOUCH_TARGET_SPEED_DOWN ||
        releasedTarget ==
          TOUCH_TARGET_SPEED_UP
      ) &&
      speedButtonVisualPressed
    )
    {
      drawSpeed(
        getCurrentSpeed(),
        TOUCH_TARGET_NONE
      );
    }

    if (
      (
        releasedTarget ==
          TOUCH_TARGET_INTENSITY_DOWN ||
        releasedTarget ==
          TOUCH_TARGET_INTENSITY_UP
      ) &&
      intensityButtonVisualPressed
    )
    {
      drawIntensity(
        getCurrentIntensity(),
        TOUCH_TARGET_NONE
      );
    }

    if (
      (
        releasedTarget ==
          TOUCH_TARGET_PALETTE_PREV ||
        releasedTarget ==
          TOUCH_TARGET_PALETTE_NEXT
      ) &&
      paletteButtonVisualPressed
    )
    {
      drawPalette(
        getCurrentPalette(),
        TOUCH_TARGET_NONE
      );
    }

    if (
      (
        releasedTarget ==
          TOUCH_TARGET_PRESET_PREV ||
        releasedTarget ==
          TOUCH_TARGET_PRESET_NEXT
      ) &&
      presetNavButtonVisualPressed
    )
    {
      drawPresetNavigation(
        TOUCH_TARGET_NONE
      );
    }

    // =======================================================
    // Preserve HSV gesture state through reset
    // =======================================================

    bool savedHueEditValid =
      hueEditValid;

    CHSV32 savedHueEditHsv =
      hueEditHsv;

    uint8_t savedHueEditValue =
      hueEditValue;

    uint8_t savedHueEditWhite =
      hueEditWhite;

    bool savedSaturationEditValid =
      saturationEditValid;

    CHSV32 savedSaturationEditHsv =
      saturationEditHsv;

    uint8_t savedSaturationEditValue =
      saturationEditValue;

    uint8_t savedSaturationEditWhite =
      saturationEditWhite;

    resetTouchGesture();

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
    // Power
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
    // Brightness short
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
    // Effect short
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
    // Open Effect Detail
    // =======================================================

    if (executeEffectDetail)
    {
      hueEditValid =
        false;

      saturationEditValid =
        false;

      drawEffectDetailScreen();

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
    // Open PRESET
    // =======================================================

    if (executePresetOpen)
    {
      hueEditValid =
        false;

      saturationEditValid =
        false;

      presetNoEntries =
        false;

      drawPresetScreen();

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
    // Hue short
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
    // Saturation short
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

    // =======================================================
    // Speed short
    // =======================================================

    if (executeSpeedShortPress)
    {
      speedShortPress(
        releasedTarget
      );

      drawSpeed(
        getCurrentSpeed(),
        TOUCH_TARGET_NONE
      );

      lastSpeedValue =
        getCurrentSpeed();

      hueEditValid =
        false;

      saturationEditValid =
        false;

      return;
    }

    // =======================================================
    // Intensity short
    // =======================================================

    if (executeIntensityShortPress)
    {
      intensityShortPress(
        releasedTarget
      );

      drawIntensity(
        getCurrentIntensity(),
        TOUCH_TARGET_NONE
      );

      lastIntensityValue =
        getCurrentIntensity();

      hueEditValid =
        false;

      saturationEditValid =
        false;

      return;
    }

    // =======================================================
    // Palette short
    // =======================================================

    if (executePaletteShortPress)
    {
      paletteShortPress(
        releasedTarget
      );

      drawPalette(
        getCurrentPalette(),
        TOUCH_TARGET_NONE
      );

      lastPaletteValue =
        getCurrentPalette();

      hueEditValid =
        false;

      saturationEditValid =
        false;

      return;
    }

    // =======================================================
    // Preset short
    // =======================================================

    if (executePresetShortPress)
    {
      presetShortPress(
        releasedTarget
      );

      drawPresetNavigation(
        TOUCH_TARGET_NONE
      );

      hueEditValid =
        false;

      saturationEditValid =
        false;

      return;
    }

    hueEditValid =
      false;

    saturationEditValid =
      false;
  }

public:

  // =========================================================
  // Usermod configuration
  // =========================================================

  void addToConfig(
    JsonObject& root
  ) override
  {
    JsonObject top =
      root.createNestedObject(
        FPSTR(
          CORES3_DISPLAY_CONFIG_NAME
        )
      );

    top[
      F("sleep-timeout")
    ] =
      sleepTimeoutSec;

    top[
      F("lcd-brightness")
    ] =
      lcdBrightness;

    top[
      F("fade")
    ] =
      fadeEnabled;

    top[
      F("fade-duration")
    ] =
      fadeDurationMs;
  }

  // =========================================================
  // Read configuration
  // =========================================================

  bool readFromConfig(
    JsonObject& root
  ) override
  {
    JsonObject top =
      root[
        FPSTR(
          CORES3_DISPLAY_CONFIG_NAME
        )
      ];

    if (
      top.isNull()
    )
    {
      Serial.println(
        F(
          "[CoreS3_Display] "
          "No display config found. Using defaults."
        )
      );

      return false;
    }

    bool configComplete =
      true;

    int newSleepTimeout =
      sleepTimeoutSec;

    int newLcdBrightness =
      lcdBrightness;

    bool newFadeEnabled =
      fadeEnabled;

    int newFadeDuration =
      fadeDurationMs;

    configComplete &=
      getJsonValue(
        top[
          F("sleep-timeout")
        ],
        newSleepTimeout,
        30
      );

    configComplete &=
      getJsonValue(
        top[
          F("lcd-brightness")
        ],
        newLcdBrightness,
        128
      );

    configComplete &=
      getJsonValue(
        top[
          F("fade")
        ],
        newFadeEnabled,
        true
      );

    configComplete &=
      getJsonValue(
        top[
          F("fade-duration")
        ],
        newFadeDuration,
        250
      );

    newSleepTimeout =
      constrain(
        newSleepTimeout,
        0,
        3600
      );

    newLcdBrightness =
      constrain(
        newLcdBrightness,
        1,
        255
      );

    newFadeDuration =
      constrain(
        newFadeDuration,
        50,
        2000
      );

    sleepTimeoutSec =
      (uint16_t)newSleepTimeout;

    lcdBrightness =
      (uint16_t)newLcdBrightness;

    fadeEnabled =
      newFadeEnabled;

    fadeDurationMs =
      (uint16_t)newFadeDuration;

    Serial.printf(
      "[CoreS3_Display] "
      "Config: Sleep=%u sec, "
      "LCD=%u, "
      "Fade=%s, "
      "FadeDuration=%u ms\n",
      sleepTimeoutSec,
      lcdBrightness,
      fadeEnabled
        ? "ON"
        : "OFF",
      fadeDurationMs
    );

    if (
      initDone &&
      displayReady &&
      displayPowerState ==
        DISPLAY_POWER_ACTIVE
    )
    {
      setDisplayBrightness(
        getNormalDisplayBrightness()
      );
    }

    return
      configComplete;
  }

  // =========================================================
  // Usermod Settings UI enhancement
  // =========================================================

  void appendConfigData(
    Print& settingsScript
  ) override
  {
    settingsScript.print(
      F(
        "cs3st=addDropdown("
        "'CoreS3_Display',"
        "'sleep-timeout'"
        ");"
      )
    );

    settingsScript.print(
      F(
        "addOption("
        "cs3st,"
        "'Never',"
        "0"
        ");"
      )
    );

    settingsScript.print(
      F(
        "addOption("
        "cs3st,"
        "'15 sec',"
        "15"
        ");"
      )
    );

    settingsScript.print(
      F(
        "addOption("
        "cs3st,"
        "'30 sec',"
        "30"
        ");"
      )
    );

    settingsScript.print(
      F(
        "addOption("
        "cs3st,"
        "'60 sec',"
        "60"
        ");"
      )
    );

    settingsScript.print(
      F(
        "addOption("
        "cs3st,"
        "'120 sec',"
        "120"
        ");"
      )
    );

    settingsScript.print(
      F(
        "addInfo("
        "'CoreS3_Display:lcd-brightness',"
        "1,"
        "'<small>1-255</small>'"
        ");"
      )
    );

    settingsScript.print(
      F(
        "cs3fd=addDropdown("
        "'CoreS3_Display',"
        "'fade-duration'"
        ");"
      )
    );

    settingsScript.print(
      F(
        "addOption("
        "cs3fd,"
        "'Fast - 150 ms',"
        "150"
        ");"
      )
    );

    settingsScript.print(
      F(
        "addOption("
        "cs3fd,"
        "'Normal - 250 ms',"
        "250"
        ");"
      )
    );

    settingsScript.print(
      F(
        "addOption("
        "cs3fd,"
        "'Slow - 400 ms',"
        "400"
        ");"
      )
    );

    settingsScript.print(
      F(
        "addOption("
        "cs3fd,"
        "'Very Slow - 600 ms',"
        "600"
        ");"
      )
    );
  }

  // =========================================================
  // Setup
  // =========================================================

  void setup() override
  {
    Serial.println();

    Serial.println(
      F(
        "[CoreS3_Display] "
        "Phase 10.1.1 start"
      )
    );

    Serial.printf(
      "[CoreS3_Display] "
      "Settings: "
      "Sleep=%u sec, "
      "LCD=%u, "
      "Fade=%s, "
      "FadeDuration=%u ms\n",
      sleepTimeoutSec,
      lcdBrightness,
      fadeEnabled
        ? "ON"
        : "OFF",
      fadeDurationMs
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

    lastPresetsModifiedTime =
      presetsModifiedTime;

    // -------------------------------------------------------
    // Phase 10.1.1
    //
    // Only initialize the cache builder here.
    //
    // Actual presets.json scanning starts later from loop()
    // after the normal startup sequence has completed.
    // -------------------------------------------------------

    presetCacheCount =
      0;

    presetCacheReady =
      false;

    presetCacheBuilding =
      false;

    presetCacheScanId =
      1;

    presetCacheSourceModifiedTime =
      presetsModifiedTime;

    displayReady =
      true;

    initDone =
      true;

    Serial.println(
      F(
        "[CoreS3_Display] "
        "Phase 10.1.1 setup complete"
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
    // Phase 10.1.1
    // Preset cache
    //
    // Start the first build after WLED startup is complete.
    //
    // Thereafter servicePresetCache() performs at most
    // one Preset ID check per interval.
    //
    // This is before sleep / update early returns so the
    // cache can continue building without blocking the UI.
    // =======================================================

    if (
      !presetCacheReady &&
      !presetCacheBuilding
    )
    {
      startPresetCacheRebuild();
    }

    servicePresetCache();

    // =======================================================
    // Normal touch
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

    // =======================================================
    // Pending Preset settle
    // =======================================================

    bool presetPendingSettled =
      settlePendingPreset();

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
    // Power sync
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
    // Current main segment values
    // =======================================================

    uint8_t effectMode =
      getCurrentEffectMode();

    uint8_t currentSpeed =
      getCurrentSpeed();

    uint8_t currentIntensity =
      getCurrentIntensity();

    uint8_t currentPalette =
      getCurrentPalette();

    uint8_t displayedPreset =
      getDisplayedPresetId();

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

      bool effectTouchActive =
        (
          touchTarget ==
            TOUCH_TARGET_EFFECT_PREV ||
          touchTarget ==
            TOUCH_TARGET_EFFECT_DETAIL ||
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

        lastSpeedValue =
          currentSpeed;

        lastIntensityValue =
          currentIntensity;

        lastPaletteValue =
          currentPalette;
      }

      if (
        (int)currentPalette !=
        lastPaletteValue
      )
      {
        lastPaletteValue =
          currentPalette;
      }

      if (
        (int)currentPreset !=
        lastPresetValue
      )
      {
        lastPresetValue =
          currentPreset;
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

    // =======================================================
    // EFFECT
    // =======================================================

    else if (
      currentPage ==
      SCREEN_EFFECT
    )
    {
      bool speedTouchActive =
        (
          touchTarget ==
            TOUCH_TARGET_SPEED_DOWN ||
          touchTarget ==
            TOUCH_TARGET_SPEED_UP
        );

      bool intensityTouchActive =
        (
          touchTarget ==
            TOUCH_TARGET_INTENSITY_DOWN ||
          touchTarget ==
            TOUCH_TARGET_INTENSITY_UP
        );

      bool paletteTouchActive =
        (
          touchTarget ==
            TOUCH_TARGET_PALETTE_PREV ||
          touchTarget ==
            TOUCH_TARGET_PALETTE_NEXT
        );

      if (
        touchTarget ==
          TOUCH_TARGET_NONE &&
        (int)effectMode !=
          lastEffectMode
      )
      {
        drawEffectDetailScreen();

        return;
      }

      if (
        !speedTouchActive &&
        (int)currentSpeed !=
          lastSpeedValue
      )
      {
        drawSpeed(
          currentSpeed,
          TOUCH_TARGET_NONE
        );

        lastSpeedValue =
          currentSpeed;
      }

      if (
        !intensityTouchActive &&
        (int)currentIntensity !=
          lastIntensityValue
      )
      {
        drawIntensity(
          currentIntensity,
          TOUCH_TARGET_NONE
        );

        lastIntensityValue =
          currentIntensity;
      }

      if (
        !paletteTouchActive &&
        (int)currentPalette !=
          lastPaletteValue
      )
      {
        drawPalette(
          currentPalette,
          TOUCH_TARGET_NONE
        );

        lastPaletteValue =
          currentPalette;
      }
    }

    // =======================================================
    // PRESET
    // =======================================================

    else if (
      currentPage ==
      SCREEN_PRESET
    )
    {
      bool presetTouchActive =
        (
          touchTarget ==
            TOUCH_TARGET_PRESET_PREV ||
          touchTarget ==
            TOUCH_TARGET_PRESET_NEXT
        );

      bool presetFileChanged =
        (
          presetsModifiedTime !=
          lastPresetsModifiedTime
        );

      if (presetFileChanged)
      {
        lastPresetsModifiedTime =
          presetsModifiedTime;

        presetNoEntries =
          false;
      }

      if (
        !presetTouchActive &&
        (
          (int)displayedPreset !=
            lastPresetValue ||
          presetPendingSettled ||
          presetFileChanged
        )
      )
      {
        drawPresetDetails(
          displayedPreset,
          pendingPresetId > 0
        );

        drawPresetNavigation(
          TOUCH_TARGET_NONE
        );

        lastPresetValue =
          displayedPreset;
      }
    }

    // =======================================================
    // Cache Primary Color
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
    // LED Power
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
    // WLED Brightness
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
    // Effect Speed
    // -------------------------------------------------------

    JsonArray speedInfo =
      user.createNestedArray(
        "CoreS3 Effect Speed"
      );

    if (
      strip.getSegmentsNum() >
      0
    )
    {
      speedInfo.add(
        getCurrentSpeed()
      );
    }
    else
    {
      speedInfo.add(
        "No segment"
      );
    }

    // -------------------------------------------------------
    // Effect Intensity
    // -------------------------------------------------------

    JsonArray intensityInfo =
      user.createNestedArray(
        "CoreS3 Effect Intensity"
      );

    if (
      strip.getSegmentsNum() >
      0
    )
    {
      intensityInfo.add(
        getCurrentIntensity()
      );
    }
    else
    {
      intensityInfo.add(
        "No segment"
      );
    }

    // -------------------------------------------------------
    // Palette
    // -------------------------------------------------------

    JsonArray paletteInfo =
      user.createNestedArray(
        "CoreS3 Effect Palette"
      );

    if (
      strip.getSegmentsNum() >
      0
    )
    {
      char paletteName[64];

      getPaletteName(
        getCurrentPalette(),
        paletteName,
        sizeof(paletteName)
      );

      paletteInfo.add(
        paletteName
      );
    }
    else
    {
      paletteInfo.add(
        "No segment"
      );
    }

    // -------------------------------------------------------
    // Palette ID
    // -------------------------------------------------------

    JsonArray paletteIdInfo =
      user.createNestedArray(
        "CoreS3 Effect Palette ID"
      );

    if (
      strip.getSegmentsNum() >
      0
    )
    {
      paletteIdInfo.add(
        getCurrentPalette()
      );
    }
    else
    {
      paletteIdInfo.add(
        "No segment"
      );
    }

    // -------------------------------------------------------
    // Palette touch area
    // -------------------------------------------------------

    JsonArray paletteTouchInfo =
      user.createNestedArray(
        "CoreS3 Palette Touch Area"
      );

    paletteTouchInfo.add(
      "Expanded"
    );

    // -------------------------------------------------------
    // Preset
    //
    // Do not call getPresetName() from addToJsonInfo()
    // because WLED may already own the shared JSON buffer.
    // -------------------------------------------------------

    JsonArray presetInfo =
      user.createNestedArray(
        "CoreS3 Display Preset"
      );

    presetInfo.add(
      currentPreset > 0
        ? "Active Preset"
        : "Custom State"
    );

    JsonArray presetIdInfo =
      user.createNestedArray(
        "CoreS3 Display Preset ID"
      );

    presetIdInfo.add(
      currentPreset
    );

    JsonArray presetTouchInfo =
      user.createNestedArray(
        "CoreS3 Preset Touch Area"
      );

    presetTouchInfo.add(
      "Expanded"
    );

    // -------------------------------------------------------
    // Phase 10.1.1
    // Preset cache
    // -------------------------------------------------------

    JsonArray presetCacheInfo =
      user.createNestedArray(
        "CoreS3 Preset Cache"
      );

    if (
      presetCacheReady
    )
    {
      char cacheText[32];

      snprintf(
        cacheText,
        sizeof(cacheText),
        "READY (%u)",
        (unsigned)presetCacheCount
      );

      presetCacheInfo.add(
        cacheText
      );
    }
    else if (
      presetCacheBuilding
    )
    {
      char cacheText[32];

      uint16_t displayScanId =
        presetCacheScanId;

      if (
        displayScanId >
        250
      )
      {
        displayScanId =
          250;
      }

      snprintf(
        cacheText,
        sizeof(cacheText),
        "LOADING (%u/250)",
        (unsigned)displayScanId
      );

      presetCacheInfo.add(
        cacheText
      );
    }
    else
    {
      presetCacheInfo.add(
        "NOT READY"
      );
    }

    // -------------------------------------------------------
    // Color
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

    if (
      currentPage ==
      SCREEN_MAIN
    )
    {
      pageInfo.add(
        "MAIN"
      );
    }
    else if (
      currentPage ==
      SCREEN_COLOR
    )
    {
      pageInfo.add(
        "COLOR"
      );
    }
    else if (
      currentPage ==
      SCREEN_EFFECT
    )
    {
      pageInfo.add(
        "EFFECT"
      );
    }
    else
    {
      pageInfo.add(
        "PRESET"
      );
    }

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
    // LCD brightness
    // -------------------------------------------------------

    JsonArray lcdBrightnessInfo =
      user.createNestedArray(
        "CoreS3 LCD Brightness"
      );

    lcdBrightnessInfo.add(
      lcdBrightness
    );

    // -------------------------------------------------------
    // Sleep
    // -------------------------------------------------------

    JsonArray sleepInfo =
      user.createNestedArray(
        "CoreS3 Display Sleep"
      );

    if (
      sleepTimeoutSec ==
      0
    )
    {
      sleepInfo.add(
        "Never"
      );
    }
    else
    {
      char sleepText[24];

      snprintf(
        sleepText,
        sizeof(sleepText),
        "%u sec",
        sleepTimeoutSec
      );

      sleepInfo.add(
        sleepText
      );
    }

    // -------------------------------------------------------
    // Fade
    // -------------------------------------------------------

    JsonArray fadeInfo =
      user.createNestedArray(
        "CoreS3 Display Fade"
      );

    if (!fadeEnabled)
    {
      fadeInfo.add(
        "OFF"
      );
    }
    else
    {
      char fadeText[24];

      snprintf(
        fadeText,
        sizeof(fadeText),
        "ON (%u ms)",
        fadeDurationMs
      );

      fadeInfo.add(
        fadeText
      );
    }

    // -------------------------------------------------------
    // Phase
    // -------------------------------------------------------

    JsonArray phaseInfo =
      user.createNestedArray(
        "CoreS3 Display Phase"
      );

    phaseInfo.add(
      "10.1.1"
    );
  }
};

static CoreS3DisplayUsermod coreS3DisplayUsermod;

REGISTER_USERMOD(coreS3DisplayUsermod);
