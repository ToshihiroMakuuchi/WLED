#include "wled.h"
#include <WiFi.h>
#include <M5GFX.h>
#include <Wire.h>

#include "CoreS3_WLED_Logo.h"

// ===========================================================
// M5Stack Display Controller Usermod
//
// Current verified runtime
//   - M5Stack CoreS3
//   - 320 x 240 display
//   - Touch input
//   - LCD brightness control
//
// Prepared hardware profiles
//   - M5Stack Core2
//   - M5Stack Core2 for AWS
//
// The Core2-family profiles remain diagnostic-only until their
// Display / Touch / Power paths are implemented and verified on
// real hardware.
//
// Responsibilities
//   - WLED power / brightness control
//   - Effect and palette navigation
//   - Color hue / saturation control
//   - Preset navigation and management
//   - Boot-preset selection
//   - Startup animation
//   - Display sleep / wake
//   - Runtime synchronization with WLED state
//
// Architecture
//   UI and WLED-state logic are kept separate from the thin
//   Display / Touch / brightness hardware-access boundary so the
//   same controller behavior can later be reused by Core2-family
//   hardware profiles.
//
// Compatibility
//   The existing "CoreS3_Display" configuration key and usermod
//   class identity are intentionally retained so existing CoreS3
//   settings continue to load without migration.
// ===========================================================

static const char CORES3_DISPLAY_CONFIG_NAME[] PROGMEM = "CoreS3_Display";

// ===========================================================
// Compile-time M5Stack hardware profile
//
// The default remains CoreS3 so existing builds do not need any
// new PlatformIO flags. Core2 / Core2 for AWS values are reserved
// for later hardware ports. Hardware revision can be
// left UNKNOWN until a board marking or hardware probe identifies it.
// ===========================================================

#define WLED_M5STACK_DISPLAY_PROFILE_CORES3        0
#define WLED_M5STACK_DISPLAY_PROFILE_CORE2         1
#define WLED_M5STACK_DISPLAY_PROFILE_CORE2_AWS     2

#define WLED_M5STACK_DISPLAY_REVISION_UNKNOWN      0
#define WLED_M5STACK_DISPLAY_REVISION_V1_0        10
#define WLED_M5STACK_DISPLAY_REVISION_V1_1        11
#define WLED_M5STACK_DISPLAY_REVISION_V1_3        13

#ifndef WLED_M5STACK_DISPLAY_PROFILE
  #define WLED_M5STACK_DISPLAY_PROFILE WLED_M5STACK_DISPLAY_PROFILE_CORES3
#endif

#ifndef WLED_M5STACK_DISPLAY_REVISION
  #define WLED_M5STACK_DISPLAY_REVISION WLED_M5STACK_DISPLAY_REVISION_UNKNOWN
#endif

// Core2 / Core2 for AWS profiles remain diagnostic-only.
// An explicit opt-in flag is required so unfinished Display/Power
// initialization can never be selected accidentally.
#ifndef WLED_M5STACK_CORE2_DIAGNOSTIC_ONLY
  #define WLED_M5STACK_CORE2_DIAGNOSTIC_ONLY 0
#endif

static_assert(
  WLED_M5STACK_DISPLAY_PROFILE >= WLED_M5STACK_DISPLAY_PROFILE_CORES3 &&
  WLED_M5STACK_DISPLAY_PROFILE <= WLED_M5STACK_DISPLAY_PROFILE_CORE2_AWS,
  "Invalid WLED_M5STACK_DISPLAY_PROFILE"
);

static_assert(
  WLED_M5STACK_DISPLAY_REVISION == WLED_M5STACK_DISPLAY_REVISION_UNKNOWN ||
  WLED_M5STACK_DISPLAY_REVISION == WLED_M5STACK_DISPLAY_REVISION_V1_0 ||
  WLED_M5STACK_DISPLAY_REVISION == WLED_M5STACK_DISPLAY_REVISION_V1_1 ||
  WLED_M5STACK_DISPLAY_REVISION == WLED_M5STACK_DISPLAY_REVISION_V1_3,
  "Invalid WLED_M5STACK_DISPLAY_REVISION"
);

static_assert(
  WLED_M5STACK_CORE2_DIAGNOSTIC_ONLY == 0 ||
  WLED_M5STACK_CORE2_DIAGNOSTIC_ONLY == 1,
  "WLED_M5STACK_CORE2_DIAGNOSTIC_ONLY must be 0 or 1"
);

static_assert(
  WLED_M5STACK_DISPLAY_PROFILE == WLED_M5STACK_DISPLAY_PROFILE_CORES3 ||
  WLED_M5STACK_CORE2_DIAGNOSTIC_ONLY == 1,
  "Core2-family profiles require WLED_M5STACK_CORE2_DIAGNOSTIC_ONLY=1"
);

static_assert(
  WLED_M5STACK_CORE2_DIAGNOSTIC_ONLY == 0 ||
  WLED_M5STACK_DISPLAY_PROFILE == WLED_M5STACK_DISPLAY_PROFILE_CORE2 ||
  WLED_M5STACK_DISPLAY_PROFILE == WLED_M5STACK_DISPLAY_PROFILE_CORE2_AWS,
  "Diagnostic-only mode is reserved for Core2-family profiles"
);

enum M5StackDisplayHardwareProfile : uint8_t {
  M5STACK_DISPLAY_HARDWARE_CORES3 = WLED_M5STACK_DISPLAY_PROFILE_CORES3,
  M5STACK_DISPLAY_HARDWARE_CORE2 = WLED_M5STACK_DISPLAY_PROFILE_CORE2,
  M5STACK_DISPLAY_HARDWARE_CORE2_AWS = WLED_M5STACK_DISPLAY_PROFILE_CORE2_AWS
};

enum M5StackDisplayHardwareRevision : uint8_t {
  M5STACK_DISPLAY_REVISION_UNKNOWN = WLED_M5STACK_DISPLAY_REVISION_UNKNOWN,
  M5STACK_DISPLAY_REVISION_V1_0 = WLED_M5STACK_DISPLAY_REVISION_V1_0,
  M5STACK_DISPLAY_REVISION_V1_1 = WLED_M5STACK_DISPLAY_REVISION_V1_1,
  M5STACK_DISPLAY_REVISION_V1_3 = WLED_M5STACK_DISPLAY_REVISION_V1_3
};

static constexpr M5StackDisplayHardwareProfile ACTIVE_M5STACK_DISPLAY_PROFILE =
  static_cast<M5StackDisplayHardwareProfile>( WLED_M5STACK_DISPLAY_PROFILE );

static constexpr M5StackDisplayHardwareRevision ACTIVE_M5STACK_DISPLAY_REVISION =
  static_cast<M5StackDisplayHardwareRevision>( WLED_M5STACK_DISPLAY_REVISION );


static constexpr bool ACTIVE_M5STACK_CORE2_DIAGNOSTIC_ONLY =
  ( WLED_M5STACK_CORE2_DIAGNOSTIC_ONLY == 1 );

// ===========================================================
// M5Stack hardware capability profile
//
// Board-dependent runtime capabilities are defined in one table.
// UI, WLED-state handling, Preset logic, Touch state machines and
// Display drawing remain board-neutral and consume this boundary.
//
// CoreS3 is the only verified active Display runtime.
// Core2 / Core2 for AWS remain diagnostic-only until their
// Display / Touch / Power implementations are hardware-verified.
// ===========================================================

struct M5StackDisplayHardwareCapabilities {
  const char* name;
  uint8_t displayRotation;
  bool displayRuntimeEnabled;
  bool touchRuntimeEnabled;
  bool brightnessRuntimeEnabled;
  bool diagnosticProbeEnabled;
};

static constexpr M5StackDisplayHardwareCapabilities M5STACK_HARDWARE_CAPABILITIES[] = {
  {
    "M5Stack CoreS3",
    1,
    true,
    true,
    true,
    false
  },
  {
    "M5Stack Core2",
    1,
    false,
    false,
    false,
    true
  },
  {
    "M5Stack Core2 for AWS",
    1,
    false,
    false,
    false,
    true
  }
};

static constexpr const M5StackDisplayHardwareCapabilities&
  ACTIVE_M5STACK_HARDWARE_CAPABILITIES =
    M5STACK_HARDWARE_CAPABILITIES[WLED_M5STACK_DISPLAY_PROFILE];

// ===========================================================
// Core2-family read-only hardware diagnostic classifications
// ===========================================================

enum M5StackHardwareProbeState : uint8_t {
  M5STACK_HARDWARE_PROBE_NOT_REQUIRED = 0,
  M5STACK_HARDWARE_PROBE_NOT_RUN,
  M5STACK_HARDWARE_PROBE_COMPLETE,
  M5STACK_HARDWARE_PROBE_FAILED
};

enum M5StackDetectedPmu : uint8_t {
  M5STACK_DETECTED_PMU_UNKNOWN = 0,
  M5STACK_DETECTED_PMU_AXP192,
  M5STACK_DETECTED_PMU_AXP2101
};

enum M5StackDetectedImu : uint8_t {
  M5STACK_DETECTED_IMU_UNKNOWN = 0,
  M5STACK_DETECTED_IMU_MPU6886,
  M5STACK_DETECTED_IMU_BMI270
};

enum M5StackDetectedVariant : uint8_t {
  M5STACK_DETECTED_VARIANT_UNKNOWN = 0,
  M5STACK_DETECTED_VARIANT_CORE2_LEGACY,
  M5STACK_DETECTED_VARIANT_CORE2_V1_1,
  M5STACK_DETECTED_VARIANT_CORE2_AWS_LEGACY,
  M5STACK_DETECTED_VARIANT_CORE2_AWS_V1_3
};

// ===========================================================
// M5Stack Display hardware backend
//
// This backend owns the board-dependent operations used by the
// controller UI:
//   - Core2-family read-only hardware diagnostics
//   - Display initialization / rotation
//   - Touch acquisition
//   - LCD brightness writes
//
// The usermod itself keeps UI state, WLED state synchronization,
// Preset handling, Touch state machines, Sleep/Wake and drawing.
//
// CoreS3 runtime behavior is intentionally unchanged.
// Core2 / Core2 for AWS remain diagnostic-only.
// ===========================================================

class M5StackDisplayHardwareBackend {
  private:

  struct HardwareProbeResult {
    M5StackHardwareProbeState state = M5STACK_HARDWARE_PROBE_NOT_RUN;
    M5StackDetectedPmu pmu = M5STACK_DETECTED_PMU_UNKNOWN;
    M5StackDetectedImu imu = M5STACK_DETECTED_IMU_UNKNOWN;
    M5StackDetectedVariant variant = M5STACK_DETECTED_VARIANT_UNKNOWN;

    bool address34 = false;
    bool address35 = false;
    bool address38 = false;
    bool address40 = false;
    bool address51 = false;
    bool address68 = false;

    uint8_t imuChipId = 0;
  };

  static constexpr int CORE2_INTERNAL_I2C_SDA = 21;
  static constexpr int CORE2_INTERNAL_I2C_SCL = 22;
  static constexpr uint32_t CORE2_INTERNAL_I2C_FREQUENCY = 400000;

  M5GFX& display;
  HardwareProbeResult hardwareProbe;

  bool probeI2CAddress( TwoWire& wire, uint8_t address ) {
    wire.beginTransmission( address );

    return wire.endTransmission() == 0;
  }

  bool readI2CRegister8( TwoWire& wire, uint8_t address, uint8_t reg, uint8_t& value ) {
    wire.beginTransmission( address );
    wire.write( reg );

    if ( wire.endTransmission( false ) != 0 ) {
      return false;
    }

    size_t received = wire.requestFrom( address, (size_t)1, true );

    if ( received != 1 || !wire.available() ) {
      return false;
    }

    value = (uint8_t)wire.read();

    return true;
  }

  void classifyCore2HardwareProbe( TwoWire& wire ) {
    if ( hardwareProbe.address68 ) {
      uint8_t chipId = 0;

      // BMI270 CHIP_ID register 0x00 returns 0x24.
      if ( readI2CRegister8( wire, 0x68, 0x00, chipId ) && chipId == 0x24 ) {
        hardwareProbe.imu = M5STACK_DETECTED_IMU_BMI270;
        hardwareProbe.imuChipId = chipId;
      }
      else {
        // MPU6886 WHO_AM_I register 0x75 returns 0x19.
        if ( readI2CRegister8( wire, 0x68, 0x75, chipId ) && chipId == 0x19 ) {
          hardwareProbe.imu = M5STACK_DETECTED_IMU_MPU6886;
          hardwareProbe.imuChipId = chipId;
        }
      }
    }

    if ( ACTIVE_M5STACK_DISPLAY_PROFILE == M5STACK_DISPLAY_HARDWARE_CORE2_AWS ) {
      if ( hardwareProbe.address34 ) {
        // Both documented Core2 for AWS generations use AXP192.
        hardwareProbe.pmu = M5STACK_DETECTED_PMU_AXP192;
      }

      if ( hardwareProbe.imu == M5STACK_DETECTED_IMU_BMI270 ) {
        hardwareProbe.variant = M5STACK_DETECTED_VARIANT_CORE2_AWS_V1_3;
      }
      else if ( hardwareProbe.imu == M5STACK_DETECTED_IMU_MPU6886 ) {
        // Official legacy Core2 for AWS documentation identifies MPU6886,
        // but does not assign the v1.0 / v1.1 name here. Keep it generic.
        hardwareProbe.variant = M5STACK_DETECTED_VARIANT_CORE2_AWS_LEGACY;
      }
    }
    else if ( ACTIVE_M5STACK_DISPLAY_PROFILE == M5STACK_DISPLAY_HARDWARE_CORE2 ) {
      if ( hardwareProbe.address34 && hardwareProbe.address40 ) {
        // Core2 v1.1 signature: AXP2101 + INA3221.
        hardwareProbe.pmu = M5STACK_DETECTED_PMU_AXP2101;
        hardwareProbe.variant = M5STACK_DETECTED_VARIANT_CORE2_V1_1;
      }
      else if ( hardwareProbe.address34 ) {
        hardwareProbe.pmu = M5STACK_DETECTED_PMU_AXP192;
        hardwareProbe.variant = M5STACK_DETECTED_VARIANT_CORE2_LEGACY;
      }
    }
  }

  void printCore2HardwareProbeResult() {
    Serial.printf(
      "[CoreS3_Display] Hardware probe: %s, Variant=%s, PMU=%s, IMU=%s",
      probeStateName(),
      detectedVariantName(),
      detectedPmuName(),
      detectedImuName()
    );

    if ( hardwareProbe.imuChipId > 0 ) {
      Serial.printf( " (ID=0x%02X)", hardwareProbe.imuChipId );
    }

    Serial.println();

    Serial.printf(
      "[CoreS3_Display] Detected revision: %s\n",
      detectedRevisionName()
    );

    Serial.printf(
      "[CoreS3_Display] Core2 I2C signature: "
      "34=%s 35=%s 38=%s 40=%s 51=%s 68=%s\n",
      hardwareProbe.address34 ? "YES" : "NO",
      hardwareProbe.address35 ? "YES" : "NO",
      hardwareProbe.address38 ? "YES" : "NO",
      hardwareProbe.address40 ? "YES" : "NO",
      hardwareProbe.address51 ? "YES" : "NO",
      hardwareProbe.address68 ? "YES" : "NO"
    );
  }

  public:

  explicit M5StackDisplayHardwareBackend( M5GFX& displayRef )
    : display( displayRef ) {
  }

  const char* profileName() const {
    return ACTIVE_M5STACK_HARDWARE_CAPABILITIES.name;
  }

  const char* revisionName() const {
    switch ( ACTIVE_M5STACK_DISPLAY_REVISION ) {
      case M5STACK_DISPLAY_REVISION_V1_0:
        return "v1.0";

      case M5STACK_DISPLAY_REVISION_V1_1:
        return "v1.1";

      case M5STACK_DISPLAY_REVISION_V1_3:
        return "v1.3";

      case M5STACK_DISPLAY_REVISION_UNKNOWN:
      default:
        return "UNKNOWN";
    }
  }

  bool isDisplayRuntimeEnabled() const {
    return ACTIVE_M5STACK_HARDWARE_CAPABILITIES.displayRuntimeEnabled;
  }

  bool isCore2FamilyProfile() const {
    return ACTIVE_M5STACK_HARDWARE_CAPABILITIES.diagnosticProbeEnabled;
  }

  bool isCore2DiagnosticOnlyMode() const {
    return isCore2FamilyProfile() && ACTIVE_M5STACK_CORE2_DIAGNOSTIC_ONLY;
  }

  const char* runtimeModeName() const {
    return isCore2DiagnosticOnlyMode() ? "CORE2 DIAGNOSTIC ONLY" : "DISPLAY ACTIVE";
  }

  const char* portStatusName() const {
    if ( ACTIVE_M5STACK_HARDWARE_CAPABILITIES.displayRuntimeEnabled ) {
      return "CORES3 VERIFIED DISPLAY RUNTIME";
    }

    if ( isCore2DiagnosticOnlyMode() ) {
      return "CORE2 PORT PREPARED - DIAGNOSTIC ONLY";
    }

    return "HARDWARE RUNTIME BLOCKED";
  }

  const char* probeStateName() const {
    switch ( hardwareProbe.state ) {
      case M5STACK_HARDWARE_PROBE_NOT_REQUIRED:
        return "NOT REQUIRED";

      case M5STACK_HARDWARE_PROBE_COMPLETE:
        return "COMPLETE";

      case M5STACK_HARDWARE_PROBE_FAILED:
        return "FAILED";

      case M5STACK_HARDWARE_PROBE_NOT_RUN:
      default:
        return "NOT RUN";
    }
  }

  const char* detectedPmuName() const {
    switch ( hardwareProbe.pmu ) {
      case M5STACK_DETECTED_PMU_AXP192:
        return "AXP192 signature";

      case M5STACK_DETECTED_PMU_AXP2101:
        return "AXP2101 signature";

      case M5STACK_DETECTED_PMU_UNKNOWN:
      default:
        return "UNKNOWN";
    }
  }

  const char* detectedImuName() const {
    switch ( hardwareProbe.imu ) {
      case M5STACK_DETECTED_IMU_MPU6886:
        return "MPU6886";

      case M5STACK_DETECTED_IMU_BMI270:
        return "BMI270";

      case M5STACK_DETECTED_IMU_UNKNOWN:
      default:
        return "UNKNOWN";
    }
  }

  const char* detectedVariantName() const {
    switch ( hardwareProbe.variant ) {
      case M5STACK_DETECTED_VARIANT_CORE2_LEGACY:
        return "Core2 legacy signature";

      case M5STACK_DETECTED_VARIANT_CORE2_V1_1:
        return "Core2 v1.1 signature";

      case M5STACK_DETECTED_VARIANT_CORE2_AWS_LEGACY:
        return "Core2 for AWS MPU6886 generation";

      case M5STACK_DETECTED_VARIANT_CORE2_AWS_V1_3:
        return "Core2 for AWS v1.3 signature";

      case M5STACK_DETECTED_VARIANT_UNKNOWN:
      default:
        return "UNKNOWN";
    }
  }

  const char* detectedRevisionName() const {
    switch ( hardwareProbe.variant ) {
      case M5STACK_DETECTED_VARIANT_CORE2_V1_1:
        return "v1.1 signature";

      case M5STACK_DETECTED_VARIANT_CORE2_AWS_V1_3:
        return "v1.3 signature";

      case M5STACK_DETECTED_VARIANT_CORE2_AWS_LEGACY:
        return "Legacy MPU6886 generation; exact revision unknown";

      case M5STACK_DETECTED_VARIANT_CORE2_LEGACY:
        return "Legacy generation; exact revision unknown";

      case M5STACK_DETECTED_VARIANT_UNKNOWN:
      default:
        return "UNKNOWN";
    }
  }

  bool isProbeComplete() const {
    return hardwareProbe.state == M5STACK_HARDWARE_PROBE_COMPLETE;
  }

  bool hasI2CAddress( uint8_t address ) const {
    switch ( address ) {
      case 0x34:
        return hardwareProbe.address34;

      case 0x35:
        return hardwareProbe.address35;

      case 0x38:
        return hardwareProbe.address38;

      case 0x40:
        return hardwareProbe.address40;

      case 0x51:
        return hardwareProbe.address51;

      case 0x68:
        return hardwareProbe.address68;

      default:
        return false;
    }
  }

  void runDiagnostics() {
    hardwareProbe = HardwareProbeResult();

    if ( !ACTIVE_M5STACK_HARDWARE_CAPABILITIES.diagnosticProbeEnabled ) {
      hardwareProbe.state = M5STACK_HARDWARE_PROBE_NOT_REQUIRED;

      Serial.println( F( "[CoreS3_Display] CoreS3 hardware probe skipped (verified profile)" ) );

      return;
    }

    TwoWire probeWire( 1 );

    if ( !probeWire.begin( CORE2_INTERNAL_I2C_SDA, CORE2_INTERNAL_I2C_SCL, CORE2_INTERNAL_I2C_FREQUENCY ) ) {
      hardwareProbe.state = M5STACK_HARDWARE_PROBE_FAILED;

      Serial.println( F( "[CoreS3_Display] ERROR: Core2 diagnostic I2C start failed" ) );

      return;
    }

    delay( 10 );

    hardwareProbe.address34 = probeI2CAddress( probeWire, 0x34 );
    hardwareProbe.address35 = probeI2CAddress( probeWire, 0x35 );
    hardwareProbe.address38 = probeI2CAddress( probeWire, 0x38 );
    hardwareProbe.address40 = probeI2CAddress( probeWire, 0x40 );
    hardwareProbe.address51 = probeI2CAddress( probeWire, 0x51 );
    hardwareProbe.address68 = probeI2CAddress( probeWire, 0x68 );

    classifyCore2HardwareProbe( probeWire );

    probeWire.end();

    hardwareProbe.state = M5STACK_HARDWARE_PROBE_COMPLETE;

    printCore2HardwareProbeResult();
  }

  bool initializeDisplay( int16_t& screenWidth, int16_t& screenHeight, bool& touchReady ) {
    if ( !isDisplayRuntimeEnabled() ) {
      Serial.printf(
        "[CoreS3_Display] Hardware profile not enabled for Display runtime: %s (%s)\n",
        profileName(),
        revisionName()
      );

      return false;
    }

    display.begin();

    display.setRotation( ACTIVE_M5STACK_HARDWARE_CAPABILITIES.displayRotation );

    screenWidth = display.width();
    screenHeight = display.height();

    Serial.printf( "[CoreS3_Display] " "Display size: %d x %d\n", screenWidth, screenHeight );

    if ( screenWidth <= 0 || screenHeight <= 0 ) {
      Serial.println( F( "[CoreS3_Display] " "ERROR: Display not detected" ) );

      return false;
    }

    touchReady = ( display.touch() != nullptr );

    Serial.printf( "[CoreS3_Display] " "Touch: %s\n", touchReady ? "READY" : "NOT FOUND" );

    return true;
  }

  bool readTouch( int16_t& touchX, int16_t& touchY ) {
    if ( !ACTIVE_M5STACK_HARDWARE_CAPABILITIES.touchRuntimeEnabled ) {
      return false;
    }

    return ( display.getTouch( &touchX, &touchY ) > 0 );
  }

  void writeBrightness( uint8_t value ) {
    if ( !ACTIVE_M5STACK_HARDWARE_CAPABILITIES.brightnessRuntimeEnabled ) {
      return;
    }

    display.setBrightness( value );
  }
};

// ===========================================================
// Fixed touch rectangles
//
// These values preserve the hardware-verified touch hit areas.
// Only their representation is consolidated here.
// ===========================================================

struct CoreS3TouchRect {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

static constexpr CoreS3TouchRect CORES3_TOUCH_POWER              = {   8,   8,  44, 44 };
static constexpr CoreS3TouchRect CORES3_TOUCH_BRIGHTNESS_DOWN    = {  16,  82,  64, 34 };
static constexpr CoreS3TouchRect CORES3_TOUCH_BRIGHTNESS_UP      = { 240,  82,  64, 34 };
static constexpr CoreS3TouchRect CORES3_TOUCH_EFFECT_PREV        = {  16, 138,  64, 34 };
static constexpr CoreS3TouchRect CORES3_TOUCH_EFFECT_DETAIL      = {  88, 138, 144, 34 };
static constexpr CoreS3TouchRect CORES3_TOUCH_EFFECT_NEXT        = { 240, 138,  64, 34 };
static constexpr CoreS3TouchRect CORES3_TOUCH_COLOR_OPEN         = {   8, 180, 152, 60 };
static constexpr CoreS3TouchRect CORES3_TOUCH_PRESET_OPEN        = { 160, 180, 152, 60 };
static constexpr CoreS3TouchRect CORES3_TOUCH_BACK               = { 268,   8,  44, 44 };
static constexpr CoreS3TouchRect CORES3_TOUCH_HUE_DOWN           = {  16, 151,  64, 34 };
static constexpr CoreS3TouchRect CORES3_TOUCH_HUE_UP             = { 240, 151,  64, 34 };
static constexpr CoreS3TouchRect CORES3_TOUCH_SATURATION_DOWN    = {  16, 204,  64, 34 };
static constexpr CoreS3TouchRect CORES3_TOUCH_SATURATION_UP      = { 240, 204,  64, 34 };
static constexpr CoreS3TouchRect CORES3_TOUCH_SPEED_DOWN         = {  16,  82,  64, 34 };
static constexpr CoreS3TouchRect CORES3_TOUCH_SPEED_UP           = { 240,  82,  64, 34 };
static constexpr CoreS3TouchRect CORES3_TOUCH_INTENSITY_DOWN     = {  16, 140,  64, 34 };
static constexpr CoreS3TouchRect CORES3_TOUCH_INTENSITY_UP       = { 240, 140,  64, 34 };
static constexpr CoreS3TouchRect CORES3_TOUCH_PALETTE_PREV       = {   8, 188,  80, 52 };
static constexpr CoreS3TouchRect CORES3_TOUCH_PALETTE_NEXT       = { 232, 188,  80, 52 };
static constexpr CoreS3TouchRect CORES3_TOUCH_PRESET_PREV        = {   8, 188,  80, 52 };
static constexpr CoreS3TouchRect CORES3_TOUCH_PRESET_NEXT        = { 232, 188,  80, 52 };
static constexpr CoreS3TouchRect CORES3_TOUCH_PRESET_MANAGE      = {  88, 188, 144, 52 };
static constexpr CoreS3TouchRect CORES3_TOUCH_PRESET_SAVE_NEW    = {  48,  60, 224, 42 };
static constexpr CoreS3TouchRect CORES3_TOUCH_PRESET_SAVE_HOLD   = {  48, 170, 224, 66 };
static constexpr CoreS3TouchRect CORES3_TOUCH_OVERWRITE_OPEN     = {  48, 102, 224, 42 };
static constexpr CoreS3TouchRect CORES3_TOUCH_OVERWRITE_PREV     = {   8, 116,  80, 52 };
static constexpr CoreS3TouchRect CORES3_TOUCH_OVERWRITE_NEXT     = { 232, 116,  80, 52 };
static constexpr CoreS3TouchRect CORES3_TOUCH_OVERWRITE_HOLD     = {  48, 184, 224, 56 };
static constexpr CoreS3TouchRect CORES3_TOUCH_DELETE_OPEN        = {  48, 144, 224, 42 };
static constexpr CoreS3TouchRect CORES3_TOUCH_DELETE_PREV        = {   8, 116,  80, 52 };
static constexpr CoreS3TouchRect CORES3_TOUCH_DELETE_NEXT        = { 232, 116,  80, 52 };
static constexpr CoreS3TouchRect CORES3_TOUCH_DELETE_HOLD        = {  48, 184, 224, 56 };
static constexpr CoreS3TouchRect CORES3_TOUCH_BOOT_OPEN          = {  48, 186, 224, 48 };
static constexpr CoreS3TouchRect CORES3_TOUCH_BOOT_PREV          = {   8, 116,  80, 52 };
static constexpr CoreS3TouchRect CORES3_TOUCH_BOOT_NEXT          = { 232, 116,  80, 52 };
static constexpr CoreS3TouchRect CORES3_TOUCH_BOOT_HOLD          = {  48, 184, 224, 56 };

class CoreS3DisplayUsermod : public Usermod {
  private:

  // =========================================================
  // Display
  // =========================================================

  M5GFX display;
  M5StackDisplayHardwareBackend hardwareBackend;

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
  // Preset state
  // =========================================================

  int lastPresetValue = -1;
  int lastBootPresetValue = -1;

  uint8_t pendingPresetId = 0;
  String pendingPresetName = "";

  unsigned long pendingPresetRequestMs = 0;

  unsigned long lastPresetsModifiedTime = 0;

  bool presetNoEntries = false;

  static constexpr unsigned long PRESET_APPLY_PENDING_MS = 1500;

  // =========================================================
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

  struct PresetCacheEntry {
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

  enum DisplayPowerState : uint8_t {
    DISPLAY_POWER_ACTIVE = 0, DISPLAY_POWER_SLEEP_FADE_OUT, DISPLAY_POWER_SLEEPING, DISPLAY_POWER_WAKE_FADE_IN, DISPLAY_POWER_WAKE_WAIT_RELEASE };

  DisplayPowerState displayPowerState = DISPLAY_POWER_ACTIVE;

  unsigned long lastUserActivityMs = 0;
  unsigned long displayFadeLastStep = 0;

  unsigned long wakeTouchLastPoll = 0;
  bool wakeTouchState = false;

  unsigned long wakeReleaseCandidate = 0;

  // =========================================================
  // Startup animation
  // =========================================================

  enum StartupState : uint8_t {
    STARTUP_FADE_IN = 0, STARTUP_WAIT_WIFI, STARTUP_CONNECTED_HOLD, STARTUP_FADE_OUT, STARTUP_MAIN_FADE_IN, STARTUP_DONE };

  StartupState startupState = STARTUP_FADE_IN;

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

  enum ScreenPage : uint8_t {
    SCREEN_MAIN = 0, SCREEN_COLOR, SCREEN_EFFECT, SCREEN_PRESET };

  ScreenPage currentPage = SCREEN_MAIN;

  // =========================================================
  // Preset sub pages
  // =========================================================

  enum PresetSubPage : uint8_t {
    PRESET_SUBPAGE_NAV = 0, PRESET_SUBPAGE_MANAGE, PRESET_SUBPAGE_SAVE, PRESET_SUBPAGE_OVERWRITE, PRESET_SUBPAGE_DELETE, PRESET_SUBPAGE_BOOT };

  PresetSubPage presetSubPage = PRESET_SUBPAGE_NAV;

  // =========================================================
  // New Preset save operation
  // =========================================================

  enum PresetSaveOperationState : uint8_t {
    PRESET_SAVE_OP_IDLE = 0, PRESET_SAVE_OP_WAIT_WLED, PRESET_SAVE_OP_WAIT_CACHE, PRESET_SAVE_OP_SUCCESS, PRESET_SAVE_OP_FAILED };

  PresetSaveOperationState presetSaveOperationState = PRESET_SAVE_OP_IDLE;

  uint8_t presetSaveCandidateId = 0;
  String presetSaveCandidateName = "";

  unsigned long presetSaveHoldStartTime = 0;
  bool presetSaveHoldTriggered = false;

  unsigned long presetSaveResultStartMs = 0;

  static constexpr unsigned long PRESET_SAVE_HOLD_MS = 1000;
  static constexpr unsigned long PRESET_SAVE_RESULT_HOLD_MS = 900;

  // =========================================================
  // Existing Preset overwrite selection
  // =========================================================

  uint8_t presetOverwriteTargetId = 0;
  String presetOverwriteTargetName = "";

  bool presetSaveOperationIsOverwrite = false;

  // =========================================================
  // Existing Preset delete selection / operation
  // =========================================================

  enum PresetDeleteOperationState : uint8_t {
    PRESET_DELETE_OP_IDLE = 0, PRESET_DELETE_OP_WAIT_CACHE, PRESET_DELETE_OP_SUCCESS, PRESET_DELETE_OP_FAILED };

  PresetDeleteOperationState presetDeleteOperationState = PRESET_DELETE_OP_IDLE;

  uint8_t presetDeleteTargetId = 0;
  String presetDeleteTargetName = "";

  bool presetDeleteWasCurrentPreset = false;
  bool presetDeleteWasBootPreset = false;

  unsigned long presetDeleteResultStartMs = 0;

  // =========================================================
  // Boot Preset selection / operation
  // =========================================================

  enum PresetBootOperationState : uint8_t {
    PRESET_BOOT_OP_IDLE = 0, PRESET_BOOT_OP_WAIT_CONFIG, PRESET_BOOT_OP_SUCCESS, PRESET_BOOT_OP_FAILED };

  PresetBootOperationState presetBootOperationState = PRESET_BOOT_OP_IDLE;

  uint8_t presetBootTargetId = 0;
  String presetBootTargetName = "NONE";

  unsigned long presetBootResultStartMs = 0;

  // =========================================================
  // Touch targets
  // =========================================================

  enum TouchTarget : uint8_t {
    TOUCH_TARGET_NONE = 0,

    TOUCH_TARGET_POWER,

    TOUCH_TARGET_BRIGHTNESS_DOWN, TOUCH_TARGET_BRIGHTNESS_UP,

    TOUCH_TARGET_EFFECT_PREV, TOUCH_TARGET_EFFECT_DETAIL, TOUCH_TARGET_EFFECT_NEXT,

    TOUCH_TARGET_COLOR_OPEN, TOUCH_TARGET_PRESET_OPEN,

    TOUCH_TARGET_BACK,

    TOUCH_TARGET_HUE_DOWN, TOUCH_TARGET_HUE_UP,

    TOUCH_TARGET_SATURATION_DOWN, TOUCH_TARGET_SATURATION_UP,

    TOUCH_TARGET_SPEED_DOWN, TOUCH_TARGET_SPEED_UP,

    TOUCH_TARGET_INTENSITY_DOWN, TOUCH_TARGET_INTENSITY_UP,

    TOUCH_TARGET_PALETTE_PREV, TOUCH_TARGET_PALETTE_NEXT,

    TOUCH_TARGET_PRESET_PREV, TOUCH_TARGET_PRESET_NEXT,

    TOUCH_TARGET_PRESET_MANAGE, TOUCH_TARGET_PRESET_SAVE_NEW, TOUCH_TARGET_PRESET_SAVE_HOLD,

    TOUCH_TARGET_PRESET_OVERWRITE_OPEN, TOUCH_TARGET_PRESET_OVERWRITE_PREV, TOUCH_TARGET_PRESET_OVERWRITE_NEXT, TOUCH_TARGET_PRESET_OVERWRITE_HOLD,

    TOUCH_TARGET_PRESET_DELETE_OPEN, TOUCH_TARGET_PRESET_DELETE_PREV, TOUCH_TARGET_PRESET_DELETE_NEXT, TOUCH_TARGET_PRESET_DELETE_HOLD,

    TOUCH_TARGET_PRESET_BOOT_OPEN, TOUCH_TARGET_PRESET_BOOT_PREV, TOUCH_TARGET_PRESET_BOOT_NEXT, TOUCH_TARGET_PRESET_BOOT_HOLD };

  TouchTarget touchTarget = TOUCH_TARGET_NONE;

  // =========================================================
  // Touch hit state
  //
  // A single hit-test snapshot is built for each active touch
  // poll and then passed to Press / Hold processing.
  // Coordinates and enable conditions are unchanged.
  // =========================================================

  struct TouchHitState {
    bool insidePower = false;
    bool insideBrightnessDown = false;
    bool insideBrightnessUp = false;
    bool insideEffectPrev = false;
    bool insideEffectDetail = false;
    bool insideEffectNext = false;
    bool insideColor = false;
    bool insidePresetOpen = false;
    bool insideBack = false;
    bool insideHueDown = false;
    bool insideHueUp = false;
    bool insideSaturationDown = false;
    bool insideSaturationUp = false;
    bool insideSpeedDown = false;
    bool insideSpeedUp = false;
    bool insideIntensityDown = false;
    bool insideIntensityUp = false;
    bool insidePalettePrev = false;
    bool insidePaletteNext = false;
    bool insidePresetPrev = false;
    bool insidePresetNext = false;
    bool insidePresetManage = false;
    bool insidePresetSaveNew = false;
    bool insidePresetSaveHold = false;
    bool insidePresetOverwriteOpen = false;
    bool insidePresetOverwritePrev = false;
    bool insidePresetOverwriteNext = false;
    bool insidePresetOverwriteHold = false;
    bool insidePresetDeleteOpen = false;
    bool insidePresetDeletePrev = false;
    bool insidePresetDeleteNext = false;
    bool insidePresetDeleteHold = false;
    bool insidePresetBootOpen = false;
    bool insidePresetBootPrev = false;
    bool insidePresetBootNext = false;
    bool insidePresetBootHold = false;
  };

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

  bool lastTouchInsidePresetManage = false;
  bool lastTouchInsidePresetSaveNew = false;
  bool lastTouchInsidePresetSaveHold = false;

  bool lastTouchInsidePresetOverwriteOpen = false;
  bool lastTouchInsidePresetOverwriteNav = false;
  bool lastTouchInsidePresetOverwriteHold = false;

  bool lastTouchInsidePresetDeleteOpen = false;
  bool lastTouchInsidePresetDeleteNav = false;
  bool lastTouchInsidePresetDeleteHold = false;

  bool lastTouchInsidePresetBootOpen = false;
  bool lastTouchInsidePresetBootNav = false;
  bool lastTouchInsidePresetBootHold = false;

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

  bool presetManageButtonVisualPressed = false;
  bool presetSaveNewButtonVisualPressed = false;
  bool presetSaveHoldButtonVisualPressed = false;

  bool presetOverwriteOpenButtonVisualPressed = false;
  bool presetOverwriteNavButtonVisualPressed = false;
  bool presetOverwriteHoldButtonVisualPressed = false;

  bool presetDeleteOpenButtonVisualPressed = false;
  bool presetDeleteNavButtonVisualPressed = false;
  bool presetDeleteHoldButtonVisualPressed = false;

  bool presetBootOpenButtonVisualPressed = false;
  bool presetBootNavButtonVisualPressed = false;
  bool presetBootHoldButtonVisualPressed = false;

  // =========================================================
  // Shared long press / repeat timing state
  // =========================================================

  struct RepeatTouchState {
    unsigned long pressStart = 0;
    unsigned long lastRepeat = 0;
    bool longPressActive = false;
  };

  RepeatTouchState brightnessRepeatState;
  RepeatTouchState effectRepeatState;
  RepeatTouchState hueRepeatState;
  RepeatTouchState saturationRepeatState;
  RepeatTouchState speedRepeatState;
  RepeatTouchState intensityRepeatState;
  RepeatTouchState paletteRepeatState;
  RepeatTouchState presetRepeatState;

  int16_t lastTouchX = -1;
  int16_t lastTouchY = -1;

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

  static constexpr int16_t HEADER_CENTER_X = ( HEADER_CONTENT_LEFT + HEADER_CONTENT_RIGHT ) / 2;

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
  // MAIN bottom visible buttons
  // =========================================================

  static constexpr int16_t MAIN_BOTTOM_BUTTON_Y = 188;
  static constexpr int16_t MAIN_BOTTOM_BUTTON_H = 40;

  static constexpr int16_t COLOR_BUTTON_X = 16;
  static constexpr int16_t COLOR_BUTTON_W = 140;

  static constexpr int16_t PRESET_OPEN_BUTTON_X = 164;
  static constexpr int16_t PRESET_OPEN_BUTTON_W = 140;

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
  // PRESET screen layout
  // =========================================================

  static constexpr int16_t PRESET_NAME_Y = 92;
  static constexpr int16_t PRESET_ID_Y = 128;
  static constexpr int16_t PRESET_STATUS_Y = 154;

  static constexpr int16_t PRESET_NAV_LABEL_Y = 188;
  static constexpr int16_t PRESET_NAV_BUTTON_Y = 198;

  // =========================================================
  // PRESET MANAGE button on navigation screen
  // =========================================================

  static constexpr int16_t PRESET_MANAGE_BUTTON_X = 88;
  static constexpr int16_t PRESET_MANAGE_BUTTON_Y = 198;
  static constexpr int16_t PRESET_MANAGE_BUTTON_W = 144;
  static constexpr int16_t PRESET_MANAGE_BUTTON_H = 34;

  // =========================================================
  // PRESET MANAGE screen
  // =========================================================

  static constexpr int16_t PRESET_SAVE_NEW_BUTTON_X = 60;
  static constexpr int16_t PRESET_SAVE_NEW_BUTTON_Y = 64;
  static constexpr int16_t PRESET_SAVE_NEW_BUTTON_W = 200;
  static constexpr int16_t PRESET_SAVE_NEW_BUTTON_H = 34;

  // =========================================================
  // PRESET MANAGE OVERWRITE button
  // =========================================================

  static constexpr int16_t PRESET_OVERWRITE_BUTTON_X = 60;
  static constexpr int16_t PRESET_OVERWRITE_BUTTON_Y = 106;
  static constexpr int16_t PRESET_OVERWRITE_BUTTON_W = 200;
  static constexpr int16_t PRESET_OVERWRITE_BUTTON_H = 34;

  // =========================================================
  // PRESET MANAGE DELETE button
  // =========================================================

  static constexpr int16_t PRESET_DELETE_BUTTON_X = 60;
  static constexpr int16_t PRESET_DELETE_BUTTON_Y = 148;
  static constexpr int16_t PRESET_DELETE_BUTTON_W = 200;
  static constexpr int16_t PRESET_DELETE_BUTTON_H = 34;

  // =========================================================
  // PRESET MANAGE BOOT PRESET button
  // =========================================================

  static constexpr int16_t PRESET_BOOT_BUTTON_X = 60;
  static constexpr int16_t PRESET_BOOT_BUTTON_Y = 190;
  static constexpr int16_t PRESET_BOOT_BUTTON_W = 200;
  static constexpr int16_t PRESET_BOOT_BUTTON_H = 34;

  // =========================================================
  // PRESET SAVE confirmation screen
  // =========================================================

  static constexpr int16_t PRESET_SAVE_HOLD_BUTTON_X = 60;
  static constexpr int16_t PRESET_SAVE_HOLD_BUTTON_Y = 180;
  static constexpr int16_t PRESET_SAVE_HOLD_BUTTON_W = 200;
  static constexpr int16_t PRESET_SAVE_HOLD_BUTTON_H = 44;

  // =========================================================
  // PRESET OVERWRITE selection / confirmation screen
  // =========================================================

  static constexpr int16_t PRESET_OVERWRITE_NAV_BUTTON_Y = 124;

  static constexpr int16_t PRESET_OVERWRITE_HOLD_BUTTON_X = 60;
  static constexpr int16_t PRESET_OVERWRITE_HOLD_BUTTON_Y = 194;
  static constexpr int16_t PRESET_OVERWRITE_HOLD_BUTTON_W = 200;
  static constexpr int16_t PRESET_OVERWRITE_HOLD_BUTTON_H = 40;

  // =========================================================
  // PRESET DELETE selection / confirmation screen
  // =========================================================

  static constexpr int16_t PRESET_DELETE_NAV_BUTTON_Y = 124;

  static constexpr int16_t PRESET_DELETE_HOLD_BUTTON_X = 60;
  static constexpr int16_t PRESET_DELETE_HOLD_BUTTON_Y = 194;
  static constexpr int16_t PRESET_DELETE_HOLD_BUTTON_W = 200;
  static constexpr int16_t PRESET_DELETE_HOLD_BUTTON_H = 40;

  // =========================================================
  // PRESET BOOT selection / confirmation screen
  // =========================================================

  static constexpr int16_t PRESET_BOOT_NAV_BUTTON_Y = 124;

  static constexpr int16_t PRESET_BOOT_HOLD_BUTTON_X = 60;
  static constexpr int16_t PRESET_BOOT_HOLD_BUTTON_Y = 194;
  static constexpr int16_t PRESET_BOOT_HOLD_BUTTON_W = 200;
  static constexpr int16_t PRESET_BOOT_HOLD_BUTTON_H = 40;

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
  // Shared long press / repeat timing helpers
  // =========================================================

  void resetRepeatTouch( RepeatTouchState& state ) {
    state.pressStart = 0;
    state.lastRepeat = 0;
    state.longPressActive = false;
  }

  void beginRepeatTouch( RepeatTouchState& state, unsigned long now ) {
    state.pressStart = now;
    state.lastRepeat = now;
    state.longPressActive = false;
  }

  bool serviceRepeatTouch( RepeatTouchState& state, unsigned long now, unsigned long longPressMs, unsigned long repeatMs ) {
    if ( !state.longPressActive && now - state.pressStart >= longPressMs ) {
      state.longPressActive = true;
      state.lastRepeat = now;
      return true;
    }

    if ( state.longPressActive && now - state.lastRepeat >= repeatMs ) {
      state.lastRepeat = now;
      return true;
    }

    return false;
  }

  // =========================================================
  // Normal LCD brightness
  // =========================================================

  uint8_t getNormalDisplayBrightness() {
    return (uint8_t)constrain( (int)lcdBrightness, 1, 255 );
  }

  // =========================================================
  // Sleep timeout
  // =========================================================

  unsigned long getSleepTimeoutMs() {
    return (unsigned long)sleepTimeoutSec * 1000UL;
  }

  // =========================================================
  // Fade interval calculation
  // =========================================================

  unsigned long getFadeIntervalMs() {
    uint16_t normalBrightness = getNormalDisplayBrightness();

    uint16_t steps = ( normalBrightness + DISPLAY_FADE_STEP - 1 ) / DISPLAY_FADE_STEP;

    if ( steps == 0 ) {
      steps = 1;
    }

    unsigned long interval = (unsigned long)fadeDurationMs / steps;

    if ( interval < 1 ) {
      interval = 1;
    }

    return interval;
  }

  // =========================================================
  // Hardware backend facade
  //
  // Keep board-specific implementation behind one backend while
  // preserving the existing usermod call sites. This minimizes the
  // regression surface for the hardware-verified CoreS3 UI.
  // =========================================================

  const char* getHardwareProbeStateName() {
    return hardwareBackend.probeStateName();
  }

  const char* getDetectedPmuName() {
    return hardwareBackend.detectedPmuName();
  }

  const char* getDetectedImuName() {
    return hardwareBackend.detectedImuName();
  }

  const char* getDetectedVariantName() {
    return hardwareBackend.detectedVariantName();
  }

  bool isCore2FamilyProfile() {
    return hardwareBackend.isCore2FamilyProfile();
  }

  bool isCore2DiagnosticOnlyMode() {
    return hardwareBackend.isCore2DiagnosticOnlyMode();
  }

  const char* getHardwareRuntimeModeName() {
    return hardwareBackend.runtimeModeName();
  }

  const char* getHardwarePortStatusName() {
    return hardwareBackend.portStatusName();
  }

  const char* getDetectedRevisionName() {
    return hardwareBackend.detectedRevisionName();
  }

  void runHardwareDiagnostics() {
    hardwareBackend.runDiagnostics();
  }

  const char* getHardwareProfileName() {
    return hardwareBackend.profileName();
  }

  const char* getHardwareRevisionName() {
    return hardwareBackend.revisionName();
  }

  bool isHardwareDisplayRuntimeEnabled() {
    return hardwareBackend.isDisplayRuntimeEnabled();
  }

  bool initializeDisplayHardware() {
    return hardwareBackend.initializeDisplay( screenWidth, screenHeight, touchReady );
  }

  bool readDisplayTouch( int16_t& touchX, int16_t& touchY ) {
    return hardwareBackend.readTouch( touchX, touchY );
  }

  void writeDisplayBrightness( uint8_t value ) {
    hardwareBackend.writeBrightness( value );
  }

  // =========================================================
  // Display brightness
  // =========================================================

  void setDisplayBrightness( uint8_t value ) {
    currentDisplayBrightness = value;

    writeDisplayBrightness( value );
  }

  // =========================================================
  // Generic Fade
  // =========================================================

  bool updateFade( uint8_t targetBrightness, unsigned long now, unsigned long& lastFadeStep ) {
    if ( currentDisplayBrightness == targetBrightness ) {
      return true;
    }

    if (!fadeEnabled) {
      setDisplayBrightness( targetBrightness );

      return true;
    }

    unsigned long fadeInterval = getFadeIntervalMs();

    if ( now - lastFadeStep < fadeInterval ) {
      return false;
    }

    lastFadeStep = now;

    if ( currentDisplayBrightness < targetBrightness ) {
      int nextValue = currentDisplayBrightness + DISPLAY_FADE_STEP;

      if ( nextValue > targetBrightness ) {
        nextValue = targetBrightness;
      }

      setDisplayBrightness( (uint8_t)nextValue );
    }
    else {
      int nextValue = currentDisplayBrightness - DISPLAY_FADE_STEP;

      if ( nextValue < targetBrightness ) {
        nextValue = targetBrightness;
      }

      setDisplayBrightness( (uint8_t)nextValue );
    }

    return ( currentDisplayBrightness == targetBrightness );
  }

  // =========================================================
  // Preset cache rebuild start
  // =========================================================

  void startPresetCacheRebuild() {
    presetCacheCount = 0;

    presetCacheScanId = 1;

    presetCacheLastScanMs = 0;

    presetCacheReady = false;

    presetCacheBuilding = true;

    presetNoEntries = false;

    presetCacheBuildSourceModifiedTime = presetsModifiedTime;

    Serial.printf( "[CoreS3_Display] " "Preset cache rebuild start " "(modified=%lu)\n", presetCacheBuildSourceModifiedTime );
  }

  // =========================================================
  // Preset cache rebuild complete
  // =========================================================

  void finishPresetCacheRebuild() {
    presetCacheBuilding = false;

    presetCacheReady = true;

    presetCacheSourceModifiedTime = presetCacheBuildSourceModifiedTime;

    presetNoEntries = ( presetCacheCount == 0 );

    Serial.printf( "[CoreS3_Display] " "Preset cache ready: %u preset(s)\n", (unsigned)presetCacheCount );

    if ( presetsModifiedTime != presetCacheSourceModifiedTime ) {
      Serial.println( F( "[CoreS3_Display] " "Preset changed during cache build. Rebuilding." ) );

      startPresetCacheRebuild();

      return;
    }

    if ( currentPage == SCREEN_PRESET && presetSubPage == PRESET_SUBPAGE_NAV && displayPowerState == DISPLAY_POWER_ACTIVE && touchTarget == TOUCH_TARGET_NONE ) {
      drawPresetDetails( getDisplayedPresetId(), pendingPresetId > 0 );

      drawPresetNavigation( TOUCH_TARGET_NONE );

      lastPresetValue = getDisplayedPresetId();
    }
    else if ( currentPage == SCREEN_PRESET && presetSubPage == PRESET_SUBPAGE_MANAGE && displayPowerState == DISPLAY_POWER_ACTIVE && touchTarget == TOUCH_TARGET_NONE ) {
      drawPresetManageScreen();
    }
    else if ( currentPage == SCREEN_PRESET && presetSubPage == PRESET_SUBPAGE_SAVE && presetSaveOperationState == PRESET_SAVE_OP_IDLE && displayPowerState == DISPLAY_POWER_ACTIVE && touchTarget == TOUCH_TARGET_NONE ) {
      preparePresetSaveCandidate();
      drawPresetSaveScreen();
    }
    else if ( currentPage == SCREEN_PRESET && presetSubPage == PRESET_SUBPAGE_OVERWRITE && presetSaveOperationState == PRESET_SAVE_OP_IDLE && displayPowerState == DISPLAY_POWER_ACTIVE && touchTarget == TOUCH_TARGET_NONE ) {
      String refreshedName;

      if ( presetOverwriteTargetId == 0 || !getCachedPresetName( presetOverwriteTargetId, refreshedName ) ) {
        preparePresetOverwriteTarget();
      }
      else {
        presetOverwriteTargetName = refreshedName;
      }

      drawPresetOverwriteScreen();
    }
    else if ( currentPage == SCREEN_PRESET && presetSubPage == PRESET_SUBPAGE_DELETE && presetDeleteOperationState == PRESET_DELETE_OP_IDLE && displayPowerState == DISPLAY_POWER_ACTIVE && touchTarget == TOUCH_TARGET_NONE ) {
      String refreshedName;

      if ( presetDeleteTargetId == 0 || !getCachedPresetName( presetDeleteTargetId, refreshedName ) ) {
        preparePresetDeleteTarget();
      }
      else {
        presetDeleteTargetName = refreshedName;
      }

      drawPresetDeleteScreen();
    }
    else if ( currentPage == SCREEN_PRESET && presetSubPage == PRESET_SUBPAGE_BOOT && presetBootOperationState == PRESET_BOOT_OP_IDLE && displayPowerState == DISPLAY_POWER_ACTIVE && touchTarget == TOUCH_TARGET_NONE ) {
      if ( presetBootTargetId > 0 ) {
        String refreshedName;

        if ( getCachedPresetName( presetBootTargetId, refreshedName ) ) {
          presetBootTargetName = refreshedName;
        }
        else {
          presetBootTargetId = 0;
          presetBootTargetName = "NONE";
        }
      }

      drawPresetBootScreen();
    }
  }

  // =========================================================
  // Background Preset cache service
  // =========================================================

  void servicePresetCache() {
    if ( presetCacheReady && !presetCacheBuilding && presetsModifiedTime != presetCacheSourceModifiedTime ) {
      startPresetCacheRebuild();
    }

    if (!presetCacheBuilding) {
      return;
    }

    if ( pendingPresetId > 0 ) {
      return;
    }

    if ( presetNeedsSaving() ) {
      return;
    }

    unsigned long now = millis();

    if ( now - presetCacheLastScanMs < PRESET_CACHE_SCAN_INTERVAL_MS ) {
      return;
    }

    presetCacheLastScanMs = now;

    if ( presetCacheScanId > 250 ) {
      finishPresetCacheRebuild();

      return;
    }

    String presetName;

    uint8_t scanId = (uint8_t)presetCacheScanId;

    if ( getPresetName( scanId, presetName ) ) {
      if ( presetCacheCount < 250 ) {
        presetCache[ presetCacheCount ].id = scanId;

        strlcpy( presetCache[ presetCacheCount ].name, presetName.c_str(), sizeof( presetCache[ presetCacheCount ].name ) );

        presetCacheCount++;
      }
    }

    presetCacheScanId++;

    if ( presetCacheScanId > 250 ) {
      finishPresetCacheRebuild();
    }
  }

  // =========================================================
  // Find Preset in RAM cache
  // =========================================================

  int findPresetCacheIndex( uint8_t presetId ) {
    for ( uint16_t i = 0; i < presetCacheCount; i++ ) {
      if ( presetCache[i].id == presetId ) {
        return (int)i;
      }
    }

    return -1;
  }

  bool getCachedPresetName( uint8_t presetId, String& name ) {
    int index = findPresetCacheIndex( presetId );

    if ( index < 0 ) {
      return false;
    }

    name = presetCache[ index ].name;

    return true;
  }

  uint8_t findFirstFreePresetId() {
    if ( !presetCacheReady || presetCacheBuilding ) {
      return 0;
    }

    uint16_t expectedId = 1;

    for ( uint16_t i = 0; i < presetCacheCount; i++ ) {
      uint8_t cachedId = presetCache[i].id;

      if ( cachedId < expectedId ) {
        continue;
      }

      if ( cachedId == expectedId ) {
        expectedId++;

        if ( expectedId > 250 ) {
          return 0;
        }

        continue;
      }

      break;
    }

    if ( expectedId >= 1 && expectedId <= 250 ) {
      return (uint8_t)expectedId;
    }

    return 0;
  }

  bool preparePresetSaveCandidate() {
    presetSaveCandidateId = findFirstFreePresetId();

    presetSaveCandidateName = "";

    if ( presetSaveCandidateId == 0 ) {
      return false;
    }

    char presetName[33];

    snprintf( presetName, sizeof(presetName), "CoreS3 Preset %u", presetSaveCandidateId );

    presetSaveCandidateName = presetName;

    return true;
  }

  bool preparePresetOverwriteTarget() {
    presetOverwriteTargetId = 0;

    presetOverwriteTargetName = "";

    if ( !presetCacheReady || presetCacheBuilding || presetCacheCount == 0 ) {
      return false;
    }

    int currentIndex = findPresetCacheIndex( currentPreset );

    uint16_t targetIndex = ( currentIndex >= 0 ) ? (uint16_t)currentIndex : 0;

    presetOverwriteTargetId = presetCache[ targetIndex ].id;

    presetOverwriteTargetName = presetCache[ targetIndex ].name;

    return true;
  }

  bool stepPresetOverwriteTarget( int direction ) {
    if ( direction == 0 || !presetCacheReady || presetCacheBuilding || presetCacheCount == 0 ) {
      return false;
    }

    String targetName;

    uint8_t targetId = findAdjacentPreset( presetOverwriteTargetId, direction, &targetName );

    if ( targetId == 0 ) {
      return false;
    }

    presetOverwriteTargetId = targetId;

    presetOverwriteTargetName = targetName;

    return true;
  }

  bool preparePresetDeleteTarget() {
    presetDeleteTargetId = 0;

    presetDeleteTargetName = "";

    if ( !presetCacheReady || presetCacheBuilding || presetCacheCount == 0 ) {
      return false;
    }

    int currentIndex = findPresetCacheIndex( currentPreset );

    uint16_t targetIndex = ( currentIndex >= 0 ) ? (uint16_t)currentIndex : 0;

    presetDeleteTargetId = presetCache[ targetIndex ].id;

    presetDeleteTargetName = presetCache[ targetIndex ].name;

    return true;
  }

  bool stepPresetDeleteTarget( int direction ) {
    if ( direction == 0 || !presetCacheReady || presetCacheBuilding || presetCacheCount == 0 ) {
      return false;
    }

    String targetName;

    uint8_t targetId = findAdjacentPreset( presetDeleteTargetId, direction, &targetName );

    if ( targetId == 0 ) {
      return false;
    }

    presetDeleteTargetId = targetId;

    presetDeleteTargetName = targetName;

    return true;
  }

  bool preparePresetBootTarget() {
    presetBootTargetId = 0;
    presetBootTargetName = "NONE";

    if ( !presetCacheReady || presetCacheBuilding ) {
      return false;
    }

    if ( currentPreset > 0 ) {
      String currentName;

      if ( getCachedPresetName( currentPreset, currentName ) ) {
        presetBootTargetId = currentPreset;
        presetBootTargetName = currentName;

        return true;
      }
    }

    if ( bootPreset > 0 ) {
      String bootName;

      if ( getCachedPresetName( bootPreset, bootName ) ) {
        presetBootTargetId = bootPreset;
        presetBootTargetName = bootName;
      }
    }

    return true;
  }

  bool stepPresetBootTarget( int direction ) {
    if ( direction == 0 || !presetCacheReady || presetCacheBuilding || presetCacheCount == 0 ) {
      return false;
    }

    if ( presetBootTargetId == 0 ) {
      uint16_t targetIndex = ( direction > 0 ) ? 0 : ( presetCacheCount - 1 );

      presetBootTargetId = presetCache[targetIndex].id;
      presetBootTargetName = presetCache[targetIndex].name;

      return true;
    }

    int currentIndex = findPresetCacheIndex( presetBootTargetId );

    if ( currentIndex < 0 ) {
      presetBootTargetId = 0;
      presetBootTargetName = "NONE";

      return true;
    }

    if ( direction > 0 ) {
      if ( currentIndex >= (int)presetCacheCount - 1 ) {
        presetBootTargetId = 0;
        presetBootTargetName = "NONE";
      }
      else {
        presetBootTargetId = presetCache[currentIndex + 1].id;
        presetBootTargetName = presetCache[currentIndex + 1].name;
      }
    }
    else {
      if ( currentIndex <= 0 ) {
        presetBootTargetId = 0;
        presetBootTargetName = "NONE";
      }
      else {
        presetBootTargetId = presetCache[currentIndex - 1].id;
        presetBootTargetName = presetCache[currentIndex - 1].name;
      }
    }

    return true;
  }

  bool isPresetSaveBusy() {
    return ( presetSaveOperationState != PRESET_SAVE_OP_IDLE );
  }

  bool isPresetDeleteBusy() {
    return ( presetDeleteOperationState != PRESET_DELETE_OP_IDLE );
  }

  bool isPresetBootBusy() {
    return ( presetBootOperationState != PRESET_BOOT_OP_IDLE );
  }

  bool requestNewPresetSave() {
    presetSaveOperationIsOverwrite = false;

    if ( presetSaveOperationState != PRESET_SAVE_OP_IDLE || !presetCacheReady || presetCacheBuilding || pendingPresetId > 0 || presetSaveCandidateId == 0 || findPresetCacheIndex( presetSaveCandidateId ) >= 0 || presetNeedsSaving() ) {
      presetSaveOperationState = PRESET_SAVE_OP_FAILED;

      presetSaveResultStartMs = millis();

      drawPresetSaveOperationStatus();

      Serial.println( F( "[CoreS3_Display] " "Preset save request rejected" ) );

      return false;
    }

    savePreset( presetSaveCandidateId, presetSaveCandidateName.c_str() );

    if ( !presetNeedsSaving() ) {
      presetSaveOperationState = PRESET_SAVE_OP_FAILED;

      presetSaveResultStartMs = millis();

      drawPresetSaveOperationStatus();

      Serial.println( F( "[CoreS3_Display] " "Preset save could not be queued" ) );

      return false;
    }

    presetSaveOperationState = PRESET_SAVE_OP_WAIT_WLED;

    presetSaveResultStartMs = 0;

    lastUserActivityMs = millis();

    drawPresetSaveOperationStatus();

    Serial.printf( "[CoreS3_Display] " "Preset save request: %u (%s)\n", presetSaveCandidateId, presetSaveCandidateName.c_str() );

    return true;
  }

  bool requestPresetOverwrite() {
    presetSaveOperationIsOverwrite = true;

    presetSaveCandidateId = presetOverwriteTargetId;

    presetSaveCandidateName = presetOverwriteTargetName;

    if ( presetSaveOperationState != PRESET_SAVE_OP_IDLE || !presetCacheReady || presetCacheBuilding || presetCacheCount == 0 || pendingPresetId > 0 || presetSaveCandidateId == 0 || findPresetCacheIndex( presetSaveCandidateId ) < 0 || presetSaveCandidateName.length() == 0 || presetNeedsSaving() ) {
      presetSaveOperationState = PRESET_SAVE_OP_FAILED;

      presetSaveResultStartMs = millis();

      drawPresetSaveOperationStatus();

      Serial.println( F( "[CoreS3_Display] " "Preset overwrite request rejected" ) );

      return false;
    }

    savePreset( presetSaveCandidateId, presetSaveCandidateName.c_str() );

    if ( !presetNeedsSaving() ) {
      presetSaveOperationState = PRESET_SAVE_OP_FAILED;

      presetSaveResultStartMs = millis();

      drawPresetSaveOperationStatus();

      Serial.println( F( "[CoreS3_Display] " "Preset overwrite could not be queued" ) );

      return false;
    }

    presetSaveOperationState = PRESET_SAVE_OP_WAIT_WLED;

    presetSaveResultStartMs = 0;

    lastUserActivityMs = millis();

    drawPresetSaveOperationStatus();

    Serial.printf( "[CoreS3_Display] " "Preset overwrite request: %u (%s)\n", presetSaveCandidateId, presetSaveCandidateName.c_str() );

    return true;
  }

  bool requestPresetDelete() {
    if ( presetDeleteOperationState != PRESET_DELETE_OP_IDLE || !presetCacheReady || presetCacheBuilding || presetCacheCount == 0 || pendingPresetId > 0 || presetDeleteTargetId == 0 || findPresetCacheIndex( presetDeleteTargetId ) < 0 || presetDeleteTargetName.length() == 0 || presetNeedsSaving() ) {
      presetDeleteOperationState = PRESET_DELETE_OP_FAILED;

      presetDeleteResultStartMs = millis();

      drawPresetDeleteOperationStatus();

      Serial.println( F( "[CoreS3_Display] " "Preset delete request rejected" ) );

      return false;
    }

    presetDeleteWasCurrentPreset = ( currentPreset == presetDeleteTargetId );
    presetDeleteWasBootPreset = ( bootPreset == presetDeleteTargetId );

    deletePreset( presetDeleteTargetId );

    if ( !presetCacheBuilding ) {
      startPresetCacheRebuild();
    }

    presetDeleteOperationState = PRESET_DELETE_OP_WAIT_CACHE;

    presetDeleteResultStartMs = 0;

    lastUserActivityMs = millis();

    drawPresetDeleteOperationStatus();

    Serial.printf( "[CoreS3_Display] " "Preset delete request: %u (%s)\n", presetDeleteTargetId, presetDeleteTargetName.c_str() );

    return true;
  }

  bool requestPresetBootSetting() {
    bool validTarget =
      presetBootTargetId == 0 ||
      findPresetCacheIndex( presetBootTargetId ) >= 0;

    if ( presetBootOperationState != PRESET_BOOT_OP_IDLE ||
         !presetCacheReady ||
         presetCacheBuilding ||
         pendingPresetId > 0 ||
         presetNeedsSaving() ||
         !validTarget ||
         presetBootTargetId == bootPreset ) {
      presetBootOperationState = PRESET_BOOT_OP_FAILED;
      presetBootResultStartMs = millis();

      drawPresetBootOperationStatus();

      Serial.println( F( "[CoreS3_Display] " "Boot Preset request rejected" ) );

      return false;
    }

    bootPreset = presetBootTargetId;
    configNeedsWrite = true;

    presetBootOperationState = PRESET_BOOT_OP_WAIT_CONFIG;
    presetBootResultStartMs = 0;
    lastUserActivityMs = millis();

    drawPresetBootOperationStatus();

    if ( presetBootTargetId == 0 ) {
      Serial.println( F( "[CoreS3_Display] " "Boot Preset clear request" ) );
    }
    else {
      Serial.printf( "[CoreS3_Display] " "Boot Preset request: %u (%s)\n", presetBootTargetId, presetBootTargetName.c_str() );
    }

    return true;
  }

  void servicePresetSaveOperation() {
    if ( presetSaveOperationState == PRESET_SAVE_OP_IDLE ) {
      return;
    }

    unsigned long now = millis();

    if ( presetSaveOperationState == PRESET_SAVE_OP_WAIT_WLED ) {
      if ( presetNeedsSaving() ) {
        return;
      }

      if ( !presetCacheBuilding ) {
        startPresetCacheRebuild();
      }

      presetSaveOperationState = PRESET_SAVE_OP_WAIT_CACHE;

      if ( currentPage == SCREEN_PRESET && ( presetSubPage == PRESET_SUBPAGE_SAVE || presetSubPage == PRESET_SUBPAGE_OVERWRITE ) && displayPowerState == DISPLAY_POWER_ACTIVE ) {
        drawPresetSaveOperationStatus();
      }

      return;
    }

    if ( presetSaveOperationState == PRESET_SAVE_OP_WAIT_CACHE ) {
      if ( !presetCacheReady || presetCacheBuilding ) {
        return;
      }

      bool presetVerified = ( findPresetCacheIndex( presetSaveCandidateId ) >= 0 );

      if ( presetVerified && presetSaveOperationIsOverwrite ) {
        String verifiedName;

        presetVerified = getCachedPresetName( presetSaveCandidateId, verifiedName ) && verifiedName == presetSaveCandidateName;
      }

      if (presetVerified) {
        presetSaveOperationState = PRESET_SAVE_OP_SUCCESS;

        Serial.printf( "[CoreS3_Display] " "%s verified: %u (%s)\n", presetSaveOperationIsOverwrite ? "Preset overwrite" : "Preset save", presetSaveCandidateId, presetSaveCandidateName.c_str() );
      }
      else {
        presetSaveOperationState = PRESET_SAVE_OP_FAILED;

        Serial.printf( "[CoreS3_Display] " "%s verification failed: %u\n", presetSaveOperationIsOverwrite ? "Preset overwrite" : "Preset save", presetSaveCandidateId );
      }

      presetSaveResultStartMs = now;

      if ( currentPage == SCREEN_PRESET && ( presetSubPage == PRESET_SUBPAGE_SAVE || presetSubPage == PRESET_SUBPAGE_OVERWRITE ) && displayPowerState == DISPLAY_POWER_ACTIVE ) {
        drawPresetSaveOperationStatus();
      }

      return;
    }

    if ( presetSaveOperationState == PRESET_SAVE_OP_SUCCESS || presetSaveOperationState == PRESET_SAVE_OP_FAILED ) {
      if ( now - presetSaveResultStartMs < PRESET_SAVE_RESULT_HOLD_MS ) {
        return;
      }

      int16_t touchX = -1;
      int16_t touchY = -1;

      if ( readDisplayTouch( touchX, touchY ) ) {
        return;
      }

      bool saveSucceeded = ( presetSaveOperationState == PRESET_SAVE_OP_SUCCESS );

      bool completedOverwrite = presetSaveOperationIsOverwrite;

      presetSaveOperationState = PRESET_SAVE_OP_IDLE;

      presetSaveOperationIsOverwrite = false;

      presetSaveResultStartMs = 0;

      presetSaveHoldStartTime = 0;

      presetSaveHoldTriggered = false;

      resetTouchGesture();

      if (completedOverwrite) {
        presetOverwriteTargetId = 0;

        presetOverwriteTargetName = "";
      }

      if (saveSucceeded) {
        presetSaveCandidateId = 0;

        presetSaveCandidateName = "";

        drawPresetScreen();
      }
      else {
        presetSaveCandidateId = 0;

        presetSaveCandidateName = "";

        drawPresetManageScreen();
      }
    }
  }

  void servicePresetDeleteOperation() {
    if ( presetDeleteOperationState == PRESET_DELETE_OP_IDLE ) {
      return;
    }

    unsigned long now = millis();

    if ( presetDeleteOperationState == PRESET_DELETE_OP_WAIT_CACHE ) {
      if ( !presetCacheReady || presetCacheBuilding ) {
        return;
      }

      bool deleteVerified = ( findPresetCacheIndex( presetDeleteTargetId ) < 0 );

      if (deleteVerified) {
        if ( presetDeleteWasCurrentPreset && currentPreset == presetDeleteTargetId ) {
          currentPreset = 0;

          lastPresetValue = -1;
        }

        if ( presetDeleteWasBootPreset && bootPreset == presetDeleteTargetId ) {
          bootPreset = 0;
          configNeedsWrite = true;
          lastBootPresetValue = -1;

          Serial.println( F( "[CoreS3_Display] " "Deleted Boot Preset cleared" ) );
        }

        presetDeleteOperationState = PRESET_DELETE_OP_SUCCESS;

        Serial.printf( "[CoreS3_Display] " "Preset delete verified: %u (%s)\n", presetDeleteTargetId, presetDeleteTargetName.c_str() );
      }
      else {
        presetDeleteOperationState = PRESET_DELETE_OP_FAILED;

        Serial.printf( "[CoreS3_Display] " "Preset delete verification failed: %u\n", presetDeleteTargetId );
      }

      presetDeleteResultStartMs = now;

      if ( currentPage == SCREEN_PRESET && presetSubPage == PRESET_SUBPAGE_DELETE && displayPowerState == DISPLAY_POWER_ACTIVE ) {
        drawPresetDeleteOperationStatus();
      }

      return;
    }

    if ( presetDeleteOperationState == PRESET_DELETE_OP_SUCCESS || presetDeleteOperationState == PRESET_DELETE_OP_FAILED ) {
      if ( now - presetDeleteResultStartMs < PRESET_SAVE_RESULT_HOLD_MS ) {
        return;
      }

      int16_t touchX = -1;
      int16_t touchY = -1;

      if ( readDisplayTouch( touchX, touchY ) ) {
        return;
      }

      bool deleteSucceeded = ( presetDeleteOperationState == PRESET_DELETE_OP_SUCCESS );

      presetDeleteOperationState = PRESET_DELETE_OP_IDLE;

      presetDeleteResultStartMs = 0;

      presetDeleteWasCurrentPreset = false;
      presetDeleteWasBootPreset = false;

      presetSaveHoldStartTime = 0;

      presetSaveHoldTriggered = false;

      resetTouchGesture();

      presetDeleteTargetId = 0;

      presetDeleteTargetName = "";

      if (deleteSucceeded) {
        drawPresetScreen();
      }
      else {
        drawPresetManageScreen();
      }
    }
  }

  void servicePresetBootOperation() {
    if ( presetBootOperationState == PRESET_BOOT_OP_IDLE ) {
      return;
    }

    unsigned long now = millis();

    if ( presetBootOperationState == PRESET_BOOT_OP_WAIT_CONFIG ) {
      if ( configNeedsWrite ) {
        return;
      }

      if ( bootPreset == presetBootTargetId ) {
        presetBootOperationState = PRESET_BOOT_OP_SUCCESS;

        if ( presetBootTargetId == 0 ) {
          Serial.println( F( "[CoreS3_Display] " "Boot Preset clear verified" ) );
        }
        else {
          Serial.printf( "[CoreS3_Display] " "Boot Preset verified: %u (%s)\n", presetBootTargetId, presetBootTargetName.c_str() );
        }
      }
      else {
        presetBootOperationState = PRESET_BOOT_OP_FAILED;

        Serial.printf( "[CoreS3_Display] " "Boot Preset verification failed: target=%u actual=%u\n", presetBootTargetId, bootPreset );
      }

      presetBootResultStartMs = now;
      lastBootPresetValue = bootPreset;

      if ( currentPage == SCREEN_PRESET && presetSubPage == PRESET_SUBPAGE_BOOT && displayPowerState == DISPLAY_POWER_ACTIVE ) {
        drawPresetBootOperationStatus();
      }

      return;
    }

    if ( presetBootOperationState == PRESET_BOOT_OP_SUCCESS || presetBootOperationState == PRESET_BOOT_OP_FAILED ) {
      if ( now - presetBootResultStartMs < PRESET_SAVE_RESULT_HOLD_MS ) {
        return;
      }

      int16_t touchX = -1;
      int16_t touchY = -1;

      if ( readDisplayTouch( touchX, touchY ) ) {
        return;
      }

      presetBootOperationState = PRESET_BOOT_OP_IDLE;
      presetBootResultStartMs = 0;
      presetSaveHoldStartTime = 0;
      presetSaveHoldTriggered = false;

      resetTouchGesture();

      presetBootTargetId = 0;
      presetBootTargetName = "NONE";

      drawPresetManageScreen();
    }
  }

  // =========================================================
  // Startup logo
  // =========================================================

  void drawStartupBase() {
    display.fillScreen( TFT_BLACK );

    bool logoResult = display.drawPng( CORES3_WLED_LOGO_PNG, CORES3_WLED_LOGO_PNG_LEN, 8, 20 );

    if (!logoResult) {
      display.setTextDatum( textdatum_t::middle_center );

      display.setTextColor( TFT_WHITE, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( "WLED M5Stack CoreS3", screenWidth / 2, 68 );

      Serial.println( F( "[CoreS3_Display] " "WARNING: startup PNG draw failed" ) );
    }
  }

  void drawStartupConnectingStatus() {
    display.fillRect( 0, 125, screenWidth, 100, TFT_BLACK );

    display.setTextDatum( textdatum_t::middle_center );
    display.setTextColor( TFT_WHITE, TFT_BLACK );
    display.setTextSize( 2 );

    // Reserve the width of all three dots from the beginning so the
    // "Wi-Fi Connecting" label never shifts while the dots animate.
    const char* label = "Wi-Fi Connecting";
    const int16_t labelWidth = display.textWidth( label );
    const int16_t dotWidth = display.textWidth( "." );
    const int16_t reservedDotsWidth = dotWidth * 3;

    const int16_t labelCenterX =
      ( screenWidth / 2 ) - ( reservedDotsWidth / 2 );

    display.drawString( label, labelCenterX, 153 );

    const int16_t labelRight =
      labelCenterX + ( labelWidth / 2 );

    for ( uint8_t i = 0; i < startupDotCount && i < 3; i++ ) {
      const int16_t dotCenterX =
        labelRight + ( dotWidth / 2 ) + ( i * dotWidth );

      display.drawString( ".", dotCenterX, 153 );
    }

    display.setTextSize( 1 );
    display.setTextColor( TFT_DARKGREY, TFT_BLACK );

    display.drawString( "Starting WLED...", screenWidth / 2, 185 );
  }


  void drawStartupConnectedStatus( const String& ipAddress ) {
    display.fillRect( 0, 125, screenWidth, 100, TFT_BLACK );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( TFT_GREEN, TFT_BLACK );

    display.setTextSize( 2 );

    display.drawString( "Wi-Fi Connected", screenWidth / 2, 150 );

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    display.setTextSize( 1 );

    display.drawString( ipAddress, screenWidth / 2, 181 );
  }

  void handleStartupSequence() {
    if ( startupState == STARTUP_DONE ) {
      return;
    }

    unsigned long now = millis();

    if ( startupState == STARTUP_FADE_IN ) {
      if ( updateFade( getNormalDisplayBrightness(), now, startupLastFadeStep ) ) {
        startupState = STARTUP_WAIT_WIFI;

        startupStateStart = now;

        startupLastDotsUpdate = now;
      }

      return;
    }

    if ( startupState == STARTUP_WAIT_WIFI ) {
      if ( now - startupLastDotsUpdate >= STARTUP_DOTS_INTERVAL_MS ) {
        startupLastDotsUpdate = now;

        startupDotCount++;

        if ( startupDotCount > 3 ) {
          startupDotCount = 0;
        }

        drawStartupConnectingStatus();
      }

      if ( WiFi.status() == WL_CONNECTED ) {
        startupIPAddress = WiFi.localIP().toString();

        drawStartupConnectedStatus( startupIPAddress );

        startupState = STARTUP_CONNECTED_HOLD;

        startupStateStart = now;
      }

      return;
    }

    if ( startupState == STARTUP_CONNECTED_HOLD ) {
      if ( now - startupStateStart >= STARTUP_CONNECTED_HOLD_MS ) {
        startupState = STARTUP_FADE_OUT;

        startupLastFadeStep = now;
      }

      return;
    }

    if ( startupState == STARTUP_FADE_OUT ) {
      if ( updateFade( 0, now, startupLastFadeStep ) ) {
        drawMainScreen( startupIPAddress );

        setDisplayBrightness( 0 );

        startupState = STARTUP_MAIN_FADE_IN;

        startupLastFadeStep = now;
      }

      return;
    }

    if ( startupState == STARTUP_MAIN_FADE_IN ) {
      if ( updateFade( getNormalDisplayBrightness(), now, startupLastFadeStep ) ) {
        startupState = STARTUP_DONE;

        lastWiFiConnected = true;

        lastIPAddress = startupIPAddress;

        readyScreenShown = true;

        connectingScreenShown = false;

        lastUserActivityMs = now;

        displayPowerState = DISPLAY_POWER_ACTIVE;

        Serial.println( F( "[CoreS3_Display] " "Startup animation complete" ) );
      }

      return;
    }
  }

  bool pollWakeTouch( unsigned long now ) {
    if ( now - wakeTouchLastPoll < TOUCH_POLL_MS ) {
      return wakeTouchState;
    }

    wakeTouchLastPoll = now;

    int16_t x = -1;
    int16_t y = -1;

    wakeTouchState = ( readDisplayTouch( x, y ) );

    return wakeTouchState;
  }

  bool settlePendingPreset() {
    if ( pendingPresetId == 0 ) {
      return false;
    }

    bool completed = ( currentPreset == pendingPresetId );

    bool timedOut = ( millis() - pendingPresetRequestMs >= PRESET_APPLY_PENDING_MS );

    if ( !completed && !timedOut ) {
      return false;
    }

    pendingPresetId = 0;

    pendingPresetName = "";

    pendingPresetRequestMs = 0;

    return true;
  }

  uint8_t getDisplayedPresetId() {
    if ( pendingPresetId > 0 ) {
      if ( millis() - pendingPresetRequestMs < PRESET_APPLY_PENDING_MS ) {
        return pendingPresetId;
      }
    }

    return currentPreset;
  }

  void redrawCurrentPageForWake() {
    settlePendingPreset();

    if ( WiFi.status() != WL_CONNECTED ) {
      drawConnectingScreen();

      return;
    }

    if ( currentPage == SCREEN_COLOR ) {
      drawColorScreen();

      return;
    }

    if ( currentPage == SCREEN_EFFECT ) {
      drawEffectDetailScreen();

      return;
    }

    if ( currentPage == SCREEN_PRESET ) {
      if ( presetSubPage == PRESET_SUBPAGE_MANAGE ) {
        drawPresetManageScreen();
      }
      else if ( presetSubPage == PRESET_SUBPAGE_SAVE ) {
        drawPresetSaveScreen();
      }
      else if ( presetSubPage == PRESET_SUBPAGE_OVERWRITE ) {
        drawPresetOverwriteScreen();
      }
      else if ( presetSubPage == PRESET_SUBPAGE_DELETE ) {
        drawPresetDeleteScreen();
      }
      else if ( presetSubPage == PRESET_SUBPAGE_BOOT ) {
        drawPresetBootScreen();
      }
      else {
        drawPresetScreen();
      }

      return;
    }

    drawMainScreen( WiFi.localIP().toString() );
  }

  void beginDisplaySleep( unsigned long now ) {
    Serial.println( F( "[CoreS3_Display] " "Display sleep start" ) );

    resetTouchGesture();

    displayPowerState = DISPLAY_POWER_SLEEP_FADE_OUT;

    displayFadeLastStep = now;

    wakeTouchLastPoll = 0;

    wakeTouchState = false;

    wakeReleaseCandidate = 0;
  }

  void beginDisplayWake( unsigned long now ) {
    Serial.println( F( "[CoreS3_Display] " "Display wake start" ) );

    resetTouchGesture();

    setDisplayBrightness( 0 );

    redrawCurrentPageForWake();

    setDisplayBrightness( 0 );

    displayPowerState = DISPLAY_POWER_WAKE_FADE_IN;

    displayFadeLastStep = now;

    wakeReleaseCandidate = 0;

    lastUserActivityMs = now;
  }

  bool handleDisplayPowerManagement() {
    unsigned long now = millis();

    if ( displayPowerState == DISPLAY_POWER_ACTIVE ) {
      if ( sleepTimeoutSec > 0 && !touchActive && now - lastUserActivityMs >= getSleepTimeoutMs() ) {
        beginDisplaySleep( now );

        return true;
      }

      return false;
    }

    if ( displayPowerState == DISPLAY_POWER_SLEEP_FADE_OUT ) {
      if ( pollWakeTouch( now ) ) {
        beginDisplayWake( now );

        return true;
      }

      if ( updateFade( 0, now, displayFadeLastStep ) ) {
        displayPowerState = DISPLAY_POWER_SLEEPING;

        setDisplayBrightness( 0 );

        Serial.println( F( "[CoreS3_Display] " "Display sleeping" ) );
      }

      return true;
    }

    if ( displayPowerState == DISPLAY_POWER_SLEEPING ) {
      if ( pollWakeTouch( now ) ) {
        beginDisplayWake( now );
      }

      return true;
    }

    if ( displayPowerState == DISPLAY_POWER_WAKE_FADE_IN ) {
      pollWakeTouch( now );

      if ( updateFade( getNormalDisplayBrightness(), now, displayFadeLastStep ) ) {
        displayPowerState = DISPLAY_POWER_WAKE_WAIT_RELEASE;

        wakeReleaseCandidate = 0;

        Serial.println( F( "[CoreS3_Display] " "Display wake fade complete" ) );
      }

      return true;
    }

    if ( displayPowerState == DISPLAY_POWER_WAKE_WAIT_RELEASE ) {
      bool touching = pollWakeTouch( now );

      if (touching) {
        wakeReleaseCandidate = 0;

        return true;
      }

      if ( wakeReleaseCandidate == 0 ) {
        wakeReleaseCandidate = now;

        return true;
      }

      if ( now - wakeReleaseCandidate >= TOUCH_RELEASE_CONFIRM_MS ) {
        displayPowerState = DISPLAY_POWER_ACTIVE;

        lastUserActivityMs = now;

        wakeReleaseCandidate = 0;

        wakeTouchState = false;

        resetTouchGesture();

        Serial.println( F( "[CoreS3_Display] " "Display active" ) );
      }

      return true;
    }

    return false;
  }

  uint16_t rgbTo565( uint8_t r, uint8_t g, uint8_t b ) {
    return ( ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | ((uint16_t)b >> 3) );
  }

  uint32_t getPrimaryColor() {
    if ( strip.getSegmentsNum() > 0 ) {
      return strip.getMainSegment().colors[0];
    }

    return 0;
  }

  uint8_t getCurrentEffectMode() {
    if ( strip.getSegmentsNum() > 0 ) {
      return strip.getMainSegment().mode;
    }

    return 0;
  }

  uint8_t getCurrentSpeed() {
    if ( strip.getSegmentsNum() > 0 ) {
      return strip.getMainSegment().speed;
    }

    return 0;
  }

  uint8_t getCurrentIntensity() {
    if ( strip.getSegmentsNum() > 0 ) {
      return strip.getMainSegment().intensity;
    }

    return 0;
  }

  uint8_t getCurrentPalette() {
    if ( strip.getSegmentsNum() > 0 ) {
      return strip.getMainSegment().palette;
    }

    return 0;
  }

  size_t getSelectablePaletteCount() {
    return FIXED_PALETTE_COUNT + customPalettes.size() + usermodPalettes.size();
  }

  uint8_t paletteIdFromSequenceIndex( size_t sequenceIndex ) {
    if ( sequenceIndex < FIXED_PALETTE_COUNT ) {
      return (uint8_t)sequenceIndex;
    }

    sequenceIndex -= FIXED_PALETTE_COUNT;

    if ( sequenceIndex < customPalettes.size() ) {
      return (uint8_t)( WLED_CUSTOM_PALETTE_ID_BASE - sequenceIndex );
    }

    sequenceIndex -= customPalettes.size();

    if ( sequenceIndex < usermodPalettes.size() ) {
      return (uint8_t)( WLED_USERMOD_PALETTE_ID_BASE - sequenceIndex );
    }

    return 0;
  }

  int findPaletteSequenceIndex( uint8_t paletteId ) {
    if ( paletteId < FIXED_PALETTE_COUNT ) {
      return paletteId;
    }

    if ( paletteId > WLED_CUSTOM_PALETTE_ID_BASE ) {
      size_t usermodIndex = WLED_USERMOD_PALETTE_ID_BASE - paletteId;

      if ( usermodIndex < usermodPalettes.size() ) {
        return (int)( FIXED_PALETTE_COUNT + customPalettes.size() + usermodIndex );
      }

      return -1;
    }

    if ( paletteId >= FIXED_PALETTE_COUNT && paletteId <= WLED_CUSTOM_PALETTE_ID_BASE ) {
      size_t customIndex = WLED_CUSTOM_PALETTE_ID_BASE - paletteId;

      if ( customIndex < customPalettes.size() ) {
        return (int)( FIXED_PALETTE_COUNT + customIndex );
      }

      return -1;
    }

    return -1;
  }

  void getPaletteName( uint8_t paletteId, char* paletteName, size_t paletteNameSize ) {
    if ( paletteName == nullptr || paletteNameSize == 0 ) {
      return;
    }

    paletteName[0] = '\0';

    extractModeName( paletteId, JSON_palette_names, paletteName, paletteNameSize - 1 );

    if ( strlen(paletteName) == 0 ) {
      snprintf( paletteName, paletteNameSize, "Palette %u", paletteId );
    }
  }

  uint8_t findAdjacentPreset( uint8_t startPreset, int direction, String* foundName = nullptr ) {
    if ( direction == 0 ) {
      return 0;
    }

    if ( !presetCacheReady || presetCacheCount == 0 ) {
      return 0;
    }

    int currentIndex = findPresetCacheIndex( startPreset );

    int newIndex;

    if ( currentIndex < 0 ) {
      if ( direction > 0 ) {
        newIndex = 0;
      }
      else {
        newIndex = presetCacheCount - 1;
      }
    }
    else {
      newIndex = currentIndex + ( direction > 0 ? 1 : -1 );

      if ( newIndex >= presetCacheCount ) {
        newIndex = 0;
      }

      if ( newIndex < 0 ) {
        newIndex = presetCacheCount - 1;
      }
    }

    if ( foundName != nullptr ) {
      *foundName = presetCache[ newIndex ].name;
    }

    return presetCache[ newIndex ].id;
  }

  uint8_t getPresetNavigationBaseId() {
    if ( pendingPresetId > 0 && millis() - pendingPresetRequestMs < PRESET_APPLY_PENDING_MS ) {
      return pendingPresetId;
    }

    return currentPreset;
  }

  uint8_t getHueFromColor( uint32_t color ) {
    CRGBW rgb( color );

    CHSV32 hsv;

    rgb2hsv( rgb, hsv );

    return (uint8_t)( hsv.h >> 8 );
  }

  uint8_t getSaturationFromColor( uint32_t color ) {
    CRGBW rgb( color );

    CHSV32 hsv;

    rgb2hsv( rgb, hsv );

    return hsv.s;
  }

  void syncLogicalColorFromRgb( uint32_t color ) {
    CRGBW rgb( color );

    rgb2hsv( rgb, logicalColorHsv );

    logicalHueValue = (uint8_t)( logicalColorHsv.h >> 8 );

    logicalSaturationValue = logicalColorHsv.s;

    logicalWhiteValue = rgb.w;

    logicalColorHsvValid = true;

    lastHueValue = logicalHueValue;

    lastSaturationValue = logicalSaturationValue;
  }

  uint8_t getDisplayedHue() {
    uint32_t currentColor = getPrimaryColor();

    if ( logicalColorHsvValid && lastPrimaryColorValid && currentColor == lastPrimaryColor ) {
      return logicalHueValue;
    }

    return getHueFromColor( currentColor );
  }

  uint8_t getDisplayedSaturation() {
    uint32_t currentColor = getPrimaryColor();

    if ( logicalColorHsvValid && lastPrimaryColorValid && currentColor == lastPrimaryColor ) {
      return logicalSaturationValue;
    }

    return getSaturationFromColor( currentColor );
  }

  // =========================================================
  // Common centered text / standard page header helpers
  // =========================================================

  void setCenteredTextStyle( uint16_t color, uint8_t size, uint16_t background = TFT_BLACK ) {
    display.setTextDatum( textdatum_t::middle_center );
    display.setTextColor( color, background );
    display.setTextSize( size );
  }

  void drawStandardPageHeader( const char* title, const char* subtitle, uint16_t titleColor = TFT_WHITE ) {
    setCenteredTextStyle( titleColor, 2 );
    display.drawString( title, screenWidth / 2, 18 );

    setCenteredTextStyle( TFT_WHITE, 1 );
    display.drawString( subtitle, screenWidth / 2, 41 );

    display.drawFastHLine( 8, 58, screenWidth - 16, TFT_DARKGREY );
  }

  void drawConnectingScreen() {
    drawStartupBase();

    display.fillRect( 0, 125, screenWidth, 100, TFT_BLACK );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    display.setTextSize( 2 );

    display.drawString( "Wi-Fi Reconnecting...", screenWidth / 2, 153 );

    display.setTextSize( 1 );

    display.setTextColor( TFT_DARKGREY, TFT_BLACK );

    display.drawString( "WLED is running", screenWidth / 2, 185 );

    connectingScreenShown = true;

    readyScreenShown = false;

    resetTouchGesture();
  }

  void drawPowerIcon( int16_t centerX, int16_t centerY, uint16_t iconColor, uint16_t backgroundColor ) {
    display.drawCircle( centerX, centerY + 2, 11, iconColor );

    display.drawCircle( centerX, centerY + 2, 10, iconColor );

    display.fillRect( centerX - 4, centerY - 11, 9, 8, backgroundColor );

    display.drawFastVLine( centerX - 1, centerY - 14, 12, iconColor );

    display.drawFastVLine( centerX, centerY - 14, 12, iconColor );

    display.drawFastVLine( centerX + 1, centerY - 14, 12, iconColor );
  }

  void drawPowerButton( bool ledOn, bool pressed ) {
    uint16_t stateColor = ledOn ? TFT_GREEN : TFT_RED;

    uint16_t backgroundColor = pressed ? stateColor : TFT_BLACK;

    uint16_t iconColor = pressed ? TFT_BLACK : stateColor;

    display.fillRect( POWER_BUTTON_X - 2, POWER_BUTTON_Y - 2, POWER_BUTTON_W + 4, POWER_BUTTON_H + 4, TFT_BLACK );

    display.fillRect( POWER_BUTTON_X, POWER_BUTTON_Y, POWER_BUTTON_W, POWER_BUTTON_H, backgroundColor );

    display.drawRect( POWER_BUTTON_X, POWER_BUTTON_Y, POWER_BUTTON_W, POWER_BUTTON_H, stateColor );

    display.drawRect( POWER_BUTTON_X + 1, POWER_BUTTON_Y + 1, POWER_BUTTON_W - 2, POWER_BUTTON_H - 2, stateColor );

    int16_t centerX = POWER_BUTTON_X + (POWER_BUTTON_W / 2);

    int16_t centerY = POWER_BUTTON_Y + (POWER_BUTTON_H / 2);

    drawPowerIcon( centerX, centerY, iconColor, backgroundColor );

    powerButtonVisualPressed = pressed;
  }

  void drawTriangleButton( int16_t x, int16_t y, bool pointRight, bool pressed ) {
    const uint16_t buttonColor = TFT_CYAN;

    const int16_t w = CONTROL_BUTTON_W;

    const int16_t h = CONTROL_BUTTON_H;

    display.fillRect( x - 2, y - 2, w + 4, h + 4, TFT_BLACK );

    if (pressed) {
      display.fillRect( x, y, w, h, buttonColor );
    }
    else {
      display.fillRect( x, y, w, h, TFT_BLACK );

      display.drawRect( x, y, w, h, buttonColor );

      display.drawRect( x + 1, y + 1, w - 2, h - 2, buttonColor );
    }

    int16_t centerX = x + (w / 2);

    int16_t centerY = y + (h / 2);

    uint16_t triangleColor = pressed ? TFT_BLACK : buttonColor;

    if (pointRight) {
      display.fillTriangle( centerX + 10, centerY, centerX - 7, centerY - 9, centerX - 7, centerY + 9, triangleColor );
    }
    else {
      display.fillTriangle( centerX - 10, centerY, centerX + 7, centerY - 9, centerX + 7, centerY + 9, triangleColor );
    }
  }

  // =========================================================
  // Shared numeric control drawing
  // =========================================================

  void drawNumericControl( int16_t clearY, int16_t clearH, const char* label, int16_t labelY, int16_t buttonY, int16_t valueY, const char* valueText, TouchTarget pressedTarget, TouchTarget downTarget, TouchTarget upTarget ) {
    display.fillRect( 0, clearY, screenWidth, clearH, TFT_BLACK );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    display.setTextSize( 1 );

    display.drawString( label, screenWidth / 2, labelY );

    drawTriangleButton( CONTROL_LEFT_X, buttonY, false, pressedTarget == downTarget );

    drawTriangleButton( CONTROL_RIGHT_X, buttonY, true, pressedTarget == upTarget );

    display.setTextSize( 2 );

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    display.drawString( valueText, screenWidth / 2, valueY );
  }

  void drawBrightness( int brightnessValue, TouchTarget pressedTarget = TOUCH_TARGET_NONE ) {
    char valueText[8];

    snprintf( valueText, sizeof(valueText), "%d", brightnessValue );

    drawNumericControl( 62, 58, "Brightness", 70, BRI_BUTTON_Y, 99, valueText, pressedTarget, TOUCH_TARGET_BRIGHTNESS_DOWN, TOUCH_TARGET_BRIGHTNESS_UP );
  }

  void getEffectName( uint8_t effectMode, char* effectName, size_t effectNameSize ) {
    if ( effectName == nullptr || effectNameSize == 0 ) {
      return;
    }

    effectName[0] = '\0';

    extractModeName( effectMode, nullptr, effectName, effectNameSize - 1 );

    if ( strlen(effectName) == 0 ) {
      strncpy( effectName, "Unknown", effectNameSize - 1 );

      effectName[ effectNameSize - 1 ] = '\0';
    }

    if ( strlen(effectName) > 22 ) {
      effectName[22] = '\0';
    }
  }

  void drawEffectDetailButton( uint8_t effectMode, bool pressed ) {
    const uint16_t buttonColor = TFT_CYAN;

    uint16_t backgroundColor = pressed ? buttonColor : TFT_BLACK;

    uint16_t textColor = pressed ? TFT_BLACK : TFT_WHITE;

    display.fillRect( EFFECT_DETAIL_X - 2, EFFECT_DETAIL_Y - 2, EFFECT_DETAIL_W + 4, EFFECT_DETAIL_H + 4, TFT_BLACK );

    display.fillRect( EFFECT_DETAIL_X, EFFECT_DETAIL_Y, EFFECT_DETAIL_W, EFFECT_DETAIL_H, backgroundColor );

    display.drawRect( EFFECT_DETAIL_X, EFFECT_DETAIL_Y, EFFECT_DETAIL_W, EFFECT_DETAIL_H, buttonColor );

    display.drawRect( EFFECT_DETAIL_X + 1, EFFECT_DETAIL_Y + 1, EFFECT_DETAIL_W - 2, EFFECT_DETAIL_H - 2, buttonColor );

    char effectName[64];

    getEffectName( effectMode, effectName, sizeof(effectName) );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( textColor, backgroundColor );

    if ( strlen(effectName) <= 10 ) {
      display.setTextSize( 2 );
    }
    else {
      display.setTextSize( 1 );
    }

    display.drawString( effectName, EFFECT_DETAIL_X + (EFFECT_DETAIL_W / 2), EFFECT_DETAIL_Y + (EFFECT_DETAIL_H / 2) );

    effectDetailVisualPressed = pressed;
  }

  void drawEffect( uint8_t effectMode, TouchTarget pressedTarget = TOUCH_TARGET_NONE ) {
    display.fillRect( 0, 120, screenWidth, 58, TFT_BLACK );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    display.setTextSize( 1 );

    display.drawString( "Effect", screenWidth / 2, 128 );

    drawTriangleButton( CONTROL_LEFT_X, FX_BUTTON_Y, false, pressedTarget == TOUCH_TARGET_EFFECT_PREV );

    drawEffectDetailButton( effectMode, pressedTarget == TOUCH_TARGET_EFFECT_DETAIL );

    drawTriangleButton( CONTROL_RIGHT_X, FX_BUTTON_Y, true, pressedTarget == TOUCH_TARGET_EFFECT_NEXT );
  }

  void drawColorButton( uint32_t color, bool pressed ) {
    const uint16_t buttonColor = TFT_CYAN;

    uint16_t backgroundColor = pressed ? buttonColor : TFT_BLACK;

    uint16_t textColor = pressed ? TFT_BLACK : TFT_WHITE;

    uint16_t previewColor = rgbTo565( R(color), G(color), B(color) );

    display.fillRect( COLOR_BUTTON_X - 2, MAIN_BOTTOM_BUTTON_Y - 2, COLOR_BUTTON_W + 4, MAIN_BOTTOM_BUTTON_H + 4, TFT_BLACK );

    display.fillRect( COLOR_BUTTON_X, MAIN_BOTTOM_BUTTON_Y, COLOR_BUTTON_W, MAIN_BOTTOM_BUTTON_H, backgroundColor );

    display.drawRect( COLOR_BUTTON_X, MAIN_BOTTOM_BUTTON_Y, COLOR_BUTTON_W, MAIN_BOTTOM_BUTTON_H, buttonColor );

    display.drawRect( COLOR_BUTTON_X + 1, MAIN_BOTTOM_BUTTON_Y + 1, COLOR_BUTTON_W - 2, MAIN_BOTTOM_BUTTON_H - 2, buttonColor );

    static constexpr int16_t SWATCH_X = 28;
    static constexpr int16_t SWATCH_W = 24;
    static constexpr int16_t SWATCH_H = 24;

    int16_t swatchY = MAIN_BOTTOM_BUTTON_Y + ( ( MAIN_BOTTOM_BUTTON_H - SWATCH_H ) / 2 );

    display.fillRect( SWATCH_X, swatchY, SWATCH_W, SWATCH_H, previewColor );

    display.drawRect( SWATCH_X, swatchY, SWATCH_W, SWATCH_H, TFT_WHITE );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( textColor, backgroundColor );

    display.setTextSize( 2 );

    display.drawString( "COLOR", 105, MAIN_BOTTOM_BUTTON_Y + (MAIN_BOTTOM_BUTTON_H / 2) );

    colorButtonVisualPressed = pressed;
  }

  void drawPresetOpenButton( bool pressed ) {
    const uint16_t buttonColor = TFT_CYAN;

    uint16_t backgroundColor = pressed ? buttonColor : TFT_BLACK;

    uint16_t textColor = pressed ? TFT_BLACK : TFT_WHITE;

    display.fillRect( PRESET_OPEN_BUTTON_X - 2, MAIN_BOTTOM_BUTTON_Y - 2, PRESET_OPEN_BUTTON_W + 4, MAIN_BOTTOM_BUTTON_H + 4, TFT_BLACK );

    display.fillRect( PRESET_OPEN_BUTTON_X, MAIN_BOTTOM_BUTTON_Y, PRESET_OPEN_BUTTON_W, MAIN_BOTTOM_BUTTON_H, backgroundColor );

    display.drawRect( PRESET_OPEN_BUTTON_X, MAIN_BOTTOM_BUTTON_Y, PRESET_OPEN_BUTTON_W, MAIN_BOTTOM_BUTTON_H, buttonColor );

    display.drawRect( PRESET_OPEN_BUTTON_X + 1, MAIN_BOTTOM_BUTTON_Y + 1, PRESET_OPEN_BUTTON_W - 2, MAIN_BOTTOM_BUTTON_H - 2, buttonColor );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( textColor, backgroundColor );

    display.setTextSize( 2 );

    display.drawString( "PRESET", PRESET_OPEN_BUTTON_X + (PRESET_OPEN_BUTTON_W / 2), MAIN_BOTTOM_BUTTON_Y + (MAIN_BOTTOM_BUTTON_H / 2) );

    presetOpenButtonVisualPressed = pressed;
  }

  void drawBackButton( bool pressed ) {
    const uint16_t buttonColor = TFT_CYAN;

    uint16_t backgroundColor = pressed ? buttonColor : TFT_BLACK;

    uint16_t iconColor = pressed ? TFT_BLACK : buttonColor;

    display.fillRect( BACK_BUTTON_X - 2, BACK_BUTTON_Y - 2, BACK_BUTTON_W + 4, BACK_BUTTON_H + 4, TFT_BLACK );

    display.fillRect( BACK_BUTTON_X, BACK_BUTTON_Y, BACK_BUTTON_W, BACK_BUTTON_H, backgroundColor );

    display.drawRect( BACK_BUTTON_X, BACK_BUTTON_Y, BACK_BUTTON_W, BACK_BUTTON_H, buttonColor );

    display.drawRect( BACK_BUTTON_X + 1, BACK_BUTTON_Y + 1, BACK_BUTTON_W - 2, BACK_BUTTON_H - 2, buttonColor );

    int16_t centerX = BACK_BUTTON_X + (BACK_BUTTON_W / 2);

    int16_t centerY = BACK_BUTTON_Y + (BACK_BUTTON_H / 2);

    display.fillTriangle( centerX - 11, centerY, centerX - 1, centerY - 9, centerX - 1, centerY + 9, iconColor );

    display.fillRect( centerX - 1, centerY - 2, 13, 5, iconColor );

    backButtonVisualPressed = pressed;
  }

  void drawColorDetails( uint32_t color ) {
    display.fillRect( 0, 60, screenWidth, 78, TFT_BLACK );

    uint16_t previewColor = rgbTo565( R(color), G(color), B(color) );

    display.fillRect( COLOR_PREVIEW_X, COLOR_PREVIEW_Y, COLOR_PREVIEW_W, COLOR_PREVIEW_H, previewColor );

    display.drawRect( COLOR_PREVIEW_X, COLOR_PREVIEW_Y, COLOR_PREVIEW_W, COLOR_PREVIEW_H, TFT_WHITE );

    display.drawRect( COLOR_PREVIEW_X + 1, COLOR_PREVIEW_Y + 1, COLOR_PREVIEW_W - 2, COLOR_PREVIEW_H - 2, TFT_DARKGREY );

    char hexText[16];

    snprintf( hexText, sizeof(hexText), "#%02X%02X%02X", R(color), G(color), B(color) );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    display.setTextSize( 2 );

    display.drawString( hexText, screenWidth / 2, 121 );

    display.drawFastHLine( 32, 136, screenWidth - 64, TFT_DARKGREY );
  }

  void drawHue( uint8_t hueValue, TouchTarget pressedTarget = TOUCH_TARGET_NONE ) {
    char valueText[8];

    snprintf( valueText, sizeof(valueText), "%u", hueValue );

    drawNumericControl( 138, 56, "Hue", 144, HUE_BUTTON_Y, HUE_BUTTON_Y + (CONTROL_BUTTON_H / 2), valueText, pressedTarget, TOUCH_TARGET_HUE_DOWN, TOUCH_TARGET_HUE_UP );
  }

  void drawSaturation( uint8_t saturationValue, TouchTarget pressedTarget = TOUCH_TARGET_NONE ) {
    char valueText[8];

    snprintf( valueText, sizeof(valueText), "%u", saturationValue );

    drawNumericControl( 194, 46, "Saturation", SATURATION_LABEL_Y, SATURATION_BUTTON_Y, SATURATION_BUTTON_Y + (CONTROL_BUTTON_H / 2), valueText, pressedTarget, TOUCH_TARGET_SATURATION_DOWN, TOUCH_TARGET_SATURATION_UP );
  }

  void drawSpeed( uint8_t speedValue, TouchTarget pressedTarget = TOUCH_TARGET_NONE ) {
    char valueText[8];

    snprintf( valueText, sizeof(valueText), "%u", speedValue );

    drawNumericControl( 62, 58, "Speed", 70, SPEED_BUTTON_Y, 99, valueText, pressedTarget, TOUCH_TARGET_SPEED_DOWN, TOUCH_TARGET_SPEED_UP );
  }

  void drawIntensity( uint8_t intensityValue, TouchTarget pressedTarget = TOUCH_TARGET_NONE ) {
    char valueText[8];

    snprintf( valueText, sizeof(valueText), "%u", intensityValue );

    drawNumericControl( 120, 60, "Intensity", 128, INTENSITY_BUTTON_Y, 157, valueText, pressedTarget, TOUCH_TARGET_INTENSITY_DOWN, TOUCH_TARGET_INTENSITY_UP );
  }

  void drawPalette( uint8_t paletteId, TouchTarget pressedTarget = TOUCH_TARGET_NONE ) {
    display.fillRect( 0, 180, screenWidth, 60, TFT_BLACK );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    display.setTextSize( 1 );

    display.drawString( "Palette", screenWidth / 2, PALETTE_LABEL_Y );

    drawTriangleButton( CONTROL_LEFT_X, PALETTE_BUTTON_Y, false, pressedTarget == TOUCH_TARGET_PALETTE_PREV );

    drawTriangleButton( CONTROL_RIGHT_X, PALETTE_BUTTON_Y, true, pressedTarget == TOUCH_TARGET_PALETTE_NEXT );

    char paletteName[64];

    getPaletteName( paletteId, paletteName, sizeof(paletteName) );

    if ( strlen(paletteName) > 22 ) {
      paletteName[22] = '\0';
    }

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    if ( strlen(paletteName) <= 10 ) {
      display.setTextSize( 2 );
    }
    else {
      display.setTextSize( 1 );
    }

    display.drawString( paletteName, screenWidth / 2, PALETTE_BUTTON_Y + (CONTROL_BUTTON_H / 2) );
  }

  void drawEffectPageName( uint8_t effectMode ) {
    display.fillRect( 56, 32, 208, 22, TFT_BLACK );

    char effectName[64];

    getEffectName( effectMode, effectName, sizeof(effectName) );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    display.setTextSize( 1 );

    display.drawString( effectName, screenWidth / 2, 42 );
  }

  String getPresetDisplayName( uint8_t presetId ) {
    if ( presetId == 0 ) {
      return "Custom State";
    }

    if ( pendingPresetId == presetId && pendingPresetName.length() > 0 ) {
      return pendingPresetName;
    }

    String name;

    if ( getCachedPresetName( presetId, name ) ) {
      return name;
    }

    char fallback[24];

    snprintf( fallback, sizeof(fallback), "Preset %u", presetId );

    return String(fallback);
  }

  void drawPresetDetails( uint8_t presetId, bool applying = false ) {
    display.fillRect( 0, 60, screenWidth, 118, TFT_BLACK );

    display.setTextDatum( textdatum_t::middle_center );

    // -------------------------------------------------------
    // Internal Preset ID scan number is intentionally hidden.
    // -------------------------------------------------------

    if ( !presetCacheReady ) {
      display.setTextColor( TFT_YELLOW, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( "Loading Presets", screenWidth / 2, PRESET_NAME_Y );

      display.setTextColor( TFT_WHITE, TFT_BLACK );

      display.setTextSize( 1 );

      display.drawString( "Updating Preset List", screenWidth / 2, PRESET_ID_Y );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );

      display.drawString( "WLED remains active", screenWidth / 2, PRESET_STATUS_Y );

      return;
    }

    if ( presetNoEntries ) {
      display.setTextColor( TFT_RED, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( "No Presets", screenWidth / 2, PRESET_NAME_Y );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );

      display.setTextSize( 1 );

      display.drawString( "No saved preset found", screenWidth / 2, PRESET_ID_Y );

      display.drawString( "Use MANAGE to create", screenWidth / 2, PRESET_STATUS_Y );

      return;
    }

    if ( presetId == 0 ) {
      display.setTextColor( TFT_WHITE, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( "Custom State", screenWidth / 2, PRESET_NAME_Y );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );

      display.setTextSize( 1 );

      display.drawString( "No active preset", screenWidth / 2, PRESET_ID_Y );

      display.drawString( "Use arrows to apply", screenWidth / 2, PRESET_STATUS_Y );

      return;
    }

    String presetName = getPresetDisplayName( presetId );

    if ( presetName.length() > 28 ) {
      presetName = presetName.substring( 0, 25 ) + "...";
    }

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    if ( presetName.length() <= 12 ) {
      display.setTextSize( 2 );
    }
    else {
      display.setTextSize( 1 );
    }

    display.drawString( presetName, screenWidth / 2, PRESET_NAME_Y );

    char idText[24];

    snprintf( idText, sizeof(idText), "Preset ID: %u", presetId );

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    display.setTextSize( 1 );

    display.drawString( idText, screenWidth / 2, PRESET_ID_Y );

    if (applying) {
      display.setTextColor( TFT_YELLOW, TFT_BLACK );

      display.drawString( "Applying...", screenWidth / 2, PRESET_STATUS_Y );
    }
    else if ( currentPreset == presetId ) {
      display.setTextColor( TFT_GREEN, TFT_BLACK );

      display.drawString( "Active Preset", screenWidth / 2, PRESET_STATUS_Y );
    }
    else {
      display.setTextColor( TFT_DARKGREY, TFT_BLACK );

      display.drawString( "Saved WLED Preset", screenWidth / 2, PRESET_STATUS_Y );
    }
  }

  void drawPresetTextButton( int16_t x, int16_t y, int16_t w, int16_t h, const char* label, bool enabled, bool pressed, uint8_t textSize ) {
    uint16_t buttonColor = enabled ? TFT_CYAN : TFT_DARKGREY;

    uint16_t backgroundColor = ( enabled && pressed ) ? buttonColor : TFT_BLACK;

    uint16_t textColor = ( enabled && pressed ) ? TFT_BLACK : ( enabled ? TFT_WHITE : TFT_DARKGREY );

    display.fillRect( x - 2, y - 2, w + 4, h + 4, TFT_BLACK );

    display.fillRect( x, y, w, h, backgroundColor );

    display.drawRect( x, y, w, h, buttonColor );

    display.drawRect( x + 1, y + 1, w - 2, h - 2, buttonColor );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( textColor, backgroundColor );

    display.setTextSize( textSize );

    display.drawString( label, x + (w / 2), y + (h / 2) );
  }

  void drawPresetDangerTextButton( int16_t x, int16_t y, int16_t w, int16_t h, const char* label, bool enabled, bool pressed, uint8_t textSize ) {
    uint16_t buttonColor = enabled ? TFT_RED : TFT_DARKGREY;

    uint16_t backgroundColor = ( enabled && pressed ) ? buttonColor : TFT_BLACK;

    uint16_t textColor = ( enabled && pressed ) ? TFT_BLACK : ( enabled ? TFT_RED : TFT_DARKGREY );

    display.fillRect( x - 2, y - 2, w + 4, h + 4, TFT_BLACK );

    display.fillRect( x, y, w, h, backgroundColor );

    display.drawRect( x, y, w, h, buttonColor );

    display.drawRect( x + 1, y + 1, w - 2, h - 2, buttonColor );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( textColor, backgroundColor );

    display.setTextSize( textSize );

    display.drawString( label, x + (w / 2), y + (h / 2) );
  }

  void drawPresetManageButton( bool pressed ) {
    bool enabled = ( presetCacheReady && !presetCacheBuilding && pendingPresetId == 0 );

    const char* label = presetCacheReady ? "MANAGE" : "LOADING";

    drawPresetTextButton( PRESET_MANAGE_BUTTON_X, PRESET_MANAGE_BUTTON_Y, PRESET_MANAGE_BUTTON_W, PRESET_MANAGE_BUTTON_H, label, enabled, pressed && enabled, 1 );

    presetManageButtonVisualPressed = ( pressed && enabled );
  }

  void drawPresetNavigation( TouchTarget pressedTarget = TOUCH_TARGET_NONE ) {
    display.fillRect( 0, 178, screenWidth, 62, TFT_BLACK );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    display.setTextSize( 1 );

    display.drawString( "Preset", screenWidth / 2, PRESET_NAV_LABEL_Y );

    drawTriangleButton( CONTROL_LEFT_X, PRESET_NAV_BUTTON_Y, false, pressedTarget == TOUCH_TARGET_PRESET_PREV );

    drawTriangleButton( CONTROL_RIGHT_X, PRESET_NAV_BUTTON_Y, true, pressedTarget == TOUCH_TARGET_PRESET_NEXT );

    drawPresetManageButton( pressedTarget == TOUCH_TARGET_PRESET_MANAGE );
  }

  void drawMainScreen( const String& ipAddress ) {
    display.fillScreen( TFT_BLACK );

    currentPage = SCREEN_MAIN;

    readyScreenShown = true;

    connectingScreenShown = false;

    resetTouchGesture();

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    display.setTextSize( 2 );

    display.drawString( "WLED M5Stack CoreS3", HEADER_CENTER_X, HEADER_TITLE_Y );

    display.setTextSize( 1 );

    display.drawString( ipAddress, HEADER_CENTER_X, HEADER_IP_Y );

    display.drawFastHLine( 8, 58, screenWidth - 16, TFT_DARKGREY );

    drawPowerButton( bri > 0, false );

    drawBrightness( bri, TOUCH_TARGET_NONE );

    uint8_t effectMode = getCurrentEffectMode();

    drawEffect( effectMode, TOUCH_TARGET_NONE );

    uint32_t primaryColor = getPrimaryColor();

    drawColorButton( primaryColor, false );

    drawPresetOpenButton( false );

    syncLogicalColorFromRgb( primaryColor );

    lastLedState = bri > 0 ? 1 : 0;

    lastBrightnessValue = bri;

    lastEffectMode = effectMode;

    lastSpeedValue = getCurrentSpeed();

    lastIntensityValue = getCurrentIntensity();

    lastPaletteValue = getCurrentPalette();

    lastPresetValue = currentPreset;

    lastPrimaryColor = primaryColor;

    lastPrimaryColorValid = true;
  }

  void drawColorScreen() {
    display.fillScreen( TFT_BLACK );

    currentPage = SCREEN_COLOR;

    readyScreenShown = true;

    connectingScreenShown = false;

    resetTouchGesture();

    drawStandardPageHeader( "COLOR", "Primary Color" );

    drawPowerButton( bri > 0, false );

    drawBackButton( false );

    uint32_t primaryColor = getPrimaryColor();

    syncLogicalColorFromRgb( primaryColor );

    drawColorDetails( primaryColor );

    drawHue( logicalHueValue, TOUCH_TARGET_NONE );

    drawSaturation( logicalSaturationValue, TOUCH_TARGET_NONE );

    lastLedState = bri > 0 ? 1 : 0;

    lastPrimaryColor = primaryColor;

    lastPrimaryColorValid = true;

    lastHueValue = logicalHueValue;

    lastSaturationValue = logicalSaturationValue;
  }

  void drawEffectDetailScreen() {
    display.fillScreen( TFT_BLACK );

    currentPage = SCREEN_EFFECT;

    readyScreenShown = true;

    connectingScreenShown = false;

    resetTouchGesture();

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    display.setTextSize( 2 );

    display.drawString( "EFFECT", screenWidth / 2, 18 );

    uint8_t effectMode = getCurrentEffectMode();

    drawEffectPageName( effectMode );

    display.drawFastHLine( 8, 58, screenWidth - 16, TFT_DARKGREY );

    drawPowerButton( bri > 0, false );

    drawBackButton( false );

    uint8_t speedValue = getCurrentSpeed();

    uint8_t intensityValue = getCurrentIntensity();

    uint8_t paletteValue = getCurrentPalette();

    drawSpeed( speedValue, TOUCH_TARGET_NONE );

    drawIntensity( intensityValue, TOUCH_TARGET_NONE );

    drawPalette( paletteValue, TOUCH_TARGET_NONE );

    lastLedState = bri > 0 ? 1 : 0;

    lastEffectMode = effectMode;

    lastSpeedValue = speedValue;

    lastIntensityValue = intensityValue;

    lastPaletteValue = paletteValue;
  }

  void drawPresetScreen() {
    display.fillScreen( TFT_BLACK );

    currentPage = SCREEN_PRESET;

    presetSubPage = PRESET_SUBPAGE_NAV;

    presetSaveOperationState = PRESET_SAVE_OP_IDLE;

    presetSaveOperationIsOverwrite = false;

    presetSaveCandidateId = 0;

    presetSaveCandidateName = "";

    presetOverwriteTargetId = 0;

    presetOverwriteTargetName = "";

    presetDeleteOperationState = PRESET_DELETE_OP_IDLE;

    presetDeleteTargetId = 0;

    presetDeleteTargetName = "";

    presetDeleteWasCurrentPreset = false;
    presetDeleteWasBootPreset = false;

    presetDeleteResultStartMs = 0;

    presetBootOperationState = PRESET_BOOT_OP_IDLE;
    presetBootTargetId = 0;
    presetBootTargetName = "NONE";
    presetBootResultStartMs = 0;

    presetSaveHoldStartTime = 0;

    presetSaveHoldTriggered = false;

    presetSaveResultStartMs = 0;

    readyScreenShown = true;

    connectingScreenShown = false;

    resetTouchGesture();

    drawStandardPageHeader( "PRESET", "Saved WLED Preset" );

    drawPowerButton( bri > 0, false );

    drawBackButton( false );

    uint8_t presetId = getDisplayedPresetId();

    bool applying = ( pendingPresetId > 0 );

    drawPresetDetails( presetId, applying );

    drawPresetNavigation( TOUCH_TARGET_NONE );

    lastLedState = bri > 0 ? 1 : 0;

    lastPresetValue = presetId;
    lastBootPresetValue = bootPreset;

    lastPresetsModifiedTime = presetsModifiedTime;
  }

  void drawPresetSaveNewButton( bool pressed ) {
    uint8_t freePresetId = findFirstFreePresetId();

    bool enabled = ( presetCacheReady && !presetCacheBuilding && freePresetId > 0 && pendingPresetId == 0 && !presetNeedsSaving() );

    drawPresetTextButton( PRESET_SAVE_NEW_BUTTON_X, PRESET_SAVE_NEW_BUTTON_Y, PRESET_SAVE_NEW_BUTTON_W, PRESET_SAVE_NEW_BUTTON_H, "SAVE NEW", enabled, pressed && enabled, 2 );

    presetSaveNewButtonVisualPressed = ( pressed && enabled );
  }

  void drawPresetOverwriteOpenButton( bool pressed ) {
    bool enabled = ( presetCacheReady && !presetCacheBuilding && presetCacheCount > 0 && pendingPresetId == 0 && !presetNeedsSaving() );

    drawPresetTextButton( PRESET_OVERWRITE_BUTTON_X, PRESET_OVERWRITE_BUTTON_Y, PRESET_OVERWRITE_BUTTON_W, PRESET_OVERWRITE_BUTTON_H, "OVERWRITE", enabled, pressed && enabled, 2 );

    presetOverwriteOpenButtonVisualPressed = ( pressed && enabled );
  }

  void drawPresetDeleteOpenButton( bool pressed ) {
    bool enabled = ( presetCacheReady && !presetCacheBuilding && presetCacheCount > 0 && pendingPresetId == 0 && !presetNeedsSaving() );

    drawPresetDangerTextButton( PRESET_DELETE_BUTTON_X, PRESET_DELETE_BUTTON_Y, PRESET_DELETE_BUTTON_W, PRESET_DELETE_BUTTON_H, "DELETE", enabled, pressed && enabled, 2 );

    presetDeleteOpenButtonVisualPressed = ( pressed && enabled );
  }

  void drawPresetBootOpenButton( bool pressed ) {
    bool enabled = ( presetCacheReady && !presetCacheBuilding && pendingPresetId == 0 && !presetNeedsSaving() );

    drawPresetTextButton( PRESET_BOOT_BUTTON_X, PRESET_BOOT_BUTTON_Y, PRESET_BOOT_BUTTON_W, PRESET_BOOT_BUTTON_H, "BOOT PRESET", enabled, pressed && enabled, 2 );

    presetBootOpenButtonVisualPressed = ( pressed && enabled );
  }

  void drawPresetSaveHoldButton( bool pressed ) {
    bool enabled = ( presetSaveOperationState == PRESET_SAVE_OP_IDLE && presetCacheReady && !presetCacheBuilding && presetSaveCandidateId > 0 && findPresetCacheIndex( presetSaveCandidateId ) < 0 && pendingPresetId == 0 && !presetNeedsSaving() );

    drawPresetTextButton( PRESET_SAVE_HOLD_BUTTON_X, PRESET_SAVE_HOLD_BUTTON_Y, PRESET_SAVE_HOLD_BUTTON_W, PRESET_SAVE_HOLD_BUTTON_H, "HOLD TO SAVE", enabled, pressed && enabled, 2 );

    presetSaveHoldButtonVisualPressed = ( pressed && enabled );
  }

  void drawPresetManageScreen() {
    display.fillScreen( TFT_BLACK );

    currentPage = SCREEN_PRESET;

    presetSubPage = PRESET_SUBPAGE_MANAGE;

    presetSaveOperationState = PRESET_SAVE_OP_IDLE;

    presetSaveOperationIsOverwrite = false;

    presetSaveCandidateId = 0;

    presetSaveCandidateName = "";

    presetOverwriteTargetId = 0;

    presetOverwriteTargetName = "";

    presetDeleteOperationState = PRESET_DELETE_OP_IDLE;

    presetDeleteTargetId = 0;

    presetDeleteTargetName = "";

    presetDeleteWasCurrentPreset = false;
    presetDeleteWasBootPreset = false;

    presetDeleteResultStartMs = 0;

    presetBootOperationState = PRESET_BOOT_OP_IDLE;
    presetBootTargetId = 0;
    presetBootTargetName = "NONE";
    presetBootResultStartMs = 0;

    presetSaveHoldStartTime = 0;

    presetSaveHoldTriggered = false;

    presetSaveResultStartMs = 0;

    readyScreenShown = true;

    connectingScreenShown = false;

    resetTouchGesture();

    uint8_t freePresetId = findFirstFreePresetId();
    char manageSubtitle[32];

    if ( !presetCacheReady || presetCacheBuilding ) {
      strlcpy( manageSubtitle, "Preset cache loading...", sizeof(manageSubtitle) );
    }
    else if ( freePresetId == 0 ) {
      strlcpy( manageSubtitle, "No free Preset ID", sizeof(manageSubtitle) );
    }
    else {
      snprintf( manageSubtitle, sizeof(manageSubtitle), "Next new Preset ID: %u", freePresetId );
    }

    drawStandardPageHeader( "PRESET MANAGE", manageSubtitle );

    drawPowerButton( bri > 0, false );

    drawBackButton( false );

    drawPresetSaveNewButton( false );

    drawPresetOverwriteOpenButton( false );

    drawPresetDeleteOpenButton( false );

    drawPresetBootOpenButton( false );

    lastLedState = bri > 0 ? 1 : 0;
  }

  // =========================================================
  // SAVE NEW / OVERWRITE operation status
  //
  // Internal 1..250 cache scan position is intentionally
  // hidden from the user.
  // =========================================================

  void drawPresetSaveOperationStatus() {
    if ( currentPage != SCREEN_PRESET || ( presetSubPage != PRESET_SUBPAGE_SAVE && presetSubPage != PRESET_SUBPAGE_OVERWRITE ) ) {
      return;
    }

    display.fillRect( 0, 60, screenWidth, 180, TFT_BLACK );

    display.setTextDatum( textdatum_t::middle_center );

    if ( presetSaveOperationState == PRESET_SAVE_OP_WAIT_WLED ) {
      display.setTextColor( TFT_YELLOW, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( presetSaveOperationIsOverwrite ? "Overwriting..." : "Saving...", screenWidth / 2, 110 );

      display.setTextColor( TFT_WHITE, TFT_BLACK );

      display.setTextSize( 1 );

      display.drawString( presetSaveCandidateName, screenWidth / 2, 142 );

      char idText[24];

      snprintf( idText, sizeof(idText), "Preset ID: %u", presetSaveCandidateId );

      display.drawString( idText, screenWidth / 2, 163 );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );

      display.drawString( "Writing WLED Preset", screenWidth / 2, 188 );

      return;
    }

    if ( presetSaveOperationState == PRESET_SAVE_OP_WAIT_CACHE ) {
      display.setTextColor( TFT_YELLOW, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( "Updating Preset List", screenWidth / 2, 110 );

      display.setTextColor( TFT_WHITE, TFT_BLACK );

      display.setTextSize( 1 );

      display.drawString( presetSaveOperationIsOverwrite ? "Verifying Overwrite..." : "Verifying New Preset...", screenWidth / 2, 158 );

      return;
    }

    if ( presetSaveOperationState == PRESET_SAVE_OP_SUCCESS ) {
      display.setTextColor( TFT_GREEN, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( presetSaveOperationIsOverwrite ? "OVERWRITTEN" : "SAVED", screenWidth / 2, 104 );

      display.setTextColor( TFT_WHITE, TFT_BLACK );

      display.setTextSize( 1 );

      display.drawString( presetSaveCandidateName, screenWidth / 2, 140 );

      char idText[24];

      snprintf( idText, sizeof(idText), "Preset ID: %u", presetSaveCandidateId );

      display.drawString( idText, screenWidth / 2, 165 );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );

      display.drawString( "Preset cache verified", screenWidth / 2, 192 );

      return;
    }

    if ( presetSaveOperationState == PRESET_SAVE_OP_FAILED ) {
      display.setTextColor( TFT_RED, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( presetSaveOperationIsOverwrite ? "OVERWRITE FAILED" : "SAVE FAILED", screenWidth / 2, 108 );

      display.setTextColor( TFT_WHITE, TFT_BLACK );

      display.setTextSize( 1 );

      display.drawString( "Preset was not verified", screenWidth / 2, 150 );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );

      display.drawString( "Returning to PRESET MANAGE", screenWidth / 2, 180 );
    }
  }

  void drawPresetSaveScreen() {
    display.fillScreen( TFT_BLACK );

    currentPage = SCREEN_PRESET;

    presetSubPage = PRESET_SUBPAGE_SAVE;

    if ( presetSaveOperationState == PRESET_SAVE_OP_IDLE ) {
      presetSaveOperationIsOverwrite = false;
    }

    readyScreenShown = true;

    connectingScreenShown = false;

    resetTouchGesture();

    drawStandardPageHeader( "SAVE PRESET", "Current WLED State" );

    drawPowerButton( bri > 0, false );

    drawBackButton( false );

    if ( presetSaveOperationState != PRESET_SAVE_OP_IDLE ) {
      drawPresetSaveOperationStatus();
      return;
    }

    if ( presetSaveCandidateId == 0 ) {
      display.setTextColor( TFT_RED, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( "No Free ID", screenWidth / 2, 110 );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );

      display.setTextSize( 1 );

      display.drawString( "Preset IDs 1-250 are unavailable", screenWidth / 2, 150 );

      return;
    }

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    if ( presetSaveCandidateName.length() <= 18 ) {
      display.setTextSize( 2 );
    }
    else {
      display.setTextSize( 1 );
    }

    display.drawString( presetSaveCandidateName, screenWidth / 2, 88 );

    char idText[24];

    snprintf( idText, sizeof(idText), "Preset ID: %u", presetSaveCandidateId );

    display.setTextSize( 1 );

    display.drawString( idText, screenWidth / 2, 120 );

    display.setTextColor( TFT_DARKGREY, TFT_BLACK );

    display.drawString( "Hold 1 second to save", screenWidth / 2, 151 );

    drawPresetSaveHoldButton( false );

    lastLedState = bri > 0 ? 1 : 0;
  }

  void drawPresetOverwriteNavigation( TouchTarget pressedTarget = TOUCH_TARGET_NONE ) {
    display.fillRect( 0, 112, screenWidth, 58, TFT_BLACK );

    drawTriangleButton( CONTROL_LEFT_X, PRESET_OVERWRITE_NAV_BUTTON_Y, false, pressedTarget == TOUCH_TARGET_PRESET_OVERWRITE_PREV );

    drawTriangleButton( CONTROL_RIGHT_X, PRESET_OVERWRITE_NAV_BUTTON_Y, true, pressedTarget == TOUCH_TARGET_PRESET_OVERWRITE_NEXT );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( TFT_DARKGREY, TFT_BLACK );

    display.setTextSize( 1 );

    display.drawString( "TARGET", screenWidth / 2, PRESET_OVERWRITE_NAV_BUTTON_Y + (CONTROL_BUTTON_H / 2) );
  }

  void drawPresetOverwriteHoldButton( bool pressed ) {
    bool enabled = ( presetSaveOperationState == PRESET_SAVE_OP_IDLE && presetCacheReady && !presetCacheBuilding && presetOverwriteTargetId > 0 && findPresetCacheIndex( presetOverwriteTargetId ) >= 0 && pendingPresetId == 0 && !presetNeedsSaving() );

    drawPresetTextButton( PRESET_OVERWRITE_HOLD_BUTTON_X, PRESET_OVERWRITE_HOLD_BUTTON_Y, PRESET_OVERWRITE_HOLD_BUTTON_W, PRESET_OVERWRITE_HOLD_BUTTON_H, "HOLD TO OVERWRITE", enabled, pressed && enabled, 1 );

    presetOverwriteHoldButtonVisualPressed = ( pressed && enabled );
  }

  void drawPresetOverwriteScreen() {
    display.fillScreen( TFT_BLACK );

    currentPage = SCREEN_PRESET;

    presetSubPage = PRESET_SUBPAGE_OVERWRITE;

    readyScreenShown = true;

    connectingScreenShown = false;

    resetTouchGesture();

    drawStandardPageHeader( "OVERWRITE PRESET", "Select destination only" );

    drawPowerButton( bri > 0, false );

    drawBackButton( false );

    if ( presetSaveOperationState != PRESET_SAVE_OP_IDLE ) {
      drawPresetSaveOperationStatus();
      return;
    }

    if ( presetOverwriteTargetId == 0 || findPresetCacheIndex( presetOverwriteTargetId ) < 0 ) {
      display.setTextColor( TFT_RED, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( "No Preset", screenWidth / 2, 104 );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );

      display.setTextSize( 1 );

      display.drawString( "Nothing can be overwritten", screenWidth / 2, 145 );

      return;
    }

    String displayName = presetOverwriteTargetName;

    if ( displayName.length() > 28 ) {
      displayName = displayName.substring( 0, 25 ) + "...";
    }

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    if ( displayName.length() <= 12 ) {
      display.setTextSize( 2 );
    }
    else {
      display.setTextSize( 1 );
    }

    display.drawString( displayName, screenWidth / 2, 82 );

    char idText[24];

    snprintf( idText, sizeof(idText), "Preset ID: %u", presetOverwriteTargetId );

    display.setTextSize( 1 );

    display.drawString( idText, screenWidth / 2, 105 );

    drawPresetOverwriteNavigation( TOUCH_TARGET_NONE );

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    display.setTextSize( 1 );

    display.drawString( "Current WLED State", screenWidth / 2, 174 );

    display.setTextColor( TFT_DARKGREY, TFT_BLACK );

    display.drawString( "will replace this Preset", screenWidth / 2, 187 );

    drawPresetOverwriteHoldButton( false );

    lastLedState = bri > 0 ? 1 : 0;
  }

  void drawPresetDeleteNavigation( TouchTarget pressedTarget = TOUCH_TARGET_NONE ) {
    display.fillRect( 0, 112, screenWidth, 58, TFT_BLACK );

    drawTriangleButton( CONTROL_LEFT_X, PRESET_DELETE_NAV_BUTTON_Y, false, pressedTarget == TOUCH_TARGET_PRESET_DELETE_PREV );

    drawTriangleButton( CONTROL_RIGHT_X, PRESET_DELETE_NAV_BUTTON_Y, true, pressedTarget == TOUCH_TARGET_PRESET_DELETE_NEXT );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( TFT_RED, TFT_BLACK );

    display.setTextSize( 1 );

    display.drawString( "TARGET", screenWidth / 2, PRESET_DELETE_NAV_BUTTON_Y + (CONTROL_BUTTON_H / 2) );
  }

  void drawPresetDeleteHoldButton( bool pressed ) {
    bool enabled = ( presetDeleteOperationState == PRESET_DELETE_OP_IDLE && presetCacheReady && !presetCacheBuilding && presetDeleteTargetId > 0 && findPresetCacheIndex( presetDeleteTargetId ) >= 0 && pendingPresetId == 0 && !presetNeedsSaving() );

    drawPresetDangerTextButton( PRESET_DELETE_HOLD_BUTTON_X, PRESET_DELETE_HOLD_BUTTON_Y, PRESET_DELETE_HOLD_BUTTON_W, PRESET_DELETE_HOLD_BUTTON_H, "HOLD TO DELETE", enabled, pressed && enabled, 1 );

    presetDeleteHoldButtonVisualPressed = ( pressed && enabled );
  }

  // =========================================================
  // DELETE operation status
  //
  // Internal cache scan position is not displayed.
  // =========================================================

  void drawPresetDeleteOperationStatus() {
    if ( currentPage != SCREEN_PRESET || presetSubPage != PRESET_SUBPAGE_DELETE ) {
      return;
    }

    display.fillRect( 0, 60, screenWidth, 180, TFT_BLACK );

    display.setTextDatum( textdatum_t::middle_center );

    if ( presetDeleteOperationState == PRESET_DELETE_OP_WAIT_CACHE ) {
      display.setTextColor( TFT_YELLOW, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( "Updating Preset List", screenWidth / 2, 105 );

      display.setTextColor( TFT_WHITE, TFT_BLACK );

      display.setTextSize( 1 );

      display.drawString( presetDeleteTargetName, screenWidth / 2, 145 );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );

      display.drawString( "Verifying Deletion...", screenWidth / 2, 175 );

      return;
    }

    if ( presetDeleteOperationState == PRESET_DELETE_OP_SUCCESS ) {
      display.setTextColor( TFT_GREEN, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( "DELETED", screenWidth / 2, 104 );

      display.setTextColor( TFT_WHITE, TFT_BLACK );

      display.setTextSize( 1 );

      display.drawString( presetDeleteTargetName, screenWidth / 2, 140 );

      char idText[24];

      snprintf( idText, sizeof(idText), "Preset ID: %u", presetDeleteTargetId );

      display.drawString( idText, screenWidth / 2, 165 );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );

      display.drawString( "Preset cache verified", screenWidth / 2, 192 );

      return;
    }

    if ( presetDeleteOperationState == PRESET_DELETE_OP_FAILED ) {
      display.setTextColor( TFT_RED, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( "DELETE FAILED", screenWidth / 2, 108 );

      display.setTextColor( TFT_WHITE, TFT_BLACK );

      display.setTextSize( 1 );

      display.drawString( "Preset still exists", screenWidth / 2, 150 );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );

      display.drawString( "Returning to PRESET MANAGE", screenWidth / 2, 180 );
    }
  }

  void drawPresetDeleteScreen() {
    display.fillScreen( TFT_BLACK );

    currentPage = SCREEN_PRESET;

    presetSubPage = PRESET_SUBPAGE_DELETE;

    readyScreenShown = true;

    connectingScreenShown = false;

    resetTouchGesture();

    drawStandardPageHeader( "DELETE PRESET", "Select target only", TFT_RED );

    drawPowerButton( bri > 0, false );

    drawBackButton( false );

    if ( presetDeleteOperationState != PRESET_DELETE_OP_IDLE ) {
      drawPresetDeleteOperationStatus();
      return;
    }

    if ( presetDeleteTargetId == 0 || findPresetCacheIndex( presetDeleteTargetId ) < 0 ) {
      display.setTextColor( TFT_RED, TFT_BLACK );

      display.setTextSize( 2 );

      display.drawString( "No Preset", screenWidth / 2, 104 );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );

      display.setTextSize( 1 );

      display.drawString( "Nothing can be deleted", screenWidth / 2, 145 );

      return;
    }

    String displayName = presetDeleteTargetName;

    if ( displayName.length() > 28 ) {
      displayName = displayName.substring( 0, 25 ) + "...";
    }

    display.setTextColor( TFT_WHITE, TFT_BLACK );

    if ( displayName.length() <= 12 ) {
      display.setTextSize( 2 );
    }
    else {
      display.setTextSize( 1 );
    }

    display.drawString( displayName, screenWidth / 2, 82 );

    char idText[24];

    snprintf( idText, sizeof(idText), "Preset ID: %u", presetDeleteTargetId );

    display.setTextSize( 1 );

    display.drawString( idText, screenWidth / 2, 105 );

    drawPresetDeleteNavigation( TOUCH_TARGET_NONE );

    display.setTextColor( TFT_RED, TFT_BLACK );

    display.setTextSize( 1 );

    display.drawString( "Delete this Preset", screenWidth / 2, 174 );

    display.setTextColor( TFT_DARKGREY, TFT_BLACK );

    display.drawString( "Current LED state is kept", screenWidth / 2, 187 );

    drawPresetDeleteHoldButton( false );

    lastLedState = bri > 0 ? 1 : 0;
  }

  void drawPresetBootNavigation( TouchTarget pressedTarget = TOUCH_TARGET_NONE ) {
    display.fillRect( 0, 112, screenWidth, 58, TFT_BLACK );

    drawTriangleButton( CONTROL_LEFT_X, PRESET_BOOT_NAV_BUTTON_Y, false, pressedTarget == TOUCH_TARGET_PRESET_BOOT_PREV );

    drawTriangleButton( CONTROL_RIGHT_X, PRESET_BOOT_NAV_BUTTON_Y, true, pressedTarget == TOUCH_TARGET_PRESET_BOOT_NEXT );

    display.setTextDatum( textdatum_t::middle_center );

    display.setTextColor( TFT_CYAN, TFT_BLACK );

    display.setTextSize( 1 );

    display.drawString( "BOOT TARGET", screenWidth / 2, PRESET_BOOT_NAV_BUTTON_Y + (CONTROL_BUTTON_H / 2) );
  }

  bool isPresetBootTargetValid() {
    return presetBootTargetId == 0 || findPresetCacheIndex( presetBootTargetId ) >= 0;
  }

  bool isPresetBootTargetCurrent() {
    return isPresetBootTargetValid() && presetBootTargetId == bootPreset;
  }

  void drawPresetBootHoldButton( bool pressed ) {
    bool enabled = ( presetBootOperationState == PRESET_BOOT_OP_IDLE && presetCacheReady && !presetCacheBuilding && pendingPresetId == 0 && !presetNeedsSaving() && isPresetBootTargetValid() && !isPresetBootTargetCurrent() );

    const char* label;

    if ( isPresetBootTargetCurrent() ) {
      label = ( presetBootTargetId == 0 ) ? "BOOT ALREADY NONE" : "CURRENT BOOT PRESET";
    }
    else {
      label = ( presetBootTargetId == 0 ) ? "HOLD TO CLEAR BOOT" : "HOLD TO SET BOOT";
    }

    drawPresetTextButton( PRESET_BOOT_HOLD_BUTTON_X, PRESET_BOOT_HOLD_BUTTON_Y, PRESET_BOOT_HOLD_BUTTON_W, PRESET_BOOT_HOLD_BUTTON_H, label, enabled, pressed && enabled, 1 );

    presetBootHoldButtonVisualPressed = ( pressed && enabled );
  }

  void drawPresetBootOperationStatus() {
    if ( currentPage != SCREEN_PRESET || presetSubPage != PRESET_SUBPAGE_BOOT ) {
      return;
    }

    display.fillRect( 0, 60, screenWidth, 180, TFT_BLACK );

    display.setTextDatum( textdatum_t::middle_center );

    if ( presetBootOperationState == PRESET_BOOT_OP_WAIT_CONFIG ) {
      display.setTextColor( TFT_YELLOW, TFT_BLACK );
      display.setTextSize( 2 );
      display.drawString( "Saving Boot Setting", screenWidth / 2, 105 );

      display.setTextColor( TFT_WHITE, TFT_BLACK );
      display.setTextSize( 1 );
      display.drawString( presetBootTargetId == 0 ? "NONE" : presetBootTargetName, screenWidth / 2, 145 );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );
      display.drawString( "Writing WLED Config...", screenWidth / 2, 175 );

      return;
    }

    if ( presetBootOperationState == PRESET_BOOT_OP_SUCCESS ) {
      display.setTextColor( TFT_GREEN, TFT_BLACK );
      display.setTextSize( 2 );
      display.drawString( presetBootTargetId == 0 ? "BOOT CLEARED" : "BOOT PRESET SET", screenWidth / 2, 104 );

      display.setTextColor( TFT_WHITE, TFT_BLACK );
      display.setTextSize( 1 );
      display.drawString( presetBootTargetId == 0 ? "NONE" : presetBootTargetName, screenWidth / 2, 140 );

      if ( presetBootTargetId > 0 ) {
        char idText[24];
        snprintf( idText, sizeof(idText), "Preset ID: %u", presetBootTargetId );
        display.drawString( idText, screenWidth / 2, 165 );
      }
      else {
        display.drawString( "No startup Preset", screenWidth / 2, 165 );
      }

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );
      display.drawString( "WLED config saved", screenWidth / 2, 192 );

      return;
    }

    if ( presetBootOperationState == PRESET_BOOT_OP_FAILED ) {
      display.setTextColor( TFT_RED, TFT_BLACK );
      display.setTextSize( 2 );
      display.drawString( "BOOT SET FAILED", screenWidth / 2, 108 );

      display.setTextColor( TFT_WHITE, TFT_BLACK );
      display.setTextSize( 1 );
      display.drawString( "Setting was not verified", screenWidth / 2, 150 );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );
      display.drawString( "Returning to PRESET MANAGE", screenWidth / 2, 180 );
    }
  }

  void drawPresetBootScreen() {
    display.fillScreen( TFT_BLACK );

    currentPage = SCREEN_PRESET;
    presetSubPage = PRESET_SUBPAGE_BOOT;
    readyScreenShown = true;
    connectingScreenShown = false;

    resetTouchGesture();

    drawStandardPageHeader( "BOOT PRESET", "Startup Preset" );

    drawPowerButton( bri > 0, false );
    drawBackButton( false );

    if ( presetBootOperationState != PRESET_BOOT_OP_IDLE ) {
      drawPresetBootOperationStatus();
      return;
    }

    if ( !presetCacheReady || presetCacheBuilding ) {
      display.setTextColor( TFT_YELLOW, TFT_BLACK );
      display.setTextSize( 2 );
      display.drawString( "Loading Presets", screenWidth / 2, 104 );

      display.setTextColor( TFT_DARKGREY, TFT_BLACK );
      display.setTextSize( 1 );
      display.drawString( "Updating Preset List", screenWidth / 2, 145 );
      return;
    }

    if ( presetBootTargetId > 0 && findPresetCacheIndex( presetBootTargetId ) < 0 ) {
      presetBootTargetId = 0;
      presetBootTargetName = "NONE";
    }

    String displayName = ( presetBootTargetId == 0 ) ? String("NONE") : presetBootTargetName;

    if ( displayName.length() > 28 ) {
      displayName = displayName.substring( 0, 25 ) + "...";
    }

    display.setTextColor( TFT_WHITE, TFT_BLACK );
    display.setTextSize( displayName.length() <= 12 ? 2 : 1 );
    display.drawString( displayName, screenWidth / 2, 82 );

    display.setTextSize( 1 );

    if ( presetBootTargetId == 0 ) {
      display.drawString( "No startup Preset", screenWidth / 2, 105 );
    }
    else {
      char idText[24];
      snprintf( idText, sizeof(idText), "Preset ID: %u", presetBootTargetId );
      display.drawString( idText, screenWidth / 2, 105 );
    }

    drawPresetBootNavigation( TOUCH_TARGET_NONE );

    if ( presetBootTargetId == bootPreset && ( bootPreset == 0 || findPresetCacheIndex( bootPreset ) >= 0 ) ) {
      display.setTextColor( TFT_GREEN, TFT_BLACK );
      display.drawString( bootPreset == 0 ? "Boot Preset Disabled" : "Current Boot Preset", screenWidth / 2, 174 );
    }
    else if ( bootPreset > 0 && findPresetCacheIndex( bootPreset ) < 0 && presetBootTargetId == 0 ) {
      char missingText[40];
      snprintf( missingText, sizeof(missingText), "Configured Boot ID %u missing", bootPreset );
      display.setTextColor( TFT_RED, TFT_BLACK );
      display.drawString( missingText, screenWidth / 2, 174 );
    }
    else {
      display.setTextColor( TFT_WHITE, TFT_BLACK );
      display.drawString( "Set this at startup", screenWidth / 2, 174 );
    }

    display.setTextColor( TFT_DARKGREY, TFT_BLACK );
    display.drawString( "Current LED state is kept", screenWidth / 2, 187 );

    drawPresetBootHoldButton( false );

    lastBootPresetValue = bootPreset;
    lastLedState = bri > 0 ? 1 : 0;
  }

  bool pointInsideRect( int16_t px, int16_t py, const CoreS3TouchRect& rect ) {
    return ( px >= rect.x && px < rect.x + rect.w && py >= rect.y && py < rect.y + rect.h );
  }

  bool isPowerButtonTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_POWER ); }
  bool isBrightnessDownTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_BRIGHTNESS_DOWN ); }
  bool isBrightnessUpTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_BRIGHTNESS_UP ); }
  bool isEffectPrevTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_EFFECT_PREV ); }
  bool isEffectDetailTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_EFFECT_DETAIL ); }
  bool isEffectNextTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_EFFECT_NEXT ); }
  bool isColorButtonTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_COLOR_OPEN ); }
  bool isPresetOpenButtonTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_PRESET_OPEN ); }
  bool isBackButtonTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_BACK ); }
  bool isHueDownTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_HUE_DOWN ); }
  bool isHueUpTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_HUE_UP ); }
  bool isSaturationDownTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_SATURATION_DOWN ); }
  bool isSaturationUpTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_SATURATION_UP ); }
  bool isSpeedDownTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_SPEED_DOWN ); }
  bool isSpeedUpTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_SPEED_UP ); }
  bool isIntensityDownTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_INTENSITY_DOWN ); }
  bool isIntensityUpTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_INTENSITY_UP ); }
  bool isPalettePrevTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_PALETTE_PREV ); }
  bool isPaletteNextTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_PALETTE_NEXT ); }
  bool isPresetPrevTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_PRESET_PREV ); }
  bool isPresetNextTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_PRESET_NEXT ); }
  bool isPresetManageTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_PRESET_MANAGE ); }
  bool isPresetSaveNewTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_PRESET_SAVE_NEW ); }
  bool isPresetSaveHoldTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_PRESET_SAVE_HOLD ); }
  bool isPresetOverwriteOpenTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_OVERWRITE_OPEN ); }
  bool isPresetOverwritePrevTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_OVERWRITE_PREV ); }
  bool isPresetOverwriteNextTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_OVERWRITE_NEXT ); }
  bool isPresetOverwriteHoldTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_OVERWRITE_HOLD ); }
  bool isPresetDeleteOpenTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_DELETE_OPEN ); }
  bool isPresetDeletePrevTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_DELETE_PREV ); }
  bool isPresetDeleteNextTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_DELETE_NEXT ); }
  bool isPresetDeleteHoldTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_DELETE_HOLD ); }
  bool isPresetBootOpenTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_BOOT_OPEN ); }
  bool isPresetBootPrevTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_BOOT_PREV ); }
  bool isPresetBootNextTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_BOOT_NEXT ); }
  bool isPresetBootHoldTouched( int16_t x, int16_t y ) { return pointInsideRect( x, y, CORES3_TOUCH_BOOT_HOLD ); }

  void beginHueEdit() {
    uint32_t currentColor = getPrimaryColor();

    if ( !logicalColorHsvValid || !lastPrimaryColorValid || currentColor != lastPrimaryColor ) {
      syncLogicalColorFromRgb( currentColor );

      lastPrimaryColor = currentColor;

      lastPrimaryColorValid = true;
    }

    hueEditHsv = logicalColorHsv;

    hueEditValue = logicalHueValue;

    hueEditWhite = logicalWhiteValue;

    hueEditValid = true;
  }

  void beginSaturationEdit() {
    uint32_t currentColor = getPrimaryColor();

    if ( !logicalColorHsvValid || !lastPrimaryColorValid || currentColor != lastPrimaryColor ) {
      syncLogicalColorFromRgb( currentColor );

      lastPrimaryColor = currentColor;

      lastPrimaryColorValid = true;
    }

    saturationEditHsv = logicalColorHsv;

    saturationEditValue = logicalSaturationValue;

    saturationEditWhite = logicalWhiteValue;

    saturationEditValid = true;
  }

  void resetTouchGesture() {
    touchActive = false;

    touchTarget = TOUCH_TARGET_NONE;

    lastTouchInsidePower = false;

    lastTouchInsideBrightness = false;

    lastTouchInsideEffect = false;

    lastTouchInsideEffectDetail = false;

    lastTouchInsideColor = false;

    lastTouchInsidePresetOpen = false;

    lastTouchInsideBack = false;

    lastTouchInsideHue = false;

    lastTouchInsideSaturation = false;

    lastTouchInsideSpeed = false;

    lastTouchInsideIntensity = false;

    lastTouchInsidePalette = false;

    lastTouchInsidePresetNav = false;

    lastTouchInsidePresetManage = false;

    lastTouchInsidePresetSaveNew = false;

    lastTouchInsidePresetSaveHold = false;

    lastTouchInsidePresetOverwriteOpen = false;

    lastTouchInsidePresetOverwriteNav = false;

    lastTouchInsidePresetOverwriteHold = false;

    lastTouchInsidePresetDeleteOpen = false;

    lastTouchInsidePresetDeleteNav = false;

    lastTouchInsidePresetDeleteHold = false;

    lastTouchInsidePresetBootOpen = false;
    lastTouchInsidePresetBootNav = false;
    lastTouchInsidePresetBootHold = false;

    powerButtonVisualPressed = false;

    brightnessButtonVisualPressed = false;

    effectButtonVisualPressed = false;

    effectDetailVisualPressed = false;

    colorButtonVisualPressed = false;

    presetOpenButtonVisualPressed = false;

    backButtonVisualPressed = false;

    hueButtonVisualPressed = false;

    saturationButtonVisualPressed = false;

    speedButtonVisualPressed = false;

    intensityButtonVisualPressed = false;

    paletteButtonVisualPressed = false;

    presetNavButtonVisualPressed = false;

    presetManageButtonVisualPressed = false;

    presetSaveNewButtonVisualPressed = false;

    presetSaveHoldButtonVisualPressed = false;

    presetOverwriteOpenButtonVisualPressed = false;

    presetOverwriteNavButtonVisualPressed = false;

    presetOverwriteHoldButtonVisualPressed = false;

    presetDeleteOpenButtonVisualPressed = false;

    presetDeleteNavButtonVisualPressed = false;

    presetDeleteHoldButtonVisualPressed = false;

    presetBootOpenButtonVisualPressed = false;
    presetBootNavButtonVisualPressed = false;
    presetBootHoldButtonVisualPressed = false;

    resetRepeatTouch( brightnessRepeatState );
    resetRepeatTouch( effectRepeatState );
    resetRepeatTouch( hueRepeatState );
    resetRepeatTouch( saturationRepeatState );
    resetRepeatTouch( speedRepeatState );
    resetRepeatTouch( intensityRepeatState );
    resetRepeatTouch( paletteRepeatState );
    resetRepeatTouch( presetRepeatState );

    hueEditValid = false;

    saturationEditValid = false;

    touchReleaseCandidate = 0;

    presetSaveHoldStartTime = 0;

    presetSaveHoldTriggered = false;

    lastTouchX = -1;

    lastTouchY = -1;
  }

  void toggleLedPowerFromTouch() {
    toggleOnOff();

    stateUpdated( CALL_MODE_BUTTON );

    lastLedState = -1;

    lastBrightnessValue = -1;
  }

  bool applyBrightnessValue( int newValue ) {
    newValue = constrain( newValue, 0, 255 );

    if ( newValue == bri ) {
      return false;
    }

    if ( newValue == 0 ) {
      if ( bri > 0 ) {
        briLast = bri;

        bri = 0;
      }
    }
    else {
      if ( bri == 0 ) {
        strip.restartRuntime();
      }

      bri = (uint8_t)newValue;
    }

    stateUpdated( CALL_MODE_BUTTON );

    lastLedState = -1;

    return true;
  }

  void applyBrightnessStep( int step ) {
    int newValue = constrain( (int)bri + step, 0, 255 );

    if ( applyBrightnessValue( newValue ) ) {
      if ( currentPage == SCREEN_MAIN ) {
        drawBrightness( bri, touchTarget );
      }

      lastBrightnessValue = bri;
    }
  }

  void brightnessShortPress( TouchTarget target ) {
    if ( target == TOUCH_TARGET_BRIGHTNESS_DOWN ) {
      applyBrightnessStep( -BRI_SHORT_STEP );
    }
    else if ( target == TOUCH_TARGET_BRIGHTNESS_UP ) {
      applyBrightnessStep( BRI_SHORT_STEP );
    }
  }

  void brightnessLongPressStep( TouchTarget target ) {
    if ( target == TOUCH_TARGET_BRIGHTNESS_DOWN ) {
      applyBrightnessStep( -BRI_LONG_STEP );
    }
    else if ( target == TOUCH_TARGET_BRIGHTNESS_UP ) {
      applyBrightnessStep( BRI_LONG_STEP );
    }
  }

  void applyEffectStep( int step ) {
    uint8_t modeCount = strip.getModeCount();

    if ( modeCount == 0 ) {
      return;
    }

    Segment& mainSegment = strip.getMainSegment();

    int newMode = mainSegment.mode + step;

    if ( newMode < 0 ) {
      newMode = modeCount - 1;
    }

    if ( newMode >= modeCount ) {
      newMode = 0;
    }

    if ( newMode == mainSegment.mode ) {
      return;
    }

    mainSegment.setMode( (uint8_t)newMode );

    stateUpdated( CALL_MODE_BUTTON );

    if ( currentPage == SCREEN_MAIN ) {
      drawEffect( mainSegment.mode, touchTarget );
    }

    lastEffectMode = mainSegment.mode;

    lastSpeedValue = mainSegment.speed;

    lastIntensityValue = mainSegment.intensity;

    lastPaletteValue = mainSegment.palette;
  }

  void effectLongPressStep( TouchTarget target ) {
    if ( target == TOUCH_TARGET_EFFECT_PREV ) {
      applyEffectStep( -1 );
    }
    else if ( target == TOUCH_TARGET_EFFECT_NEXT ) {
      applyEffectStep( 1 );
    }
  }

  bool applySpeedValue( int newValue ) {
    if ( strip.getSegmentsNum() == 0 ) {
      return false;
    }

    newValue = constrain( newValue, 0, 255 );

    Segment& mainSegment = strip.getMainSegment();

    if ( newValue == mainSegment.speed ) {
      return false;
    }

    mainSegment.speed = (uint8_t)newValue;

    stateUpdated( CALL_MODE_BUTTON );

    if ( currentPage == SCREEN_EFFECT ) {
      drawSpeed( mainSegment.speed, touchTarget );
    }

    lastSpeedValue = mainSegment.speed;

    return true;
  }

  void applySpeedStep( int step ) {
    int newValue = constrain( (int)getCurrentSpeed() + step, 0, 255 );

    applySpeedValue( newValue );
  }

  void speedShortPress( TouchTarget target ) {
    if ( target == TOUCH_TARGET_SPEED_DOWN ) {
      applySpeedStep( -SPEED_SHORT_STEP );
    }
    else if ( target == TOUCH_TARGET_SPEED_UP ) {
      applySpeedStep( SPEED_SHORT_STEP );
    }
  }

  void speedLongPressStep( TouchTarget target ) {
    if ( target == TOUCH_TARGET_SPEED_DOWN ) {
      applySpeedStep( -SPEED_LONG_STEP );
    }
    else if ( target == TOUCH_TARGET_SPEED_UP ) {
      applySpeedStep( SPEED_LONG_STEP );
    }
  }

  bool applyIntensityValue( int newValue ) {
    if ( strip.getSegmentsNum() == 0 ) {
      return false;
    }

    newValue = constrain( newValue, 0, 255 );

    Segment& mainSegment = strip.getMainSegment();

    if ( newValue == mainSegment.intensity ) {
      return false;
    }

    mainSegment.intensity = (uint8_t)newValue;

    stateUpdated( CALL_MODE_BUTTON );

    if ( currentPage == SCREEN_EFFECT ) {
      drawIntensity( mainSegment.intensity, touchTarget );
    }

    lastIntensityValue = mainSegment.intensity;

    return true;
  }

  void applyIntensityStep( int step ) {
    int newValue = constrain( (int)getCurrentIntensity() + step, 0, 255 );

    applyIntensityValue( newValue );
  }

  void intensityShortPress( TouchTarget target ) {
    if ( target == TOUCH_TARGET_INTENSITY_DOWN ) {
      applyIntensityStep( -INTENSITY_SHORT_STEP );
    }
    else if ( target == TOUCH_TARGET_INTENSITY_UP ) {
      applyIntensityStep( INTENSITY_SHORT_STEP );
    }
  }

  void intensityLongPressStep( TouchTarget target ) {
    if ( target == TOUCH_TARGET_INTENSITY_DOWN ) {
      applyIntensityStep( -INTENSITY_LONG_STEP );
    }
    else if ( target == TOUCH_TARGET_INTENSITY_UP ) {
      applyIntensityStep( INTENSITY_LONG_STEP );
    }
  }

  bool applyPaletteValue( uint8_t newPalette ) {
    if ( strip.getSegmentsNum() == 0 ) {
      return false;
    }

    Segment& mainSegment = strip.getMainSegment();

    if ( newPalette == mainSegment.palette ) {
      return false;
    }

    mainSegment.setPalette( newPalette );

    stateUpdated( CALL_MODE_BUTTON );

    if ( currentPage == SCREEN_EFFECT ) {
      drawPalette( mainSegment.palette, touchTarget );
    }

    lastPaletteValue = mainSegment.palette;

    return true;
  }

  void applyPaletteStep( int step ) {
    size_t paletteCount = getSelectablePaletteCount();

    if ( paletteCount == 0 ) {
      return;
    }

    uint8_t currentPalette = getCurrentPalette();

    int currentIndex = findPaletteSequenceIndex( currentPalette );

    if ( currentIndex < 0 ) {
      currentIndex = 0;
    }

    int newIndex = currentIndex + step;

    while ( newIndex < 0 ) {
      newIndex += (int)paletteCount;
    }

    while ( newIndex >= (int)paletteCount ) {
      newIndex -= (int)paletteCount;
    }

    uint8_t newPalette = paletteIdFromSequenceIndex( (size_t)newIndex );

    applyPaletteValue( newPalette );
  }

  void paletteShortPress( TouchTarget target ) {
    if ( target == TOUCH_TARGET_PALETTE_PREV ) {
      applyPaletteStep( -1 );
    }
    else if ( target == TOUCH_TARGET_PALETTE_NEXT ) {
      applyPaletteStep( 1 );
    }
  }

  void paletteLongPressStep( TouchTarget target ) {
    if ( target == TOUCH_TARGET_PALETTE_PREV ) {
      applyPaletteStep( -1 );
    }
    else if ( target == TOUCH_TARGET_PALETTE_NEXT ) {
      applyPaletteStep( 1 );
    }
  }

  bool applyPresetStep( int direction ) {
    if ( direction == 0 ) {
      return false;
    }

    if ( !presetCacheReady ) {
      if ( currentPage == SCREEN_PRESET ) {
        drawPresetDetails( getDisplayedPresetId(), false );

        drawPresetNavigation( TOUCH_TARGET_NONE );
      }

      Serial.println( F( "[CoreS3_Display] " "Preset cache not ready" ) );

      return false;
    }

    uint8_t basePreset = getPresetNavigationBaseId();

    String presetName;

    uint8_t newPreset = findAdjacentPreset( basePreset, direction, &presetName );

    if ( newPreset == 0 ) {
      presetNoEntries = true;

      pendingPresetId = 0;

      pendingPresetName = "";

      pendingPresetRequestMs = 0;

      if ( currentPage == SCREEN_PRESET ) {
        drawPresetDetails( 0, false );

        drawPresetNavigation( touchTarget );
      }

      lastPresetValue = 0;

      return false;
    }

    presetNoEntries = false;

    pendingPresetId = newPreset;

    pendingPresetName = presetName;

    pendingPresetRequestMs = millis();

    applyPreset( newPreset, CALL_MODE_BUTTON_PRESET );

    if ( currentPage == SCREEN_PRESET ) {
      drawPresetDetails( newPreset, true );

      drawPresetNavigation( touchTarget );
    }

    lastPresetValue = newPreset;

    Serial.printf( "[CoreS3_Display] " "Preset cache request: %u (%s)\n", newPreset, presetName.c_str() );

    return true;
  }

  void presetShortPress( TouchTarget target ) {
    if ( target == TOUCH_TARGET_PRESET_PREV ) {
      applyPresetStep( -1 );
    }
    else if ( target == TOUCH_TARGET_PRESET_NEXT ) {
      applyPresetStep( 1 );
    }
  }

  void presetLongPressStep( TouchTarget target ) {
    if ( target == TOUCH_TARGET_PRESET_PREV ) {
      applyPresetStep( -1 );
    }
    else if ( target == TOUCH_TARGET_PRESET_NEXT ) {
      applyPresetStep( 1 );
    }
  }

  bool applyHueValue( uint8_t newHue ) {
    if ( strip.getSegmentsNum() == 0 ) {
      return false;
    }

    if (!hueEditValid) {
      beginHueEdit();
    }

    if (!hueEditValid) {
      return false;
    }

    hueEditValue = newHue;

    hueEditHsv.h = ((uint16_t)newHue) << 8;

    logicalHueValue = newHue;

    logicalColorHsv = hueEditHsv;

    logicalSaturationValue = logicalColorHsv.s;

    logicalWhiteValue = hueEditWhite;

    logicalColorHsvValid = true;

    CRGBW newRgb;

    hsv2rgb_spectrum( logicalColorHsv, newRgb );

    newRgb.w = logicalWhiteValue;

    uint32_t newColor = newRgb.color32;

    Segment& mainSegment = strip.getMainSegment();

    if ( newColor != mainSegment.colors[0] ) {
      mainSegment.setColor( 0, newColor );

      stateUpdated( CALL_MODE_BUTTON );
    }

    if ( currentPage == SCREEN_COLOR ) {
      drawColorDetails( newColor );

      drawHue( logicalHueValue, touchTarget );

      drawSaturation( logicalSaturationValue, TOUCH_TARGET_NONE );
    }

    lastPrimaryColor = newColor;

    lastPrimaryColorValid = true;

    lastHueValue = logicalHueValue;

    lastSaturationValue = logicalSaturationValue;

    return true;
  }

  void applyHueStep( int step ) {
    if (!hueEditValid) {
      beginHueEdit();
    }

    if (!hueEditValid) {
      return;
    }

    int newValue = (int)logicalHueValue + step;

    while ( newValue < 0 ) {
      newValue += 256;
    }

    while ( newValue > 255 ) {
      newValue -= 256;
    }

    applyHueValue( (uint8_t)newValue );
  }

  void hueShortPress( TouchTarget target ) {
    if ( target == TOUCH_TARGET_HUE_DOWN ) {
      applyHueStep( -HUE_SHORT_STEP );
    }
    else if ( target == TOUCH_TARGET_HUE_UP ) {
      applyHueStep( HUE_SHORT_STEP );
    }
  }

  void hueLongPressStep( TouchTarget target ) {
    if ( target == TOUCH_TARGET_HUE_DOWN ) {
      applyHueStep( -HUE_LONG_STEP );
    }
    else if ( target == TOUCH_TARGET_HUE_UP ) {
      applyHueStep( HUE_LONG_STEP );
    }
  }

  bool applySaturationValue( uint8_t newSaturation ) {
    if ( strip.getSegmentsNum() == 0 ) {
      return false;
    }

    if (!saturationEditValid) {
      beginSaturationEdit();
    }

    if (!saturationEditValid) {
      return false;
    }

    saturationEditValue = newSaturation;

    saturationEditHsv.s = newSaturation;

    logicalSaturationValue = newSaturation;

    logicalColorHsv = saturationEditHsv;

    logicalHueValue = (uint8_t)( logicalColorHsv.h >> 8 );

    logicalWhiteValue = saturationEditWhite;

    logicalColorHsvValid = true;

    CRGBW newRgb;

    hsv2rgb_spectrum( logicalColorHsv, newRgb );

    newRgb.w = logicalWhiteValue;

    uint32_t newColor = newRgb.color32;

    Segment& mainSegment = strip.getMainSegment();

    if ( newColor != mainSegment.colors[0] ) {
      mainSegment.setColor( 0, newColor );

      stateUpdated( CALL_MODE_BUTTON );
    }

    if ( currentPage == SCREEN_COLOR ) {
      drawColorDetails( newColor );

      drawHue( logicalHueValue, TOUCH_TARGET_NONE );

      drawSaturation( logicalSaturationValue, touchTarget );
    }

    lastPrimaryColor = newColor;

    lastPrimaryColorValid = true;

    lastHueValue = logicalHueValue;

    lastSaturationValue = logicalSaturationValue;

    return true;
  }

  void applySaturationStep( int step ) {
    if (!saturationEditValid) {
      beginSaturationEdit();
    }

    if (!saturationEditValid) {
      return;
    }

    int newValue = constrain( (int)logicalSaturationValue + step, 0, 255 );

    if ( newValue == logicalSaturationValue ) {
      return;
    }

    applySaturationValue( (uint8_t)newValue );
  }

  void saturationShortPress( TouchTarget target ) {
    if ( target == TOUCH_TARGET_SATURATION_DOWN ) {
      applySaturationStep( -SATURATION_SHORT_STEP );
    }
    else if ( target == TOUCH_TARGET_SATURATION_UP ) {
      applySaturationStep( SATURATION_SHORT_STEP );
    }
  }

  void saturationLongPressStep( TouchTarget target ) {
    if ( target == TOUCH_TARGET_SATURATION_DOWN ) {
      applySaturationStep( -SATURATION_LONG_STEP );
    }
    else if ( target == TOUCH_TARGET_SATURATION_UP ) {
      applySaturationStep( SATURATION_LONG_STEP );
    }
  }

  // =========================================================
  // Shared paired-touch helpers
  // =========================================================

  bool isTouchTargetPair( TouchTarget target, TouchTarget firstTarget, TouchTarget secondTarget ) {
    return target == firstTarget || target == secondTarget;
  }

  bool isSelectedTouchPairInside( TouchTarget firstTarget, bool firstInside, TouchTarget secondTarget, bool secondInside ) {
    if ( touchTarget == firstTarget ) {
      return firstInside;
    }

    if ( touchTarget == secondTarget ) {
      return secondInside;
    }

    return false;
  }

  // =========================================================
  // Touch processing
  // =========================================================

  // =========================================================
  // Build one touch hit-test snapshot
  // =========================================================

  TouchHitState buildTouchHitState( int16_t touchX, int16_t touchY ) {
    TouchHitState hit;

    hit.insidePower = isPowerButtonTouched( touchX, touchY );

    if ( currentPage == SCREEN_MAIN ) {
      hit.insideBrightnessDown = isBrightnessDownTouched( touchX, touchY );

      hit.insideBrightnessUp = isBrightnessUpTouched( touchX, touchY );

      hit.insideEffectPrev = isEffectPrevTouched( touchX, touchY );

      hit.insideEffectDetail = isEffectDetailTouched( touchX, touchY );

      hit.insideEffectNext = isEffectNextTouched( touchX, touchY );

      hit.insideColor = isColorButtonTouched( touchX, touchY );

      hit.insidePresetOpen = isPresetOpenButtonTouched( touchX, touchY );
    }

    if ( currentPage == SCREEN_COLOR ) {
      hit.insideBack = isBackButtonTouched( touchX, touchY );

      hit.insideHueDown = isHueDownTouched( touchX, touchY );

      hit.insideHueUp = isHueUpTouched( touchX, touchY );

      hit.insideSaturationDown = isSaturationDownTouched( touchX, touchY );

      hit.insideSaturationUp = isSaturationUpTouched( touchX, touchY );
    }

    if ( currentPage == SCREEN_EFFECT ) {
      hit.insideBack = isBackButtonTouched( touchX, touchY );

      hit.insideSpeedDown = isSpeedDownTouched( touchX, touchY );

      hit.insideSpeedUp = isSpeedUpTouched( touchX, touchY );

      hit.insideIntensityDown = isIntensityDownTouched( touchX, touchY );

      hit.insideIntensityUp = isIntensityUpTouched( touchX, touchY );

      hit.insidePalettePrev = isPalettePrevTouched( touchX, touchY );

      hit.insidePaletteNext = isPaletteNextTouched( touchX, touchY );
    }

    if ( currentPage == SCREEN_PRESET ) {
      hit.insideBack = isBackButtonTouched( touchX, touchY );

      if ( presetSubPage == PRESET_SUBPAGE_NAV ) {
        hit.insidePresetPrev = isPresetPrevTouched( touchX, touchY );

        hit.insidePresetNext = isPresetNextTouched( touchX, touchY );

        hit.insidePresetManage = ( presetCacheReady && !presetCacheBuilding && pendingPresetId == 0 && isPresetManageTouched( touchX, touchY ) );
      }
      else if ( presetSubPage == PRESET_SUBPAGE_MANAGE ) {
        hit.insidePresetSaveNew = ( findFirstFreePresetId() > 0 && pendingPresetId == 0 && !presetNeedsSaving() && isPresetSaveNewTouched( touchX, touchY ) );

        hit.insidePresetOverwriteOpen = ( presetCacheReady && !presetCacheBuilding && presetCacheCount > 0 && pendingPresetId == 0 && !presetNeedsSaving() && isPresetOverwriteOpenTouched( touchX, touchY ) );

        hit.insidePresetDeleteOpen = ( presetCacheReady && !presetCacheBuilding && presetCacheCount > 0 && pendingPresetId == 0 && !presetNeedsSaving() && isPresetDeleteOpenTouched( touchX, touchY ) );

        hit.insidePresetBootOpen = ( presetCacheReady && !presetCacheBuilding && pendingPresetId == 0 && !presetNeedsSaving() && isPresetBootOpenTouched( touchX, touchY ) );
      }
      else if ( presetSubPage == PRESET_SUBPAGE_SAVE ) {
        hit.insidePresetSaveHold = ( presetSaveOperationState == PRESET_SAVE_OP_IDLE && presetCacheReady && !presetCacheBuilding && presetSaveCandidateId > 0 && findPresetCacheIndex( presetSaveCandidateId ) < 0 && pendingPresetId == 0 && !presetNeedsSaving() && isPresetSaveHoldTouched( touchX, touchY ) );
      }
      else if ( presetSubPage == PRESET_SUBPAGE_OVERWRITE ) {
        hit.insidePresetOverwritePrev = ( presetCacheReady && !presetCacheBuilding && presetCacheCount > 0 && isPresetOverwritePrevTouched( touchX, touchY ) );

        hit.insidePresetOverwriteNext = ( presetCacheReady && !presetCacheBuilding && presetCacheCount > 0 && isPresetOverwriteNextTouched( touchX, touchY ) );

        hit.insidePresetOverwriteHold = ( presetSaveOperationState == PRESET_SAVE_OP_IDLE && presetCacheReady && !presetCacheBuilding && presetOverwriteTargetId > 0 && findPresetCacheIndex( presetOverwriteTargetId ) >= 0 && pendingPresetId == 0 && !presetNeedsSaving() && isPresetOverwriteHoldTouched( touchX, touchY ) );
      }
      else if ( presetSubPage == PRESET_SUBPAGE_DELETE ) {
        hit.insidePresetDeletePrev = ( presetCacheReady && !presetCacheBuilding && presetCacheCount > 0 && isPresetDeletePrevTouched( touchX, touchY ) );

        hit.insidePresetDeleteNext = ( presetCacheReady && !presetCacheBuilding && presetCacheCount > 0 && isPresetDeleteNextTouched( touchX, touchY ) );

        hit.insidePresetDeleteHold = ( presetDeleteOperationState == PRESET_DELETE_OP_IDLE && presetCacheReady && !presetCacheBuilding && presetDeleteTargetId > 0 && findPresetCacheIndex( presetDeleteTargetId ) >= 0 && pendingPresetId == 0 && !presetNeedsSaving() && isPresetDeleteHoldTouched( touchX, touchY ) );
      }
      else if ( presetSubPage == PRESET_SUBPAGE_BOOT ) {
        hit.insidePresetBootPrev = ( presetCacheReady && !presetCacheBuilding && presetCacheCount > 0 && isPresetBootPrevTouched( touchX, touchY ) );

        hit.insidePresetBootNext = ( presetCacheReady && !presetCacheBuilding && presetCacheCount > 0 && isPresetBootNextTouched( touchX, touchY ) );

        hit.insidePresetBootHold = ( presetBootOperationState == PRESET_BOOT_OP_IDLE && presetCacheReady && !presetCacheBuilding && pendingPresetId == 0 && !presetNeedsSaving() && isPresetBootTargetValid() && !isPresetBootTargetCurrent() && isPresetBootHoldTouched( touchX, touchY ) );
      }
    }


    return hit;
  }

  // =========================================================
  // Touch Press
  //
  // Starts a new gesture and acquires its TouchTarget.
  // =========================================================

  void handleTouchPress( const TouchHitState& hit, unsigned long now ) {
    if (!touchActive) {
      touchActive = true;

      touchTarget = TOUCH_TARGET_NONE;

      resetRepeatTouch( brightnessRepeatState );
      resetRepeatTouch( effectRepeatState );
      resetRepeatTouch( hueRepeatState );
      resetRepeatTouch( saturationRepeatState );
      resetRepeatTouch( speedRepeatState );
      resetRepeatTouch( intensityRepeatState );
      resetRepeatTouch( paletteRepeatState );
      resetRepeatTouch( presetRepeatState );

      presetSaveHoldStartTime = 0;

      presetSaveHoldTriggered = false;
    }

    if ( touchTarget == TOUCH_TARGET_NONE ) {
      if (hit.insidePower) {
        touchTarget = TOUCH_TARGET_POWER;

        lastTouchInsidePower = true;
      }

      else if ( currentPage == SCREEN_MAIN ) {
        if (hit.insideBrightnessDown) {
          touchTarget = TOUCH_TARGET_BRIGHTNESS_DOWN;

          lastTouchInsideBrightness = true;

          beginRepeatTouch( brightnessRepeatState, now );
        }
        else if (hit.insideBrightnessUp) {
          touchTarget = TOUCH_TARGET_BRIGHTNESS_UP;

          lastTouchInsideBrightness = true;

          beginRepeatTouch( brightnessRepeatState, now );
        }
        else if (hit.insideEffectPrev) {
          touchTarget = TOUCH_TARGET_EFFECT_PREV;

          lastTouchInsideEffect = true;

          beginRepeatTouch( effectRepeatState, now );
        }
        else if (hit.insideEffectDetail) {
          touchTarget = TOUCH_TARGET_EFFECT_DETAIL;

          lastTouchInsideEffectDetail = true;
        }
        else if (hit.insideEffectNext) {
          touchTarget = TOUCH_TARGET_EFFECT_NEXT;

          lastTouchInsideEffect = true;

          beginRepeatTouch( effectRepeatState, now );
        }
        else if (hit.insideColor) {
          touchTarget = TOUCH_TARGET_COLOR_OPEN;

          lastTouchInsideColor = true;
        }
        else if (hit.insidePresetOpen) {
          touchTarget = TOUCH_TARGET_PRESET_OPEN;

          lastTouchInsidePresetOpen = true;
        }
      }

      else if ( currentPage == SCREEN_COLOR ) {
        if (hit.insideBack) {
          touchTarget = TOUCH_TARGET_BACK;

          lastTouchInsideBack = true;
        }
        else if (hit.insideHueDown) {
          touchTarget = TOUCH_TARGET_HUE_DOWN;

          lastTouchInsideHue = true;

          beginRepeatTouch( hueRepeatState, now );

          beginHueEdit();
        }
        else if (hit.insideHueUp) {
          touchTarget = TOUCH_TARGET_HUE_UP;

          lastTouchInsideHue = true;

          beginRepeatTouch( hueRepeatState, now );

          beginHueEdit();
        }
        else if (hit.insideSaturationDown) {
          touchTarget = TOUCH_TARGET_SATURATION_DOWN;

          lastTouchInsideSaturation = true;

          beginRepeatTouch( saturationRepeatState, now );

          beginSaturationEdit();
        }
        else if (hit.insideSaturationUp) {
          touchTarget = TOUCH_TARGET_SATURATION_UP;

          lastTouchInsideSaturation = true;

          beginRepeatTouch( saturationRepeatState, now );

          beginSaturationEdit();
        }
      }

      else if ( currentPage == SCREEN_EFFECT ) {
        if (hit.insideBack) {
          touchTarget = TOUCH_TARGET_BACK;

          lastTouchInsideBack = true;
        }
        else if (hit.insideSpeedDown) {
          touchTarget = TOUCH_TARGET_SPEED_DOWN;

          lastTouchInsideSpeed = true;

          beginRepeatTouch( speedRepeatState, now );
        }
        else if (hit.insideSpeedUp) {
          touchTarget = TOUCH_TARGET_SPEED_UP;

          lastTouchInsideSpeed = true;

          beginRepeatTouch( speedRepeatState, now );
        }
        else if (hit.insideIntensityDown) {
          touchTarget = TOUCH_TARGET_INTENSITY_DOWN;

          lastTouchInsideIntensity = true;

          beginRepeatTouch( intensityRepeatState, now );
        }
        else if (hit.insideIntensityUp) {
          touchTarget = TOUCH_TARGET_INTENSITY_UP;

          lastTouchInsideIntensity = true;

          beginRepeatTouch( intensityRepeatState, now );
        }
        else if (hit.insidePalettePrev) {
          touchTarget = TOUCH_TARGET_PALETTE_PREV;

          lastTouchInsidePalette = true;

          beginRepeatTouch( paletteRepeatState, now );
        }
        else if (hit.insidePaletteNext) {
          touchTarget = TOUCH_TARGET_PALETTE_NEXT;

          lastTouchInsidePalette = true;

          beginRepeatTouch( paletteRepeatState, now );
        }
      }

      else if ( currentPage == SCREEN_PRESET ) {
        if (hit.insideBack) {
          touchTarget = TOUCH_TARGET_BACK;

          lastTouchInsideBack = true;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_NAV && hit.insidePresetPrev ) {
          touchTarget = TOUCH_TARGET_PRESET_PREV;

          lastTouchInsidePresetNav = true;

          beginRepeatTouch( presetRepeatState, now );
        }
        else if ( presetSubPage == PRESET_SUBPAGE_NAV && hit.insidePresetNext ) {
          touchTarget = TOUCH_TARGET_PRESET_NEXT;

          lastTouchInsidePresetNav = true;

          beginRepeatTouch( presetRepeatState, now );
        }
        else if ( presetSubPage == PRESET_SUBPAGE_NAV && hit.insidePresetManage ) {
          touchTarget = TOUCH_TARGET_PRESET_MANAGE;

          lastTouchInsidePresetManage = true;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_MANAGE && hit.insidePresetSaveNew ) {
          touchTarget = TOUCH_TARGET_PRESET_SAVE_NEW;

          lastTouchInsidePresetSaveNew = true;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_MANAGE && hit.insidePresetOverwriteOpen ) {
          touchTarget = TOUCH_TARGET_PRESET_OVERWRITE_OPEN;

          lastTouchInsidePresetOverwriteOpen = true;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_MANAGE && hit.insidePresetDeleteOpen ) {
          touchTarget = TOUCH_TARGET_PRESET_DELETE_OPEN;

          lastTouchInsidePresetDeleteOpen = true;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_MANAGE && hit.insidePresetBootOpen ) {
          touchTarget = TOUCH_TARGET_PRESET_BOOT_OPEN;

          lastTouchInsidePresetBootOpen = true;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_SAVE && hit.insidePresetSaveHold ) {
          touchTarget = TOUCH_TARGET_PRESET_SAVE_HOLD;

          lastTouchInsidePresetSaveHold = true;

          presetSaveHoldStartTime = now;

          presetSaveHoldTriggered = false;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_OVERWRITE && hit.insidePresetOverwritePrev ) {
          touchTarget = TOUCH_TARGET_PRESET_OVERWRITE_PREV;

          lastTouchInsidePresetOverwriteNav = true;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_OVERWRITE && hit.insidePresetOverwriteNext ) {
          touchTarget = TOUCH_TARGET_PRESET_OVERWRITE_NEXT;

          lastTouchInsidePresetOverwriteNav = true;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_OVERWRITE && hit.insidePresetOverwriteHold ) {
          touchTarget = TOUCH_TARGET_PRESET_OVERWRITE_HOLD;

          lastTouchInsidePresetOverwriteHold = true;

          presetSaveHoldStartTime = now;

          presetSaveHoldTriggered = false;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_DELETE && hit.insidePresetDeletePrev ) {
          touchTarget = TOUCH_TARGET_PRESET_DELETE_PREV;

          lastTouchInsidePresetDeleteNav = true;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_DELETE && hit.insidePresetDeleteNext ) {
          touchTarget = TOUCH_TARGET_PRESET_DELETE_NEXT;

          lastTouchInsidePresetDeleteNav = true;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_DELETE && hit.insidePresetDeleteHold ) {
          touchTarget = TOUCH_TARGET_PRESET_DELETE_HOLD;

          lastTouchInsidePresetDeleteHold = true;

          presetSaveHoldStartTime = now;

          presetSaveHoldTriggered = false;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_BOOT && hit.insidePresetBootPrev ) {
          touchTarget = TOUCH_TARGET_PRESET_BOOT_PREV;

          lastTouchInsidePresetBootNav = true;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_BOOT && hit.insidePresetBootNext ) {
          touchTarget = TOUCH_TARGET_PRESET_BOOT_NEXT;

          lastTouchInsidePresetBootNav = true;
        }
        else if ( presetSubPage == PRESET_SUBPAGE_BOOT && hit.insidePresetBootHold ) {
          touchTarget = TOUCH_TARGET_PRESET_BOOT_HOLD;

          lastTouchInsidePresetBootHold = true;

          presetSaveHoldStartTime = now;

          presetSaveHoldTriggered = false;
        }
      }
    }

  }

  // =========================================================
  // Touch Hold
  //
  // Preserves pressed visuals, long-press and repeat behavior.
  // =========================================================

  void handleTouchHold( const TouchHitState& hit, unsigned long now ) {
    if ( touchTarget == TOUCH_TARGET_POWER ) {
      lastTouchInsidePower = hit.insidePower;

      if ( hit.insidePower != powerButtonVisualPressed ) {
        drawPowerButton( bri > 0, hit.insidePower );
      }

      return;
    }

    if ( isTouchTargetPair( touchTarget, TOUCH_TARGET_BRIGHTNESS_DOWN, TOUCH_TARGET_BRIGHTNESS_UP ) ) {
      bool insideSelectedButton = isSelectedTouchPairInside( TOUCH_TARGET_BRIGHTNESS_DOWN, hit.insideBrightnessDown, TOUCH_TARGET_BRIGHTNESS_UP, hit.insideBrightnessUp );

      lastTouchInsideBrightness = insideSelectedButton;

      if ( insideSelectedButton != brightnessButtonVisualPressed ) {
        drawBrightness( bri, insideSelectedButton ? touchTarget : TOUCH_TARGET_NONE );

        brightnessButtonVisualPressed = insideSelectedButton;
      }

      if (!insideSelectedButton) {
        return;
      }

      if ( serviceRepeatTouch( brightnessRepeatState, now, BRI_LONG_PRESS_MS, BRI_REPEAT_MS ) ) {
        brightnessLongPressStep( touchTarget );
      }

      return;
    }

    if ( isTouchTargetPair( touchTarget, TOUCH_TARGET_EFFECT_PREV, TOUCH_TARGET_EFFECT_NEXT ) ) {
      bool insideSelectedButton = isSelectedTouchPairInside( TOUCH_TARGET_EFFECT_PREV, hit.insideEffectPrev, TOUCH_TARGET_EFFECT_NEXT, hit.insideEffectNext );

      lastTouchInsideEffect = insideSelectedButton;

      if ( insideSelectedButton != effectButtonVisualPressed ) {
        drawEffect( getCurrentEffectMode(), insideSelectedButton ? touchTarget : TOUCH_TARGET_NONE );

        effectButtonVisualPressed = insideSelectedButton;
      }

      if (!insideSelectedButton) {
        return;
      }

      if ( serviceRepeatTouch( effectRepeatState, now, EFFECT_LONG_PRESS_MS, EFFECT_REPEAT_MS ) ) {
        effectLongPressStep( touchTarget );
      }

      return;
    }

    if ( touchTarget == TOUCH_TARGET_EFFECT_DETAIL ) {
      bool insideSelectedButton = hit.insideEffectDetail;

      lastTouchInsideEffectDetail = insideSelectedButton;

      if ( insideSelectedButton != effectDetailVisualPressed ) {
        drawEffectDetailButton( getCurrentEffectMode(), insideSelectedButton );
      }

      return;
    }

    if ( touchTarget == TOUCH_TARGET_COLOR_OPEN ) {
      lastTouchInsideColor = hit.insideColor;

      if ( hit.insideColor != colorButtonVisualPressed ) {
        drawColorButton( getPrimaryColor(), hit.insideColor );
      }

      return;
    }

    if ( touchTarget == TOUCH_TARGET_PRESET_OPEN ) {
      lastTouchInsidePresetOpen = hit.insidePresetOpen;

      if ( hit.insidePresetOpen != presetOpenButtonVisualPressed ) {
        drawPresetOpenButton( hit.insidePresetOpen );
      }

      return;
    }

    if ( touchTarget == TOUCH_TARGET_BACK ) {
      lastTouchInsideBack = hit.insideBack;

      if ( hit.insideBack != backButtonVisualPressed ) {
        drawBackButton( hit.insideBack );
      }

      return;
    }

    if ( isTouchTargetPair( touchTarget, TOUCH_TARGET_HUE_DOWN, TOUCH_TARGET_HUE_UP ) ) {
      bool insideSelectedButton = isSelectedTouchPairInside( TOUCH_TARGET_HUE_DOWN, hit.insideHueDown, TOUCH_TARGET_HUE_UP, hit.insideHueUp );

      lastTouchInsideHue = insideSelectedButton;

      if ( insideSelectedButton != hueButtonVisualPressed ) {
        drawHue( logicalHueValue, insideSelectedButton ? touchTarget : TOUCH_TARGET_NONE );

        hueButtonVisualPressed = insideSelectedButton;
      }

      if (!insideSelectedButton) {
        return;
      }

      if ( serviceRepeatTouch( hueRepeatState, now, HUE_LONG_PRESS_MS, HUE_REPEAT_MS ) ) {
        hueLongPressStep( touchTarget );
      }

      return;
    }

    if ( isTouchTargetPair( touchTarget, TOUCH_TARGET_SATURATION_DOWN, TOUCH_TARGET_SATURATION_UP ) ) {
      bool insideSelectedButton = isSelectedTouchPairInside( TOUCH_TARGET_SATURATION_DOWN, hit.insideSaturationDown, TOUCH_TARGET_SATURATION_UP, hit.insideSaturationUp );

      lastTouchInsideSaturation = insideSelectedButton;

      if ( insideSelectedButton != saturationButtonVisualPressed ) {
        drawSaturation( logicalSaturationValue, insideSelectedButton ? touchTarget : TOUCH_TARGET_NONE );

        saturationButtonVisualPressed = insideSelectedButton;
      }

      if (!insideSelectedButton) {
        return;
      }

      if ( serviceRepeatTouch( saturationRepeatState, now, SATURATION_LONG_PRESS_MS, SATURATION_REPEAT_MS ) ) {
        saturationLongPressStep( touchTarget );
      }

      return;
    }

    if ( isTouchTargetPair( touchTarget, TOUCH_TARGET_SPEED_DOWN, TOUCH_TARGET_SPEED_UP ) ) {
      bool insideSelectedButton = isSelectedTouchPairInside( TOUCH_TARGET_SPEED_DOWN, hit.insideSpeedDown, TOUCH_TARGET_SPEED_UP, hit.insideSpeedUp );

      lastTouchInsideSpeed = insideSelectedButton;

      if ( insideSelectedButton != speedButtonVisualPressed ) {
        drawSpeed( getCurrentSpeed(), insideSelectedButton ? touchTarget : TOUCH_TARGET_NONE );

        speedButtonVisualPressed = insideSelectedButton;
      }

      if (!insideSelectedButton) {
        return;
      }

      if ( serviceRepeatTouch( speedRepeatState, now, SPEED_LONG_PRESS_MS, SPEED_REPEAT_MS ) ) {
        speedLongPressStep( touchTarget );
      }

      return;
    }

    if ( isTouchTargetPair( touchTarget, TOUCH_TARGET_INTENSITY_DOWN, TOUCH_TARGET_INTENSITY_UP ) ) {
      bool insideSelectedButton = isSelectedTouchPairInside( TOUCH_TARGET_INTENSITY_DOWN, hit.insideIntensityDown, TOUCH_TARGET_INTENSITY_UP, hit.insideIntensityUp );

      lastTouchInsideIntensity = insideSelectedButton;

      if ( insideSelectedButton != intensityButtonVisualPressed ) {
        drawIntensity( getCurrentIntensity(), insideSelectedButton ? touchTarget : TOUCH_TARGET_NONE );

        intensityButtonVisualPressed = insideSelectedButton;
      }

      if (!insideSelectedButton) {
        return;
      }

      if ( serviceRepeatTouch( intensityRepeatState, now, INTENSITY_LONG_PRESS_MS, INTENSITY_REPEAT_MS ) ) {
        intensityLongPressStep( touchTarget );
      }

      return;
    }

    if ( isTouchTargetPair( touchTarget, TOUCH_TARGET_PALETTE_PREV, TOUCH_TARGET_PALETTE_NEXT ) ) {
      bool insideSelectedButton = isSelectedTouchPairInside( TOUCH_TARGET_PALETTE_PREV, hit.insidePalettePrev, TOUCH_TARGET_PALETTE_NEXT, hit.insidePaletteNext );

      lastTouchInsidePalette = insideSelectedButton;

      if ( insideSelectedButton != paletteButtonVisualPressed ) {
        drawPalette( getCurrentPalette(), insideSelectedButton ? touchTarget : TOUCH_TARGET_NONE );

        paletteButtonVisualPressed = insideSelectedButton;
      }

      if (!insideSelectedButton) {
        return;
      }

      if ( serviceRepeatTouch( paletteRepeatState, now, PALETTE_LONG_PRESS_MS, PALETTE_REPEAT_MS ) ) {
        paletteLongPressStep( touchTarget );
      }

      return;
    }

    if ( isTouchTargetPair( touchTarget, TOUCH_TARGET_PRESET_PREV, TOUCH_TARGET_PRESET_NEXT ) ) {
      bool insideSelectedButton = isSelectedTouchPairInside( TOUCH_TARGET_PRESET_PREV, hit.insidePresetPrev, TOUCH_TARGET_PRESET_NEXT, hit.insidePresetNext );

      lastTouchInsidePresetNav = insideSelectedButton;

      if ( insideSelectedButton != presetNavButtonVisualPressed ) {
        drawPresetNavigation( insideSelectedButton ? touchTarget : TOUCH_TARGET_NONE );

        presetNavButtonVisualPressed = insideSelectedButton;
      }

      if (!insideSelectedButton) {
        return;
      }

      if ( serviceRepeatTouch( presetRepeatState, now, PRESET_LONG_PRESS_MS, PRESET_REPEAT_MS ) ) {
        presetLongPressStep( touchTarget );
      }

      return;
    }

    if ( touchTarget == TOUCH_TARGET_PRESET_MANAGE ) {
      lastTouchInsidePresetManage = hit.insidePresetManage;

      if ( hit.insidePresetManage != presetManageButtonVisualPressed ) {
        drawPresetManageButton( hit.insidePresetManage );
      }

      return;
    }

    if ( touchTarget == TOUCH_TARGET_PRESET_SAVE_NEW ) {
      lastTouchInsidePresetSaveNew = hit.insidePresetSaveNew;

      if ( hit.insidePresetSaveNew != presetSaveNewButtonVisualPressed ) {
        drawPresetSaveNewButton( hit.insidePresetSaveNew );
      }

      return;
    }

    if ( touchTarget == TOUCH_TARGET_PRESET_SAVE_HOLD ) {
      lastTouchInsidePresetSaveHold = hit.insidePresetSaveHold;

      if ( hit.insidePresetSaveHold != presetSaveHoldButtonVisualPressed ) {
        drawPresetSaveHoldButton( hit.insidePresetSaveHold );
      }

      if (!hit.insidePresetSaveHold) {
        presetSaveHoldStartTime = 0;

        return;
      }

      if ( presetSaveHoldStartTime == 0 ) {
        presetSaveHoldStartTime = now;
      }

      if ( !presetSaveHoldTriggered && now - presetSaveHoldStartTime >= PRESET_SAVE_HOLD_MS ) {
        presetSaveHoldTriggered = true;

        requestNewPresetSave();

        return;
      }

      return;
    }

    if ( touchTarget == TOUCH_TARGET_PRESET_OVERWRITE_OPEN ) {
      lastTouchInsidePresetOverwriteOpen = hit.insidePresetOverwriteOpen;

      if ( hit.insidePresetOverwriteOpen != presetOverwriteOpenButtonVisualPressed ) {
        drawPresetOverwriteOpenButton( hit.insidePresetOverwriteOpen );
      }

      return;
    }

    if ( isTouchTargetPair( touchTarget, TOUCH_TARGET_PRESET_OVERWRITE_PREV, TOUCH_TARGET_PRESET_OVERWRITE_NEXT ) ) {
      bool insideSelectedButton = isSelectedTouchPairInside( TOUCH_TARGET_PRESET_OVERWRITE_PREV, hit.insidePresetOverwritePrev, TOUCH_TARGET_PRESET_OVERWRITE_NEXT, hit.insidePresetOverwriteNext );

      lastTouchInsidePresetOverwriteNav = insideSelectedButton;

      if ( insideSelectedButton != presetOverwriteNavButtonVisualPressed ) {
        drawPresetOverwriteNavigation( insideSelectedButton ? touchTarget : TOUCH_TARGET_NONE );

        presetOverwriteNavButtonVisualPressed = insideSelectedButton;
      }

      return;
    }

    if ( touchTarget == TOUCH_TARGET_PRESET_OVERWRITE_HOLD ) {
      lastTouchInsidePresetOverwriteHold = hit.insidePresetOverwriteHold;

      if ( hit.insidePresetOverwriteHold != presetOverwriteHoldButtonVisualPressed ) {
        drawPresetOverwriteHoldButton( hit.insidePresetOverwriteHold );
      }

      if (!hit.insidePresetOverwriteHold) {
        presetSaveHoldStartTime = 0;

        return;
      }

      if ( presetSaveHoldStartTime == 0 ) {
        presetSaveHoldStartTime = now;
      }

      if ( !presetSaveHoldTriggered && now - presetSaveHoldStartTime >= PRESET_SAVE_HOLD_MS ) {
        presetSaveHoldTriggered = true;

        requestPresetOverwrite();

        return;
      }

      return;
    }

    if ( touchTarget == TOUCH_TARGET_PRESET_DELETE_OPEN ) {
      lastTouchInsidePresetDeleteOpen = hit.insidePresetDeleteOpen;

      if ( hit.insidePresetDeleteOpen != presetDeleteOpenButtonVisualPressed ) {
        drawPresetDeleteOpenButton( hit.insidePresetDeleteOpen );
      }

      return;
    }

    if ( isTouchTargetPair( touchTarget, TOUCH_TARGET_PRESET_DELETE_PREV, TOUCH_TARGET_PRESET_DELETE_NEXT ) ) {
      bool insideSelectedButton = isSelectedTouchPairInside( TOUCH_TARGET_PRESET_DELETE_PREV, hit.insidePresetDeletePrev, TOUCH_TARGET_PRESET_DELETE_NEXT, hit.insidePresetDeleteNext );

      lastTouchInsidePresetDeleteNav = insideSelectedButton;

      if ( insideSelectedButton != presetDeleteNavButtonVisualPressed ) {
        drawPresetDeleteNavigation( insideSelectedButton ? touchTarget : TOUCH_TARGET_NONE );

        presetDeleteNavButtonVisualPressed = insideSelectedButton;
      }

      return;
    }

    if ( touchTarget == TOUCH_TARGET_PRESET_DELETE_HOLD ) {
      lastTouchInsidePresetDeleteHold = hit.insidePresetDeleteHold;

      if ( hit.insidePresetDeleteHold != presetDeleteHoldButtonVisualPressed ) {
        drawPresetDeleteHoldButton( hit.insidePresetDeleteHold );
      }

      if (!hit.insidePresetDeleteHold) {
        presetSaveHoldStartTime = 0;

        return;
      }

      if ( presetSaveHoldStartTime == 0 ) {
        presetSaveHoldStartTime = now;
      }

      if ( !presetSaveHoldTriggered && now - presetSaveHoldStartTime >= PRESET_SAVE_HOLD_MS ) {
        presetSaveHoldTriggered = true;

        requestPresetDelete();

        return;
      }

      return;
    }

    if ( touchTarget == TOUCH_TARGET_PRESET_BOOT_OPEN ) {
      lastTouchInsidePresetBootOpen = hit.insidePresetBootOpen;

      if ( hit.insidePresetBootOpen != presetBootOpenButtonVisualPressed ) {
        drawPresetBootOpenButton( hit.insidePresetBootOpen );
      }

      return;
    }

    if ( isTouchTargetPair( touchTarget, TOUCH_TARGET_PRESET_BOOT_PREV, TOUCH_TARGET_PRESET_BOOT_NEXT ) ) {
      bool insideSelectedButton = isSelectedTouchPairInside( TOUCH_TARGET_PRESET_BOOT_PREV, hit.insidePresetBootPrev, TOUCH_TARGET_PRESET_BOOT_NEXT, hit.insidePresetBootNext );

      lastTouchInsidePresetBootNav = insideSelectedButton;

      if ( insideSelectedButton != presetBootNavButtonVisualPressed ) {
        drawPresetBootNavigation( insideSelectedButton ? touchTarget : TOUCH_TARGET_NONE );

        presetBootNavButtonVisualPressed = insideSelectedButton;
      }

      return;
    }

    if ( touchTarget == TOUCH_TARGET_PRESET_BOOT_HOLD ) {
      lastTouchInsidePresetBootHold = hit.insidePresetBootHold;

      if ( hit.insidePresetBootHold != presetBootHoldButtonVisualPressed ) {
        drawPresetBootHoldButton( hit.insidePresetBootHold );
      }

      if (!hit.insidePresetBootHold) {
        presetSaveHoldStartTime = 0;

        return;
      }

      if ( presetSaveHoldStartTime == 0 ) {
        presetSaveHoldStartTime = now;
      }

      if ( !presetSaveHoldTriggered && now - presetSaveHoldStartTime >= PRESET_SAVE_HOLD_MS ) {
        presetSaveHoldTriggered = true;

        requestPresetBootSetting();

        return;
      }

      return;
    }

    return;
    }

  // =========================================================
  // Touch Release action state
  //
  // Only one TouchTarget can own a gesture, so release-time
  // execution is represented by one action instead of parallel
  // boolean flags.
  // =========================================================

  enum TouchReleaseAction : uint8_t {
    TOUCH_RELEASE_ACTION_NONE = 0,
    TOUCH_RELEASE_ACTION_POWER,
    TOUCH_RELEASE_ACTION_BRIGHTNESS_SHORT,
    TOUCH_RELEASE_ACTION_EFFECT_STEP,
    TOUCH_RELEASE_ACTION_EFFECT_DETAIL,
    TOUCH_RELEASE_ACTION_COLOR_OPEN,
    TOUCH_RELEASE_ACTION_PRESET_OPEN,
    TOUCH_RELEASE_ACTION_BACK,
    TOUCH_RELEASE_ACTION_HUE_SHORT,
    TOUCH_RELEASE_ACTION_SATURATION_SHORT,
    TOUCH_RELEASE_ACTION_SPEED_SHORT,
    TOUCH_RELEASE_ACTION_INTENSITY_SHORT,
    TOUCH_RELEASE_ACTION_PALETTE_SHORT,
    TOUCH_RELEASE_ACTION_PRESET_SHORT,
    TOUCH_RELEASE_ACTION_PRESET_MANAGE_OPEN,
    TOUCH_RELEASE_ACTION_PRESET_SAVE_NEW,
    TOUCH_RELEASE_ACTION_PRESET_OVERWRITE_OPEN,
    TOUCH_RELEASE_ACTION_PRESET_OVERWRITE_STEP,
    TOUCH_RELEASE_ACTION_PRESET_DELETE_OPEN,
    TOUCH_RELEASE_ACTION_PRESET_DELETE_STEP,
    TOUCH_RELEASE_ACTION_PRESET_BOOT_OPEN,
    TOUCH_RELEASE_ACTION_PRESET_BOOT_STEP
  };

  struct ColorEditSnapshot {
    bool hueValid;
    CHSV32 hueHsv;
    uint8_t hueValue;
    uint8_t hueWhite;

    bool saturationValid;
    CHSV32 saturationHsv;
    uint8_t saturationValue;
    uint8_t saturationWhite;
  };

  TouchReleaseAction determineTouchReleaseAction( TouchTarget releasedTarget, unsigned long now ) {
    switch (releasedTarget) {
      case TOUCH_TARGET_POWER:
        return ( lastTouchInsidePower && now - lastTouchAction >= TOUCH_ACTION_COOLDOWN_MS ) ? TOUCH_RELEASE_ACTION_POWER : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_BRIGHTNESS_DOWN:
      case TOUCH_TARGET_BRIGHTNESS_UP:
        return ( lastTouchInsideBrightness && !brightnessRepeatState.longPressActive ) ? TOUCH_RELEASE_ACTION_BRIGHTNESS_SHORT : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_EFFECT_PREV:
      case TOUCH_TARGET_EFFECT_NEXT:
        return ( lastTouchInsideEffect && !effectRepeatState.longPressActive ) ? TOUCH_RELEASE_ACTION_EFFECT_STEP : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_EFFECT_DETAIL:
        return lastTouchInsideEffectDetail ? TOUCH_RELEASE_ACTION_EFFECT_DETAIL : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_COLOR_OPEN:
        return lastTouchInsideColor ? TOUCH_RELEASE_ACTION_COLOR_OPEN : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_PRESET_OPEN:
        return lastTouchInsidePresetOpen ? TOUCH_RELEASE_ACTION_PRESET_OPEN : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_BACK:
        return lastTouchInsideBack ? TOUCH_RELEASE_ACTION_BACK : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_HUE_DOWN:
      case TOUCH_TARGET_HUE_UP:
        return ( lastTouchInsideHue && !hueRepeatState.longPressActive ) ? TOUCH_RELEASE_ACTION_HUE_SHORT : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_SATURATION_DOWN:
      case TOUCH_TARGET_SATURATION_UP:
        return ( lastTouchInsideSaturation && !saturationRepeatState.longPressActive ) ? TOUCH_RELEASE_ACTION_SATURATION_SHORT : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_SPEED_DOWN:
      case TOUCH_TARGET_SPEED_UP:
        return ( lastTouchInsideSpeed && !speedRepeatState.longPressActive ) ? TOUCH_RELEASE_ACTION_SPEED_SHORT : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_INTENSITY_DOWN:
      case TOUCH_TARGET_INTENSITY_UP:
        return ( lastTouchInsideIntensity && !intensityRepeatState.longPressActive ) ? TOUCH_RELEASE_ACTION_INTENSITY_SHORT : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_PALETTE_PREV:
      case TOUCH_TARGET_PALETTE_NEXT:
        return ( lastTouchInsidePalette && !paletteRepeatState.longPressActive ) ? TOUCH_RELEASE_ACTION_PALETTE_SHORT : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_PRESET_PREV:
      case TOUCH_TARGET_PRESET_NEXT:
        return ( lastTouchInsidePresetNav && !presetRepeatState.longPressActive ) ? TOUCH_RELEASE_ACTION_PRESET_SHORT : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_PRESET_MANAGE:
        return lastTouchInsidePresetManage ? TOUCH_RELEASE_ACTION_PRESET_MANAGE_OPEN : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_PRESET_SAVE_NEW:
        return lastTouchInsidePresetSaveNew ? TOUCH_RELEASE_ACTION_PRESET_SAVE_NEW : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_PRESET_OVERWRITE_OPEN:
        return lastTouchInsidePresetOverwriteOpen ? TOUCH_RELEASE_ACTION_PRESET_OVERWRITE_OPEN : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_PRESET_OVERWRITE_PREV:
      case TOUCH_TARGET_PRESET_OVERWRITE_NEXT:
        return lastTouchInsidePresetOverwriteNav ? TOUCH_RELEASE_ACTION_PRESET_OVERWRITE_STEP : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_PRESET_DELETE_OPEN:
        return lastTouchInsidePresetDeleteOpen ? TOUCH_RELEASE_ACTION_PRESET_DELETE_OPEN : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_PRESET_DELETE_PREV:
      case TOUCH_TARGET_PRESET_DELETE_NEXT:
        return lastTouchInsidePresetDeleteNav ? TOUCH_RELEASE_ACTION_PRESET_DELETE_STEP : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_PRESET_BOOT_OPEN:
        return lastTouchInsidePresetBootOpen ? TOUCH_RELEASE_ACTION_PRESET_BOOT_OPEN : TOUCH_RELEASE_ACTION_NONE;

      case TOUCH_TARGET_PRESET_BOOT_PREV:
      case TOUCH_TARGET_PRESET_BOOT_NEXT:
        return lastTouchInsidePresetBootNav ? TOUCH_RELEASE_ACTION_PRESET_BOOT_STEP : TOUCH_RELEASE_ACTION_NONE;

      default:
        return TOUCH_RELEASE_ACTION_NONE;
    }
  }

  void releaseTouchVisualState( TouchTarget releasedTarget ) {
    if ( releasedTarget == TOUCH_TARGET_POWER && powerButtonVisualPressed ) {
      drawPowerButton( bri > 0, false );
    }

    if ( ( releasedTarget == TOUCH_TARGET_BRIGHTNESS_DOWN || releasedTarget == TOUCH_TARGET_BRIGHTNESS_UP ) && brightnessButtonVisualPressed ) {
      drawBrightness( bri, TOUCH_TARGET_NONE );
    }

    if ( ( releasedTarget == TOUCH_TARGET_EFFECT_PREV || releasedTarget == TOUCH_TARGET_EFFECT_NEXT ) && effectButtonVisualPressed ) {
      drawEffect( getCurrentEffectMode(), TOUCH_TARGET_NONE );
    }

    if ( releasedTarget == TOUCH_TARGET_EFFECT_DETAIL && effectDetailVisualPressed ) {
      drawEffectDetailButton( getCurrentEffectMode(), false );
    }

    if ( releasedTarget == TOUCH_TARGET_COLOR_OPEN && colorButtonVisualPressed ) {
      drawColorButton( getPrimaryColor(), false );
    }

    if ( releasedTarget == TOUCH_TARGET_PRESET_OPEN && presetOpenButtonVisualPressed ) {
      drawPresetOpenButton( false );
    }

    if ( releasedTarget == TOUCH_TARGET_BACK && backButtonVisualPressed ) {
      drawBackButton( false );
    }

    if ( ( releasedTarget == TOUCH_TARGET_HUE_DOWN || releasedTarget == TOUCH_TARGET_HUE_UP ) && hueButtonVisualPressed ) {
      drawHue( logicalHueValue, TOUCH_TARGET_NONE );
    }

    if ( ( releasedTarget == TOUCH_TARGET_SATURATION_DOWN || releasedTarget == TOUCH_TARGET_SATURATION_UP ) && saturationButtonVisualPressed ) {
      drawSaturation( logicalSaturationValue, TOUCH_TARGET_NONE );
    }

    if ( ( releasedTarget == TOUCH_TARGET_SPEED_DOWN || releasedTarget == TOUCH_TARGET_SPEED_UP ) && speedButtonVisualPressed ) {
      drawSpeed( getCurrentSpeed(), TOUCH_TARGET_NONE );
    }

    if ( ( releasedTarget == TOUCH_TARGET_INTENSITY_DOWN || releasedTarget == TOUCH_TARGET_INTENSITY_UP ) && intensityButtonVisualPressed ) {
      drawIntensity( getCurrentIntensity(), TOUCH_TARGET_NONE );
    }

    if ( ( releasedTarget == TOUCH_TARGET_PALETTE_PREV || releasedTarget == TOUCH_TARGET_PALETTE_NEXT ) && paletteButtonVisualPressed ) {
      drawPalette( getCurrentPalette(), TOUCH_TARGET_NONE );
    }

    if ( ( releasedTarget == TOUCH_TARGET_PRESET_PREV || releasedTarget == TOUCH_TARGET_PRESET_NEXT ) && presetNavButtonVisualPressed ) {
      drawPresetNavigation( TOUCH_TARGET_NONE );
    }

    if ( releasedTarget == TOUCH_TARGET_PRESET_MANAGE && presetManageButtonVisualPressed ) {
      drawPresetManageButton( false );
    }

    if ( releasedTarget == TOUCH_TARGET_PRESET_SAVE_NEW && presetSaveNewButtonVisualPressed ) {
      drawPresetSaveNewButton( false );
    }

    if ( releasedTarget == TOUCH_TARGET_PRESET_SAVE_HOLD && presetSaveHoldButtonVisualPressed ) {
      drawPresetSaveHoldButton( false );
    }

    if ( releasedTarget == TOUCH_TARGET_PRESET_OVERWRITE_OPEN && presetOverwriteOpenButtonVisualPressed ) {
      drawPresetOverwriteOpenButton( false );
    }

    if ( ( releasedTarget == TOUCH_TARGET_PRESET_OVERWRITE_PREV || releasedTarget == TOUCH_TARGET_PRESET_OVERWRITE_NEXT ) && presetOverwriteNavButtonVisualPressed ) {
      drawPresetOverwriteNavigation( TOUCH_TARGET_NONE );
    }

    if ( releasedTarget == TOUCH_TARGET_PRESET_OVERWRITE_HOLD && presetOverwriteHoldButtonVisualPressed ) {
      drawPresetOverwriteHoldButton( false );
    }

    if ( releasedTarget == TOUCH_TARGET_PRESET_DELETE_OPEN && presetDeleteOpenButtonVisualPressed ) {
      drawPresetDeleteOpenButton( false );
    }

    if ( ( releasedTarget == TOUCH_TARGET_PRESET_DELETE_PREV || releasedTarget == TOUCH_TARGET_PRESET_DELETE_NEXT ) && presetDeleteNavButtonVisualPressed ) {
      drawPresetDeleteNavigation( TOUCH_TARGET_NONE );
    }

    if ( releasedTarget == TOUCH_TARGET_PRESET_DELETE_HOLD && presetDeleteHoldButtonVisualPressed ) {
      drawPresetDeleteHoldButton( false );
    }

    if ( releasedTarget == TOUCH_TARGET_PRESET_BOOT_OPEN && presetBootOpenButtonVisualPressed ) {
      drawPresetBootOpenButton( false );
    }

    if ( ( releasedTarget == TOUCH_TARGET_PRESET_BOOT_PREV || releasedTarget == TOUCH_TARGET_PRESET_BOOT_NEXT ) && presetBootNavButtonVisualPressed ) {
      drawPresetBootNavigation( TOUCH_TARGET_NONE );
    }

    if ( releasedTarget == TOUCH_TARGET_PRESET_BOOT_HOLD && presetBootHoldButtonVisualPressed ) {
      drawPresetBootHoldButton( false );
    }
  }

  void clearColorEditState() {
    hueEditValid = false;
    saturationEditValid = false;
  }

  void executeBackReleaseAction() {
    if ( currentPage == SCREEN_PRESET && presetSubPage == PRESET_SUBPAGE_BOOT ) {
      presetBootOperationState = PRESET_BOOT_OP_IDLE;
      presetBootTargetId = 0;
      presetBootTargetName = "NONE";
      presetBootResultStartMs = 0;

      drawPresetManageScreen();

      return;
    }

    if ( currentPage == SCREEN_PRESET && presetSubPage == PRESET_SUBPAGE_DELETE ) {
      presetDeleteOperationState = PRESET_DELETE_OP_IDLE;

      presetDeleteTargetId = 0;

      presetDeleteTargetName = "";

      presetDeleteWasCurrentPreset = false;
      presetDeleteWasBootPreset = false;

      presetDeleteResultStartMs = 0;

      drawPresetManageScreen();

      return;
    }

    if ( currentPage == SCREEN_PRESET && presetSubPage == PRESET_SUBPAGE_OVERWRITE ) {
      presetSaveOperationState = PRESET_SAVE_OP_IDLE;

      presetSaveOperationIsOverwrite = false;

      presetSaveCandidateId = 0;

      presetSaveCandidateName = "";

      presetOverwriteTargetId = 0;

      presetOverwriteTargetName = "";

      drawPresetManageScreen();

      return;
    }

    if ( currentPage == SCREEN_PRESET && presetSubPage == PRESET_SUBPAGE_SAVE ) {
      presetSaveOperationState = PRESET_SAVE_OP_IDLE;

      presetSaveCandidateId = 0;

      presetSaveCandidateName = "";

      drawPresetManageScreen();

      return;
    }

    if ( currentPage == SCREEN_PRESET && presetSubPage == PRESET_SUBPAGE_MANAGE ) {
      drawPresetScreen();

      return;
    }

    drawMainScreen( WiFi.localIP().toString() );
  }

  void executeTouchReleaseAction( TouchReleaseAction action, TouchTarget releasedTarget, unsigned long now ) {
    switch (action) {
      case TOUCH_RELEASE_ACTION_POWER:
        lastTouchAction = now;

        toggleLedPowerFromTouch();

        clearColorEditState();
        return;

      case TOUCH_RELEASE_ACTION_BRIGHTNESS_SHORT:
        brightnessShortPress( releasedTarget );

        drawBrightness( bri, TOUCH_TARGET_NONE );

        lastBrightnessValue = bri;

        clearColorEditState();
        return;

      case TOUCH_RELEASE_ACTION_EFFECT_STEP:
        if ( releasedTarget == TOUCH_TARGET_EFFECT_PREV ) {
          applyEffectStep( -1 );
        }
        else {
          applyEffectStep( 1 );
        }

        drawEffect( getCurrentEffectMode(), TOUCH_TARGET_NONE );

        clearColorEditState();
        return;

      case TOUCH_RELEASE_ACTION_EFFECT_DETAIL:
        clearColorEditState();

        drawEffectDetailScreen();
        return;

      case TOUCH_RELEASE_ACTION_COLOR_OPEN:
        clearColorEditState();

        drawColorScreen();
        return;

      case TOUCH_RELEASE_ACTION_PRESET_OPEN:
        clearColorEditState();

        presetNoEntries = false;

        drawPresetScreen();
        return;

      case TOUCH_RELEASE_ACTION_PRESET_MANAGE_OPEN:
        clearColorEditState();

        drawPresetManageScreen();
        return;

      case TOUCH_RELEASE_ACTION_PRESET_SAVE_NEW:
        clearColorEditState();

        if ( preparePresetSaveCandidate() ) {
          drawPresetSaveScreen();
        }
        else {
          drawPresetManageScreen();
        }
        return;

      case TOUCH_RELEASE_ACTION_PRESET_OVERWRITE_OPEN:
        clearColorEditState();

        if ( preparePresetOverwriteTarget() ) {
          drawPresetOverwriteScreen();
        }
        else {
          drawPresetManageScreen();
        }
        return;

      case TOUCH_RELEASE_ACTION_PRESET_OVERWRITE_STEP: {
        int direction = ( releasedTarget == TOUCH_TARGET_PRESET_OVERWRITE_PREV ) ? -1 : 1;

        if ( stepPresetOverwriteTarget( direction ) ) {
          drawPresetOverwriteScreen();
        }

        clearColorEditState();
        return;
      }

      case TOUCH_RELEASE_ACTION_PRESET_DELETE_OPEN:
        clearColorEditState();

        if ( preparePresetDeleteTarget() ) {
          drawPresetDeleteScreen();
        }
        else {
          drawPresetManageScreen();
        }
        return;

      case TOUCH_RELEASE_ACTION_PRESET_DELETE_STEP: {
        int direction = ( releasedTarget == TOUCH_TARGET_PRESET_DELETE_PREV ) ? -1 : 1;

        if ( stepPresetDeleteTarget( direction ) ) {
          drawPresetDeleteScreen();
        }

        clearColorEditState();
        return;
      }

      case TOUCH_RELEASE_ACTION_PRESET_BOOT_OPEN:
        clearColorEditState();

        if ( preparePresetBootTarget() ) {
          drawPresetBootScreen();
        }
        else {
          drawPresetManageScreen();
        }
        return;

      case TOUCH_RELEASE_ACTION_PRESET_BOOT_STEP: {
        int direction = ( releasedTarget == TOUCH_TARGET_PRESET_BOOT_PREV ) ? -1 : 1;

        if ( stepPresetBootTarget( direction ) ) {
          drawPresetBootScreen();
        }

        clearColorEditState();
        return;
      }

      case TOUCH_RELEASE_ACTION_BACK:
        clearColorEditState();

        executeBackReleaseAction();
        return;

      case TOUCH_RELEASE_ACTION_HUE_SHORT:
        hueShortPress( releasedTarget );

        drawHue( logicalHueValue, TOUCH_TARGET_NONE );

        drawSaturation( logicalSaturationValue, TOUCH_TARGET_NONE );

        clearColorEditState();
        return;

      case TOUCH_RELEASE_ACTION_SATURATION_SHORT:
        saturationShortPress( releasedTarget );

        drawHue( logicalHueValue, TOUCH_TARGET_NONE );

        drawSaturation( logicalSaturationValue, TOUCH_TARGET_NONE );

        clearColorEditState();
        return;

      case TOUCH_RELEASE_ACTION_SPEED_SHORT:
        speedShortPress( releasedTarget );

        drawSpeed( getCurrentSpeed(), TOUCH_TARGET_NONE );

        lastSpeedValue = getCurrentSpeed();

        clearColorEditState();
        return;

      case TOUCH_RELEASE_ACTION_INTENSITY_SHORT:
        intensityShortPress( releasedTarget );

        drawIntensity( getCurrentIntensity(), TOUCH_TARGET_NONE );

        lastIntensityValue = getCurrentIntensity();

        clearColorEditState();
        return;

      case TOUCH_RELEASE_ACTION_PALETTE_SHORT:
        paletteShortPress( releasedTarget );

        drawPalette( getCurrentPalette(), TOUCH_TARGET_NONE );

        lastPaletteValue = getCurrentPalette();

        clearColorEditState();
        return;

      case TOUCH_RELEASE_ACTION_PRESET_SHORT:
        presetShortPress( releasedTarget );

        drawPresetNavigation( TOUCH_TARGET_NONE );

        clearColorEditState();
        return;

      default:
        clearColorEditState();
        return;
    }
  }

  // =========================================================
  // Touch Release
  //
  // Release confirmation is unchanged. Action selection,
  // pressed-visual release, gesture reset, and action execution
  // are now separated into focused helpers.
  // =========================================================

  void handleTouchRelease( unsigned long now ) {
    if (!touchActive) {
      return;
    }

    if ( touchReleaseCandidate == 0 ) {
      touchReleaseCandidate = now;

      return;
    }

    if ( now - touchReleaseCandidate < TOUCH_RELEASE_CONFIRM_MS ) {
      return;
    }

    TouchTarget releasedTarget = touchTarget;

    TouchReleaseAction releaseAction = determineTouchReleaseAction( releasedTarget, now );

    releaseTouchVisualState( releasedTarget );

    ColorEditSnapshot editSnapshot = {
      hueEditValid, hueEditHsv, hueEditValue, hueEditWhite,
      saturationEditValid, saturationEditHsv, saturationEditValue, saturationEditWhite
    };

    resetTouchGesture();

    hueEditValid = editSnapshot.hueValid;
    hueEditHsv = editSnapshot.hueHsv;
    hueEditValue = editSnapshot.hueValue;
    hueEditWhite = editSnapshot.hueWhite;

    saturationEditValid = editSnapshot.saturationValid;
    saturationEditHsv = editSnapshot.saturationHsv;
    saturationEditValue = editSnapshot.saturationValue;
    saturationEditWhite = editSnapshot.saturationWhite;

    executeTouchReleaseAction( releaseAction, releasedTarget, now );
  }

  // =========================================================
  // Touch dispatcher
  //
  // Poll touch once, then route the same state through
  // Press / Hold / Release helpers.
  // =========================================================

  void handleTouch() {
    if (!touchReady) {
      return;
    }

    if (!readyScreenShown) {
      return;
    }

    if ( isPresetSaveBusy() || isPresetDeleteBusy() || isPresetBootBusy() ) {
      return;
    }

    unsigned long now = millis();

    if ( now - lastTouchPoll < TOUCH_POLL_MS ) {
      return;
    }

    lastTouchPoll = now;

    int16_t touchX = -1;

    int16_t touchY = -1;

    bool touching = readDisplayTouch( touchX, touchY );

    if (touching) {

      lastUserActivityMs = now;

      touchReleaseCandidate = 0;

      lastTouchX = touchX;

      lastTouchY = touchY;

      TouchHitState hit = buildTouchHitState( touchX, touchY );

      handleTouchPress( hit, now );

      handleTouchHold( hit, now );

      return;
    }

    handleTouchRelease( now );
  }

  // =========================================================
  // Page-specific runtime display synchronization
  //
  // These helpers contain only UI/WLED state synchronization.
  // Hardware-specific display/touch access remains outside this layer
  // so the same page logic can be reused for Core2 variants later.
  // =========================================================

  bool updateMainPageState( uint8_t effectMode, uint8_t currentSpeed, uint8_t currentIntensity, uint8_t currentPalette, uint32_t primaryColor, bool primaryColorChanged ) {
    bool brightnessTouchActive = ( touchTarget == TOUCH_TARGET_BRIGHTNESS_DOWN || touchTarget == TOUCH_TARGET_BRIGHTNESS_UP );

    if ( !brightnessTouchActive && (int)bri != lastBrightnessValue ) {
      drawBrightness( bri, TOUCH_TARGET_NONE );

      lastBrightnessValue = bri;
    }

    bool effectTouchActive = ( touchTarget == TOUCH_TARGET_EFFECT_PREV || touchTarget == TOUCH_TARGET_EFFECT_DETAIL || touchTarget == TOUCH_TARGET_EFFECT_NEXT );

    if ( !effectTouchActive && (int)effectMode != lastEffectMode ) {
      drawEffect( effectMode, TOUCH_TARGET_NONE );

      lastEffectMode = effectMode;

      lastSpeedValue = currentSpeed;

      lastIntensityValue = currentIntensity;

      lastPaletteValue = currentPalette;
    }

    if ( (int)currentPalette != lastPaletteValue ) {
      lastPaletteValue = currentPalette;
    }

    if ( (int)currentPreset != lastPresetValue ) {
      lastPresetValue = currentPreset;
    }

    if ( primaryColorChanged && touchTarget != TOUCH_TARGET_COLOR_OPEN ) {
      syncLogicalColorFromRgb( primaryColor );

      drawColorButton( primaryColor, false );

      return true;
    }

    return false;
  }

  bool updateColorPageState( uint32_t primaryColor, bool primaryColorChanged, bool colorControlTouchActive ) {
    if ( primaryColorChanged && !colorControlTouchActive ) {
      syncLogicalColorFromRgb( primaryColor );

      drawColorDetails( primaryColor );

      drawHue( logicalHueValue, TOUCH_TARGET_NONE );

      drawSaturation( logicalSaturationValue, TOUCH_TARGET_NONE );

      lastHueValue = logicalHueValue;

      lastSaturationValue = logicalSaturationValue;

      return true;
    }

    return false;
  }

  bool updateEffectPageState( uint8_t effectMode, uint8_t currentSpeed, uint8_t currentIntensity, uint8_t currentPalette ) {
    bool speedTouchActive = ( touchTarget == TOUCH_TARGET_SPEED_DOWN || touchTarget == TOUCH_TARGET_SPEED_UP );

    bool intensityTouchActive = ( touchTarget == TOUCH_TARGET_INTENSITY_DOWN || touchTarget == TOUCH_TARGET_INTENSITY_UP );

    bool paletteTouchActive = ( touchTarget == TOUCH_TARGET_PALETTE_PREV || touchTarget == TOUCH_TARGET_PALETTE_NEXT );

    if ( touchTarget == TOUCH_TARGET_NONE && (int)effectMode != lastEffectMode ) {
      drawEffectDetailScreen();

      return true;
    }

    if ( !speedTouchActive && (int)currentSpeed != lastSpeedValue ) {
      drawSpeed( currentSpeed, TOUCH_TARGET_NONE );

      lastSpeedValue = currentSpeed;
    }

    if ( !intensityTouchActive && (int)currentIntensity != lastIntensityValue ) {
      drawIntensity( currentIntensity, TOUCH_TARGET_NONE );

      lastIntensityValue = currentIntensity;
    }

    if ( !paletteTouchActive && (int)currentPalette != lastPaletteValue ) {
      drawPalette( currentPalette, TOUCH_TARGET_NONE );

      lastPaletteValue = currentPalette;
    }

    return false;
  }

  void updatePresetPageState( uint8_t displayedPreset, bool presetPendingSettled ) {
    if ( presetSubPage == PRESET_SUBPAGE_NAV ) {
      bool presetTouchActive = ( touchTarget == TOUCH_TARGET_PRESET_PREV || touchTarget == TOUCH_TARGET_PRESET_NEXT );

      bool presetFileChanged = ( presetsModifiedTime != lastPresetsModifiedTime );

      if (presetFileChanged) {
        lastPresetsModifiedTime = presetsModifiedTime;

        presetNoEntries = false;
      }

      if ( !presetTouchActive && ( (int)displayedPreset != lastPresetValue || presetPendingSettled || presetFileChanged ) ) {
        drawPresetDetails( displayedPreset, pendingPresetId > 0 );

        drawPresetNavigation( TOUCH_TARGET_NONE );

        lastPresetValue = displayedPreset;
      }

      return;
    }

    if ( presetSubPage == PRESET_SUBPAGE_BOOT ) {
      if ( touchTarget == TOUCH_TARGET_NONE && (int)bootPreset != lastBootPresetValue ) {
        drawPresetBootScreen();
        lastBootPresetValue = bootPreset;
      }
    }
  }

  public:

  CoreS3DisplayUsermod()
    : hardwareBackend( display ) {
  }

  void addToConfig( JsonObject& root ) override {
    JsonObject top = root.createNestedObject( FPSTR( CORES3_DISPLAY_CONFIG_NAME ) );

    top[ F("sleep-timeout") ] = sleepTimeoutSec;

    top[ F("lcd-brightness") ] = lcdBrightness;

    top[ F("fade") ] = fadeEnabled;

    top[ F("fade-duration") ] = fadeDurationMs;
  }

  bool readFromConfig( JsonObject& root ) override {
    JsonObject top = root[ FPSTR( CORES3_DISPLAY_CONFIG_NAME ) ];

    if ( top.isNull() ) {
      Serial.println( F( "[CoreS3_Display] " "No display config found. Using defaults." ) );

      return false;
    }

    bool configComplete = true;

    int newSleepTimeout = sleepTimeoutSec;

    int newLcdBrightness = lcdBrightness;

    bool newFadeEnabled = fadeEnabled;

    int newFadeDuration = fadeDurationMs;

    configComplete &= getJsonValue( top[ F("sleep-timeout") ], newSleepTimeout, 30 );

    configComplete &= getJsonValue( top[ F("lcd-brightness") ], newLcdBrightness, 128 );

    configComplete &= getJsonValue( top[ F("fade") ], newFadeEnabled, true );

    configComplete &= getJsonValue( top[ F("fade-duration") ], newFadeDuration, 250 );

    newSleepTimeout = constrain( newSleepTimeout, 0, 3600 );

    newLcdBrightness = constrain( newLcdBrightness, 1, 255 );

    newFadeDuration = constrain( newFadeDuration, 50, 2000 );

    sleepTimeoutSec = (uint16_t)newSleepTimeout;

    lcdBrightness = (uint16_t)newLcdBrightness;

    fadeEnabled = newFadeEnabled;

    fadeDurationMs = (uint16_t)newFadeDuration;

    Serial.printf( "[CoreS3_Display] " "Config: Sleep=%u sec, " "LCD=%u, " "Fade=%s, " "FadeDuration=%u ms\n", sleepTimeoutSec, lcdBrightness, fadeEnabled ? "ON" : "OFF", fadeDurationMs );

    if ( initDone && displayReady && displayPowerState == DISPLAY_POWER_ACTIVE ) {
      setDisplayBrightness( getNormalDisplayBrightness() );
    }

    return configComplete;
  }

  void appendConfigData( Print& settingsScript ) override {
    settingsScript.print( F( "cs3st=addDropdown(" "'CoreS3_Display'," "'sleep-timeout'" ");" ) );

    settingsScript.print( F( "addOption(" "cs3st," "'Never'," "0" ");" ) );

    settingsScript.print( F( "addOption(" "cs3st," "'15 sec'," "15" ");" ) );

    settingsScript.print( F( "addOption(" "cs3st," "'30 sec'," "30" ");" ) );

    settingsScript.print( F( "addOption(" "cs3st," "'60 sec'," "60" ");" ) );

    settingsScript.print( F( "addOption(" "cs3st," "'120 sec'," "120" ");" ) );

    settingsScript.print( F( "addInfo(" "'CoreS3_Display:lcd-brightness'," "1," "'<small>1-255</small>'" ");" ) );

    settingsScript.print( F( "cs3fd=addDropdown(" "'CoreS3_Display'," "'fade-duration'" ");" ) );

    settingsScript.print( F( "addOption(" "cs3fd," "'Fast - 150 ms'," "150" ");" ) );

    settingsScript.print( F( "addOption(" "cs3fd," "'Normal - 250 ms'," "250" ");" ) );

    settingsScript.print( F( "addOption(" "cs3fd," "'Slow - 400 ms'," "400" ");" ) );

    settingsScript.print( F( "addOption(" "cs3fd," "'Very Slow - 600 ms'," "600" ");" ) );
  }

  void setup() override {
    Serial.println();

    Serial.println( F( "[CoreS3_Display] Initialization start" ) );

    Serial.printf(
      "[CoreS3_Display] " "Hardware: %s, Revision=%s, Runtime=%s\n",
      getHardwareProfileName(),
      getHardwareRevisionName(),
      getHardwareRuntimeModeName()
    );

    Serial.printf(
      "[CoreS3_Display] " "Port status: %s\n",
      getHardwarePortStatusName()
    );

    Serial.printf( "[CoreS3_Display] " "Settings: " "Sleep=%u sec, " "LCD=%u, " "Fade=%s, " "FadeDuration=%u ms\n", sleepTimeoutSec, lcdBrightness, fadeEnabled ? "ON" : "OFF", fadeDurationMs );

    runHardwareDiagnostics();

    if ( isCore2DiagnosticOnlyMode() ) {
      initDone = true;

      Serial.println( F( "[CoreS3_Display] Core2 diagnostic-only runtime complete" ) );
      Serial.println( F( "[CoreS3_Display] Display/Touch/LCD brightness initialization intentionally skipped" ) );
      Serial.println( F( "[CoreS3_Display] Capture the hardware probe result before enabling Core2 UI/Power support" ) );
      Serial.println();

      return;
    }

    if ( !initializeDisplayHardware() ) {
      return;
    }

    setDisplayBrightness( 0 );

    drawStartupBase();

    startupDotCount = 3;

    drawStartupConnectingStatus();

    startupState = STARTUP_FADE_IN;

    startupStateStart = millis();

    startupLastFadeStep = startupStateStart;

    startupLastDotsUpdate = startupStateStart;

    displayPowerState = DISPLAY_POWER_ACTIVE;

    lastUserActivityMs = startupStateStart;

    lastPresetsModifiedTime = presetsModifiedTime;

    presetCacheCount = 0;

    presetCacheReady = false;

    presetCacheBuilding = false;

    presetCacheScanId = 1;

    presetCacheSourceModifiedTime = presetsModifiedTime;

    displayReady = true;

    initDone = true;

    Serial.println( F( "[CoreS3_Display] Initialization complete" ) );

    Serial.println();
  }

  void loop() override {
    if (!displayReady) {
      return;
    }

    if ( startupState != STARTUP_DONE ) {
      handleStartupSequence();

      return;
    }

    if ( !presetCacheReady && !presetCacheBuilding ) {
      startPresetCacheRebuild();
    }

    servicePresetCache();

    servicePresetSaveOperation();

    servicePresetDeleteOperation();

    servicePresetBootOperation();

    if ( displayPowerState == DISPLAY_POWER_ACTIVE ) {
      handleTouch();
    }

    if ( handleDisplayPowerManagement() ) {
      return;
    }

    unsigned long now = millis();

    if ( now - lastUpdate < 250 ) {
      return;
    }

    lastUpdate = now;

    bool presetPendingSettled = settlePendingPreset();

    bool wifiConnected = ( WiFi.status() == WL_CONNECTED );

    if (!wifiConnected) {
      if ( !connectingScreenShown ) {
        drawConnectingScreen();
      }

      lastWiFiConnected = false;

      lastIPAddress = "";

      return;
    }

    String currentIPAddress = WiFi.localIP().toString();

    if ( !lastWiFiConnected || !readyScreenShown ) {
      drawMainScreen( currentIPAddress );

      lastIPAddress = currentIPAddress;

      lastWiFiConnected = true;

      return;
    }

    if ( currentIPAddress != lastIPAddress ) {
      lastIPAddress = currentIPAddress;

      if ( currentPage == SCREEN_MAIN ) {
        drawMainScreen( currentIPAddress );

        return;
      }
    }

    lastWiFiConnected = true;

    bool ledOn = (bri > 0);

    if ( (int8_t)ledOn != lastLedState ) {
      if ( touchTarget != TOUCH_TARGET_POWER ) {
        drawPowerButton( ledOn, false );
      }

      lastLedState = ledOn ? 1 : 0;
    }

    uint8_t effectMode = getCurrentEffectMode();

    uint8_t currentSpeed = getCurrentSpeed();

    uint8_t currentIntensity = getCurrentIntensity();

    uint8_t currentPalette = getCurrentPalette();

    uint8_t displayedPreset = getDisplayedPresetId();

    uint32_t primaryColor = getPrimaryColor();

    bool primaryColorChanged = ( !lastPrimaryColorValid || primaryColor != lastPrimaryColor );

    bool hueTouchActive = ( touchTarget == TOUCH_TARGET_HUE_DOWN || touchTarget == TOUCH_TARGET_HUE_UP );

    bool saturationTouchActive = ( touchTarget == TOUCH_TARGET_SATURATION_DOWN || touchTarget == TOUCH_TARGET_SATURATION_UP );

    bool colorControlTouchActive = ( hueTouchActive || saturationTouchActive );

    bool primaryColorChangeHandled = false;

    if ( currentPage == SCREEN_MAIN ) {
      primaryColorChangeHandled = updateMainPageState( effectMode, currentSpeed, currentIntensity, currentPalette, primaryColor, primaryColorChanged );
    }
    else if ( currentPage == SCREEN_COLOR ) {
      primaryColorChangeHandled = updateColorPageState( primaryColor, primaryColorChanged, colorControlTouchActive );
    }
    else if ( currentPage == SCREEN_EFFECT ) {
      if ( updateEffectPageState( effectMode, currentSpeed, currentIntensity, currentPalette ) ) {
        return;
      }
    }
    else if ( currentPage == SCREEN_PRESET ) {
      updatePresetPageState( displayedPreset, presetPendingSettled );
    }

    if ( primaryColorChanged && primaryColorChangeHandled ) {
      lastPrimaryColor = primaryColor;

      lastPrimaryColorValid = true;
    }
  }

  void addToJsonInfo( JsonObject& root ) override {
    JsonObject user = root["u"];

    if ( user.isNull() ) {
      user = root.createNestedObject( "u" );
    }

    JsonArray hardwareProfileInfo = user.createNestedArray( "M5Stack Hardware Profile" );

    hardwareProfileInfo.add( getHardwareProfileName() );

    JsonArray hardwareRevisionInfo = user.createNestedArray( "M5Stack Hardware Revision" );

    hardwareRevisionInfo.add( getHardwareRevisionName() );

    JsonArray hardwareRuntimeInfo = user.createNestedArray( "M5Stack Runtime Mode" );

    hardwareRuntimeInfo.add( getHardwareRuntimeModeName() );

    JsonArray hardwarePortStatusInfo = user.createNestedArray( "M5Stack Porting Status" );

    hardwarePortStatusInfo.add( getHardwarePortStatusName() );

    JsonArray hardwareProbeInfo = user.createNestedArray( "M5Stack Hardware Probe" );

    hardwareProbeInfo.add( getHardwareProbeStateName() );

    JsonArray hardwareVariantInfo = user.createNestedArray( "M5Stack Detected Variant" );

    hardwareVariantInfo.add( getDetectedVariantName() );

    JsonArray hardwareDetectedRevisionInfo = user.createNestedArray( "M5Stack Detected Revision" );

    hardwareDetectedRevisionInfo.add( getDetectedRevisionName() );

    JsonArray hardwarePmuInfo = user.createNestedArray( "M5Stack Detected PMU" );

    hardwarePmuInfo.add( getDetectedPmuName() );

    JsonArray hardwareImuInfo = user.createNestedArray( "M5Stack Detected IMU" );

    hardwareImuInfo.add( getDetectedImuName() );

    JsonArray hardwareCore2I2cInfo = user.createNestedArray( "M5Stack Core2 I2C Signature" );

    if ( hardwareBackend.isProbeComplete() ) {
      char signature[64];

      snprintf(
        signature,
        sizeof(signature),
        "34:%c 35:%c 38:%c 40:%c 51:%c 68:%c",
        hardwareBackend.hasI2CAddress( 0x34 ) ? 'Y' : 'N',
        hardwareBackend.hasI2CAddress( 0x35 ) ? 'Y' : 'N',
        hardwareBackend.hasI2CAddress( 0x38 ) ? 'Y' : 'N',
        hardwareBackend.hasI2CAddress( 0x40 ) ? 'Y' : 'N',
        hardwareBackend.hasI2CAddress( 0x51 ) ? 'Y' : 'N',
        hardwareBackend.hasI2CAddress( 0x68 ) ? 'Y' : 'N'
      );

      hardwareCore2I2cInfo.add( signature );
    }
    else {
      hardwareCore2I2cInfo.add( "Not probed" );
    }

    JsonArray displayInfo = user.createNestedArray( "CoreS3 Display" );

    if ( isCore2DiagnosticOnlyMode() ) {
      displayInfo.add( "DIAGNOSTIC ONLY - NOT INITIALIZED" );
    }
    else if (displayReady) {
      char text[32];

      snprintf( text, sizeof(text), "READY (%d x %d)", screenWidth, screenHeight );

      displayInfo.add( text );
    }
    else {
      displayInfo.add( "FAILED" );
    }

    JsonArray touchInfo = user.createNestedArray( "CoreS3 Display Touch" );

    if ( isCore2DiagnosticOnlyMode() ) {
      if ( hardwareBackend.isProbeComplete() ) {
        touchInfo.add(
          hardwareBackend.hasI2CAddress( 0x38 )
            ? "I2C 0x38 DETECTED - NOT INITIALIZED"
            : "I2C 0x38 NOT DETECTED"
        );
      }
      else {
        touchInfo.add( "DIAGNOSTIC PROBE UNAVAILABLE" );
      }
    }
    else {
      touchInfo.add( touchReady ? "READY" : "NOT FOUND" );
    }

    JsonArray wifiInfo = user.createNestedArray( "CoreS3 Display WiFi" );

    if ( WiFi.status() == WL_CONNECTED ) {
      wifiInfo.add( WiFi.localIP().toString() );
    }
    else {
      wifiInfo.add( "Not connected" );
    }

    JsonArray ledInfo = user.createNestedArray( "CoreS3 Display LED" );

    ledInfo.add( bri > 0 ? "ON" : "OFF" );

    JsonArray brightnessInfo = user.createNestedArray( "CoreS3 Display Brightness" );

    brightnessInfo.add( bri );

    JsonArray effectInfo = user.createNestedArray( "CoreS3 Display Effect" );

    if ( strip.getSegmentsNum() > 0 ) {
      char effectName[64];

      getEffectName( strip.getMainSegment().mode, effectName, sizeof(effectName) );

      effectInfo.add( effectName );
    }
    else {
      effectInfo.add( "No segment" );
    }

    JsonArray speedInfo = user.createNestedArray( "CoreS3 Effect Speed" );

    if ( strip.getSegmentsNum() > 0 ) {
      speedInfo.add( getCurrentSpeed() );
    }
    else {
      speedInfo.add( "No segment" );
    }

    JsonArray intensityInfo = user.createNestedArray( "CoreS3 Effect Intensity" );

    if ( strip.getSegmentsNum() > 0 ) {
      intensityInfo.add( getCurrentIntensity() );
    }
    else {
      intensityInfo.add( "No segment" );
    }

    JsonArray paletteInfo = user.createNestedArray( "CoreS3 Effect Palette" );

    if ( strip.getSegmentsNum() > 0 ) {
      char paletteName[64];

      getPaletteName( getCurrentPalette(), paletteName, sizeof(paletteName) );

      paletteInfo.add( paletteName );
    }
    else {
      paletteInfo.add( "No segment" );
    }

    JsonArray paletteIdInfo = user.createNestedArray( "CoreS3 Effect Palette ID" );

    if ( strip.getSegmentsNum() > 0 ) {
      paletteIdInfo.add( getCurrentPalette() );
    }
    else {
      paletteIdInfo.add( "No segment" );
    }

    JsonArray paletteTouchInfo = user.createNestedArray( "CoreS3 Palette Touch Area" );

    paletteTouchInfo.add( "Expanded" );

    JsonArray presetInfo = user.createNestedArray( "CoreS3 Display Preset" );

    presetInfo.add( currentPreset > 0 ? "Active Preset" : "Custom State" );

    JsonArray presetIdInfo = user.createNestedArray( "CoreS3 Display Preset ID" );

    presetIdInfo.add( currentPreset );

    JsonArray bootPresetInfo = user.createNestedArray( "CoreS3 Boot Preset" );

    if ( bootPreset == 0 ) {
      bootPresetInfo.add( "NONE" );
    }
    else {
      bootPresetInfo.add( bootPreset );
    }

    JsonArray presetTouchInfo = user.createNestedArray( "CoreS3 Preset Touch Area" );

    presetTouchInfo.add( "Expanded" );

    JsonArray presetCacheInfo = user.createNestedArray( "CoreS3 Preset Cache" );

    if ( presetCacheReady ) {
      char cacheText[32];

      snprintf( cacheText, sizeof(cacheText), "READY (%u)", (unsigned)presetCacheCount );

      presetCacheInfo.add( cacheText );
    }
    else if ( presetCacheBuilding ) {
      // -----------------------------------------------------
      // Do not expose the internal 1..250 scan position.
      // -----------------------------------------------------

      presetCacheInfo.add( "LOADING" );
    }
    else {
      presetCacheInfo.add( "NOT READY" );
    }

    JsonArray colorInfo = user.createNestedArray( "CoreS3 Display Color" );

    if ( strip.getSegmentsNum() > 0 ) {
      uint32_t color = getPrimaryColor();

      char colorText[16];

      snprintf( colorText, sizeof(colorText), "#%02X%02X%02X", R(color), G(color), B(color) );

      colorInfo.add( colorText );
    }
    else {
      colorInfo.add( "No segment" );
    }

    JsonArray hueInfo = user.createNestedArray( "CoreS3 Display Hue" );

    if ( strip.getSegmentsNum() > 0 ) {
      hueInfo.add( getDisplayedHue() );
    }
    else {
      hueInfo.add( "No segment" );
    }

    JsonArray saturationInfo = user.createNestedArray( "CoreS3 Display Saturation" );

    if ( strip.getSegmentsNum() > 0 ) {
      saturationInfo.add( getDisplayedSaturation() );
    }
    else {
      saturationInfo.add( "No segment" );
    }

    JsonArray pageInfo = user.createNestedArray( "CoreS3 Display Page" );

    if ( currentPage == SCREEN_MAIN ) {
      pageInfo.add( "MAIN" );
    }
    else if ( currentPage == SCREEN_COLOR ) {
      pageInfo.add( "COLOR" );
    }
    else if ( currentPage == SCREEN_EFFECT ) {
      pageInfo.add( "EFFECT" );
    }
    else {
      pageInfo.add( "PRESET" );
    }

    JsonArray powerStateInfo = user.createNestedArray( "CoreS3 Display Power State" );

    switch ( displayPowerState ) {
    case DISPLAY_POWER_ACTIVE:
      powerStateInfo.add( "ACTIVE" );
      break;

    case DISPLAY_POWER_SLEEP_FADE_OUT:
      powerStateInfo.add( "SLEEP FADE OUT" );
      break;

    case DISPLAY_POWER_SLEEPING:
      powerStateInfo.add( "SLEEPING" );
      break;

    case DISPLAY_POWER_WAKE_FADE_IN:
      powerStateInfo.add( "WAKE FADE IN" );
      break;

    case DISPLAY_POWER_WAKE_WAIT_RELEASE:
      powerStateInfo.add( "WAKE WAIT RELEASE" );
      break;
    }

    JsonArray lcdBrightnessInfo = user.createNestedArray( "CoreS3 LCD Brightness" );

    lcdBrightnessInfo.add( lcdBrightness );

    JsonArray sleepInfo = user.createNestedArray( "CoreS3 Display Sleep" );

    if ( sleepTimeoutSec == 0 ) {
      sleepInfo.add( "Never" );
    }
    else {
      char sleepText[24];

      snprintf( sleepText, sizeof(sleepText), "%u sec", sleepTimeoutSec );

      sleepInfo.add( sleepText );
    }

    JsonArray fadeInfo = user.createNestedArray( "CoreS3 Display Fade" );

    if (!fadeEnabled) {
      fadeInfo.add( "OFF" );
    }
    else {
      char fadeText[24];

      snprintf( fadeText, sizeof(fadeText), "ON (%u ms)", fadeDurationMs );

      fadeInfo.add( fadeText );
    }

  }
};

static CoreS3DisplayUsermod coreS3DisplayUsermod;

REGISTER_USERMOD(coreS3DisplayUsermod);
