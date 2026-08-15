#include "wled.h"
#include <Wire.h>

// ===========================================================
// M5Stack CoreS3 Power Usermod
//
// Responsibilities
//   - Verify the CoreS3 internal I2C pin assignment.
//   - Detect the AW9523B I/O expander and AXP2101 PMU.
//   - Apply the CoreS3 AW9523B configuration used for external power.
//   - Enable BOOST_EN followed by BUS_EN for the external 5V ports.
//   - Preserve unrelated AW9523B output bits.
//   - Verify the resulting register state and report it to WLED Info.
//
// This usermod owns only CoreS3 external 5V power enablement.
// It does not manage Display, Touch, audio codec configuration,
// LED data output, or periodic power recovery.
// ===========================================================

class CoreS3PowerUsermod : public Usermod
{
private:
  // ------------------------------------------------------------
  // M5Stack CoreS3 internal I2C devices
  // ------------------------------------------------------------
  static constexpr uint8_t AW9523B_ADDR = 0x58;
  static constexpr uint8_t AXP2101_ADDR = 0x34;

  // ------------------------------------------------------------
  // AW9523B registers used by CoreS3
  // ------------------------------------------------------------
  static constexpr uint8_t REG_OUTPUT_P0 = 0x02;
  static constexpr uint8_t REG_OUTPUT_P1 = 0x03;
  static constexpr uint8_t REG_CONFIG_P0 = 0x04;
  static constexpr uint8_t REG_CONFIG_P1 = 0x05;
  static constexpr uint8_t REG_GCR       = 0x11;
  static constexpr uint8_t REG_LEDMODE_P0 = 0x12;
  static constexpr uint8_t REG_LEDMODE_P1 = 0x13;

  // ------------------------------------------------------------
  // CoreS3 power control bits
  // ------------------------------------------------------------
  static constexpr uint8_t BUS_EN_MASK   = 0x02;  // P0 bit1
  static constexpr uint8_t BOOST_EN_MASK = 0x80;  // P1 bit7

  // ------------------------------------------------------------
  // CoreS3 official AW9523B configuration
  // Values used by M5GFX for CoreS3 initialization
  // ------------------------------------------------------------
  static constexpr uint8_t CORE_S3_CONFIG_P0  = 0x18;
  static constexpr uint8_t CORE_S3_CONFIG_P1  = 0x0C;
  static constexpr uint8_t CORE_S3_GCR        = 0x10;
  static constexpr uint8_t CORE_S3_LEDMODE_P0 = 0xFF;
  static constexpr uint8_t CORE_S3_LEDMODE_P1 = 0xFF;

  bool aw9523Found = false;
  bool axp2101Found = false;

  bool powerEnableAttempted = false;
  bool powerEnableSuccess = false;

  bool busEnabled = false;
  bool boostEnabled = false;

  uint8_t p0Before = 0;
  uint8_t p1Before = 0;
  uint8_t p0After = 0;
  uint8_t p1After = 0;

  // ------------------------------------------------------------
  // Probe an I2C address
  // ------------------------------------------------------------
  bool probeI2C(uint8_t address)
  {
    Wire.beginTransmission(address);
    return (Wire.endTransmission() == 0);
  }

  // ------------------------------------------------------------
  // Read one I2C register
  // ------------------------------------------------------------
  bool readRegister(uint8_t address, uint8_t reg, uint8_t &value)
  {
    Wire.beginTransmission(address);
    Wire.write(reg);

    if (Wire.endTransmission(false) != 0) {
      return false;
    }

    uint8_t count = Wire.requestFrom(address, (uint8_t)1);

    if (count != 1 || !Wire.available()) {
      return false;
    }

    value = Wire.read();
    return true;
  }

  // ------------------------------------------------------------
  // Write one I2C register
  // ------------------------------------------------------------
  bool writeRegister(uint8_t address, uint8_t reg, uint8_t value)
  {
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(value);

    return (Wire.endTransmission() == 0);
  }

  // ------------------------------------------------------------
  // Configure AW9523B for M5Stack CoreS3
  //
  // These values are based on the official M5GFX CoreS3
  // initialization.
  // ------------------------------------------------------------
  bool configureAW9523()
  {
    bool ok = true;

    ok &= writeRegister(
      AW9523B_ADDR,
      REG_CONFIG_P0,
      CORE_S3_CONFIG_P0
    );

    ok &= writeRegister(
      AW9523B_ADDR,
      REG_CONFIG_P1,
      CORE_S3_CONFIG_P1
    );

    ok &= writeRegister(
      AW9523B_ADDR,
      REG_GCR,
      CORE_S3_GCR
    );

    ok &= writeRegister(
      AW9523B_ADDR,
      REG_LEDMODE_P0,
      CORE_S3_LEDMODE_P0
    );

    ok &= writeRegister(
      AW9523B_ADDR,
      REG_LEDMODE_P1,
      CORE_S3_LEDMODE_P1
    );

    return ok;
  }

  // ------------------------------------------------------------
  // Enable CoreS3 external 5V BUS
  //
  // BOOST_EN : AW9523B P1 bit7
  // BUS_EN   : AW9523B P0 bit1
  //
  // Existing register bits are preserved.
  // ------------------------------------------------------------
  bool enableExternal5V()
  {
    powerEnableAttempted = true;

    // Read current AW9523B output registers.
    if (!readRegister(AW9523B_ADDR, REG_OUTPUT_P0, p0Before)) {
      return false;
    }

    if (!readRegister(AW9523B_ADDR, REG_OUTPUT_P1, p1Before)) {
      return false;
    }

    // Configure AW9523B as expected by CoreS3.
    if (!configureAW9523()) {
      return false;
    }

    // --------------------------------------------------------
    // Enable BOOST first.
    // Preserve all other P1 bits.
    // --------------------------------------------------------
    uint8_t newP1 = p1Before | BOOST_EN_MASK;

    if (!writeRegister(AW9523B_ADDR, REG_OUTPUT_P1, newP1)) {
      return false;
    }

    delay(10);

    // --------------------------------------------------------
    // Enable external BUS.
    // Preserve all other P0 bits.
    // --------------------------------------------------------
    uint8_t newP0 = p0Before | BUS_EN_MASK;

    if (!writeRegister(AW9523B_ADDR, REG_OUTPUT_P0, newP0)) {
      return false;
    }

    delay(10);

    // --------------------------------------------------------
    // Read back registers and verify.
    // --------------------------------------------------------
    if (!readRegister(AW9523B_ADDR, REG_OUTPUT_P0, p0After)) {
      return false;
    }

    if (!readRegister(AW9523B_ADDR, REG_OUTPUT_P1, p1After)) {
      return false;
    }

    busEnabled   = (p0After & BUS_EN_MASK) != 0;
    boostEnabled = (p1After & BOOST_EN_MASK) != 0;

    return busEnabled && boostEnabled;
  }

public:
  // ------------------------------------------------------------
  // WLED Usermod setup
  // ------------------------------------------------------------
  void setup() override
  {
    Serial.println();
    Serial.println(F("[CoreS3_Power] Initialization start"));

    Serial.printf(
      "[CoreS3_Power] I2C SDA=%d SCL=%d\n",
      i2c_sda,
      i2c_scl
    );

    // --------------------------------------------------------
    // Safety check:
    // Only operate when WLED uses the CoreS3 internal I2C pins.
    // --------------------------------------------------------
    if (i2c_sda != 12 || i2c_scl != 11) {
      Serial.println(
        F("[CoreS3_Power] ERROR: Invalid CoreS3 I2C pins")
      );
      return;
    }

    // --------------------------------------------------------
    // Detect internal devices.
    // --------------------------------------------------------
    aw9523Found = probeI2C(AW9523B_ADDR);
    axp2101Found = probeI2C(AXP2101_ADDR);

    Serial.printf(
      "[CoreS3_Power] AW9523B (0x58): %s\n",
      aw9523Found ? "FOUND" : "NOT FOUND"
    );

    Serial.printf(
      "[CoreS3_Power] AXP2101 (0x34): %s\n",
      axp2101Found ? "FOUND" : "NOT FOUND"
    );

    // --------------------------------------------------------
    // Do not touch power unless both expected CoreS3 devices
    // are present.
    // --------------------------------------------------------
    if (!aw9523Found || !axp2101Found) {
      Serial.println(
        F("[CoreS3_Power] Power enable canceled")
      );
      return;
    }

    powerEnableSuccess = enableExternal5V();

    Serial.printf(
      "[CoreS3_Power] BUS_EN: %s\n",
      busEnabled ? "ON" : "OFF"
    );

    Serial.printf(
      "[CoreS3_Power] BOOST_EN: %s\n",
      boostEnabled ? "ON" : "OFF"
    );

    Serial.printf(
      "[CoreS3_Power] External 5V: %s\n",
      powerEnableSuccess ? "ENABLED" : "FAILED"
    );

    Serial.printf(
      "[CoreS3_Power] AW9523 P0: 0x%02X -> 0x%02X\n",
      p0Before,
      p0After
    );

    Serial.printf(
      "[CoreS3_Power] AW9523 P1: 0x%02X -> 0x%02X\n",
      p1Before,
      p1After
    );

    Serial.println(F("[CoreS3_Power] Initialization complete"));
    Serial.println();
  }

  // ------------------------------------------------------------
  // No periodic work is required after initialization.
  // ------------------------------------------------------------
  void loop() override
  {
  }

  // ------------------------------------------------------------
  // Add status to WLED Info page.
  // ------------------------------------------------------------
  void addToJsonInfo(JsonObject& root) override
  {
    JsonObject user = root["u"];

    if (user.isNull()) {
      user = root.createNestedObject("u");
    }

    // I2C
    JsonArray i2cInfo = user.createNestedArray("CoreS3 I2C");

    if (i2c_sda == 12 && i2c_scl == 11) {
      i2cInfo.add("GPIO12 / GPIO11 OK");
    } else {
      i2cInfo.add("I2C PIN ERROR");
    }

    // AW9523B
    JsonArray awInfo = user.createNestedArray("CoreS3 AW9523B");

    if (aw9523Found) {
      awInfo.add("Found (0x58)");
    } else {
      awInfo.add("Not found");
    }

    // AXP2101
    JsonArray axpInfo = user.createNestedArray("CoreS3 AXP2101");

    if (axp2101Found) {
      axpInfo.add("Found (0x34)");
    } else {
      axpInfo.add("Not found");
    }

    // Power
    JsonArray powerInfo = user.createNestedArray("CoreS3 Ext 5V");

    if (!powerEnableAttempted) {
      powerInfo.add("Not attempted");
    } else if (powerEnableSuccess) {
      powerInfo.add("ENABLED");
    } else {
      powerInfo.add("FAILED");
    }

    // BUS_EN
    JsonArray busInfo = user.createNestedArray("CoreS3 BUS_EN");
    busInfo.add(busEnabled ? "ON" : "OFF");

    // BOOST_EN
    JsonArray boostInfo = user.createNestedArray("CoreS3 BOOST_EN");
    boostInfo.add(boostEnabled ? "ON" : "OFF");

    // AW9523 register values
    char p0Text[32];
    snprintf(
      p0Text,
      sizeof(p0Text),
      "0x%02X -> 0x%02X",
      p0Before,
      p0After
    );

    JsonArray p0Info = user.createNestedArray("CoreS3 AW P0");
    p0Info.add(p0Text);

    char p1Text[32];
    snprintf(
      p1Text,
      sizeof(p1Text),
      "0x%02X -> 0x%02X",
      p1Before,
      p1After
    );

    JsonArray p1Info = user.createNestedArray("CoreS3 AW P1");
    p1Info.add(p1Text);
  }
};

// ------------------------------------------------------------
// Register CoreS3 Power Usermod with WLED
// ------------------------------------------------------------
static CoreS3PowerUsermod coreS3PowerUsermod;
REGISTER_USERMOD(coreS3PowerUsermod);
