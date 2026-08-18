#include "wled.h"
#include <Wire.h>
#include <M5GFX.h>
#include <driver/i2c.h>

class CoreS3PowerUsermod : public Usermod
{
private:
  // ------------------------------------------------------------
  // M5Stack CoreS3 internal I2C devices
  // ------------------------------------------------------------
  static constexpr uint8_t AW9523B_ADDR = 0x58;
  static constexpr uint8_t AXP2101_ADDR = 0x34;

  // ------------------------------------------------------------
  // CoreS3 internal runtime I2C ownership
  //
  // During early WLED/usermod setup, the existing power-enable path uses
  // Arduino Wire successfully on GPIO12/GPIO11.
  //
  // After CoreS3 Display/Touch initializes, the stable project design uses
  // M5GFX I2C_NUM_1 as the runtime owner of those same internal pins.
  // Therefore physical power-key polling must use the M5GFX transaction API
  // and must not keep using Arduino Wire from loop().
  // ------------------------------------------------------------
  static constexpr i2c_port_t CORES3_INTERNAL_I2C_PORT = I2C_NUM_1;
  static constexpr uint32_t CORES3_INTERNAL_I2C_FREQUENCY = 400000;

  // ------------------------------------------------------------
  // AXP2101 power-key IRQ registers
  //
  // CoreS3 does not route the AXP2101 IRQ pin to the ESP32.
  // Therefore the power-key edge/status flags are polled over I2C.
  //
  // IRQ Enable 1 : 0x41
  // IRQ Status 1 : 0x49
  //
  // Bit0 : Power-key positive edge  (release)
  // Bit1 : Power-key negative edge  (press)
  // Bit2 : Power-key long press
  // Bit3 : Power-key short press
  //
  // IRQ status bits are write-1-to-clear.
  // ------------------------------------------------------------
  static constexpr uint8_t AXP2101_REG_IRQ_ENABLE_1 = 0x41;
  static constexpr uint8_t AXP2101_REG_IRQ_STATUS_1 = 0x49;

  static constexpr uint8_t AXP2101_PKEY_POSITIVE_MASK = 0x01;
  static constexpr uint8_t AXP2101_PKEY_NEGATIVE_MASK = 0x02;
  static constexpr uint8_t AXP2101_PKEY_LONG_MASK     = 0x04;
  static constexpr uint8_t AXP2101_PKEY_SHORT_MASK    = 0x08;
  static constexpr uint8_t AXP2101_PKEY_EVENT_MASK    = 0x0F;

  // Enable press/release/long events. We poll the status register;
  // no physical IRQ input to ESP32 is required.
  static constexpr uint8_t AXP2101_PKEY_IRQ_ENABLE_MASK =
    AXP2101_PKEY_POSITIVE_MASK |
    AXP2101_PKEY_NEGATIVE_MASK |
    AXP2101_PKEY_LONG_MASK;

  // ------------------------------------------------------------
  // Physical power-key safe-shutdown timing
  //
  // We deliberately do NOT change AXP2101 register 0x27 here.
  // M5Unified configures CoreS3 with:
  //   Long Press notification ~= 1 second
  //   Hardware Power Off     ~= 4 seconds
  //
  // Primary trigger:
  //   AXP2101 Long Press IRQ status bit (0x49 bit2)
  //
  // Fallback:
  //   If the negative-edge PRESS event was observed but, for any reason,
  //   the Long IRQ is missed, blank after 1.5 seconds of the software
  //   press timer. The fallback is never used unless a PRESS edge was
  //   actually observed.
  //
  // If the user releases the key before hardware power-off, restore
  // the current logical WLED brightness and resume rendering.
  // ------------------------------------------------------------
  static constexpr unsigned long POWER_KEY_POLL_INTERVAL_MS = 20;
  static constexpr unsigned long SAFE_SHUTDOWN_FALLBACK_HOLD_MS = 1500;
  static constexpr unsigned long SAFE_SHUTDOWN_BLACK_REFRESH_MS = 100;
  static constexpr unsigned long SAFE_SHUTDOWN_SHOW_WAIT_MS = 150;

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
  // Physical power-key / safe-shutdown runtime state
  // ------------------------------------------------------------
  bool powerKeyMonitorReady = false;
  bool runtimePowerKeyMonitorAttempted = false;
  bool runtimePowerKeyBusReadyLogged = false;
  bool powerKeyPressed = false;

  bool safeShutdownBlankActive = false;
  bool safeShutdownEverTriggered = false;
  bool safeShutdownLastCanceled = false;

  uint8_t axpIrqEnableBefore = 0;
  uint8_t axpIrqEnableAfter = 0;
  uint8_t lastPowerKeyStatus = 0;
  uint8_t lastSafeShutdownTriggerStatus = 0;

  // Save WLED's logical brightness for cancel/recovery. strip.getBrightness()
  // is an internal output brightness and may already be gamma-corrected.
  uint8_t savedLogicalBrightness = 0;

  unsigned long powerKeyPressedAt = 0;
  unsigned long lastPowerKeyPoll = 0;
  unsigned long lastShutdownBlackRefresh = 0;
  unsigned long lastRuntimeI2CFailureLog = 0;

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
  // Runtime CoreS3 internal I2C helpers
  //
  // IMPORTANT:
  // These are used only after all usermod setup() calls have completed.
  // At that point CoreS3 Display/Touch owns the internal GPIO12/GPIO11
  // bus through M5GFX I2C_NUM_1.
  //
  // Do NOT call Wire.begin(), lgfx::i2c::init(), release(), or install a
  // second runtime driver here. Use the already-active M5GFX owner.
  // ------------------------------------------------------------
  bool readRuntimeRegister(uint8_t address, uint8_t reg, uint8_t& value)
  {
    auto result = lgfx::i2c::transactionWriteRead(
      CORES3_INTERNAL_I2C_PORT,
      address,
      &reg,
      1,
      &value,
      1,
      CORES3_INTERNAL_I2C_FREQUENCY
    );

    return result.has_value();
  }

  bool writeRuntimeRegister(uint8_t address, uint8_t reg, uint8_t value)
  {
    const uint8_t data[2] = { reg, value };

    auto result = lgfx::i2c::transactionWrite(
      CORES3_INTERNAL_I2C_PORT,
      address,
      data,
      sizeof(data),
      CORES3_INTERNAL_I2C_FREQUENCY
    );

    return result.has_value();
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

  // ------------------------------------------------------------
  // Arm AXP2101 power-key polling on the RUNTIME M5GFX-owned I2C bus.
  //
  // CoreS3's AXP2101 IRQ output does not reach an ESP32 GPIO, but the PMIC
  // still latches the power-key status in register 0x49. We therefore poll
  // that register over I2C_NUM_1.
  //
  // This initialization is intentionally deferred to loop(), after the
  // Display/Touch backend has initialized M5GFX I2C_NUM_1.
  // ------------------------------------------------------------
  bool configureRuntimePowerKeyMonitor()
  {
    runtimePowerKeyMonitorAttempted = true;

    uint8_t chipId = 0;

    if (!readRuntimeRegister(AXP2101_ADDR, 0x03, chipId)) {
      Serial.println(
        F("[CoreS3_Power] Runtime power key: M5GFX I2C1 AXP2101 read FAILED")
      );

      return false;
    }

    if (chipId != 0x4A) {
      Serial.printf(
        "[CoreS3_Power] Runtime power key: unexpected AXP2101 chip ID 0x%02X\n",
        chipId
      );

      return false;
    }

    if (!runtimePowerKeyBusReadyLogged) {
      runtimePowerKeyBusReadyLogged = true;

      Serial.printf(
        "[CoreS3_Power] Runtime I2C: M5GFX I2C_NUM_1 AXP2101 READY (ID=0x%02X)\n",
        chipId
      );
    }

    if (!readRuntimeRegister(
          AXP2101_ADDR,
          AXP2101_REG_IRQ_ENABLE_1,
          axpIrqEnableBefore
        )) {
      return false;
    }

    uint8_t newIrqEnable =
      axpIrqEnableBefore | AXP2101_PKEY_IRQ_ENABLE_MASK;

    if (!writeRuntimeRegister(
          AXP2101_ADDR,
          AXP2101_REG_IRQ_ENABLE_1,
          newIrqEnable
        )) {
      return false;
    }

    if (!readRuntimeRegister(
          AXP2101_ADDR,
          AXP2101_REG_IRQ_ENABLE_1,
          axpIrqEnableAfter
        )) {
      return false;
    }

    if (
      (axpIrqEnableAfter & AXP2101_PKEY_IRQ_ENABLE_MASK) !=
      AXP2101_PKEY_IRQ_ENABLE_MASK
    ) {
      return false;
    }

    uint8_t staleStatus = 0;

    if (readRuntimeRegister(
          AXP2101_ADDR,
          AXP2101_REG_IRQ_STATUS_1,
          staleStatus
        )) {
      uint8_t stalePowerKeyFlags =
        staleStatus & AXP2101_PKEY_EVENT_MASK;

      if (stalePowerKeyFlags != 0) {
        writeRuntimeRegister(
          AXP2101_ADDR,
          AXP2101_REG_IRQ_STATUS_1,
          stalePowerKeyFlags
        );
      }
    }

    powerKeyPressed = false;
    powerKeyPressedAt = 0;
    lastPowerKeyPoll = millis();

    Serial.printf(
      "[CoreS3_Power] Runtime PKEY IRQEN1: 0x%02X -> 0x%02X\n",
      axpIrqEnableBefore,
      axpIrqEnableAfter
    );

    Serial.println(
      F("[CoreS3_Power] Runtime power key monitor: ARMED on M5GFX I2C1")
    );

    return true;
  }

  // ------------------------------------------------------------
  // Wait briefly for an asynchronous LED transfer to finish.
  //
  // This is intentionally bounded. We must never block indefinitely
  // while the PMIC hardware shutdown timer is running.
  // ------------------------------------------------------------
  void waitForLedOutputComplete()
  {
    unsigned long waitStart = millis();

    while (
      strip.isUpdating() &&
      millis() - waitStart < SAFE_SHUTDOWN_SHOW_WAIT_MS
    ) {
      delay(1);
    }
  }

  // ------------------------------------------------------------
  // Force the physical LED output black before CoreS3 hard-off.
  //
  // The ordering here is deliberate:
  //
  //   1. wait for any current strip.service() execution
  //   2. set physical strip brightness to 0
  //   3. call strip.show() while output is still active
  //   4. wait for the asynchronous bus transfer
  //   5. only then suspend future effect service
  //
  // WLED's setBrightness(..., true) contract expects the caller to
  // call show() when immediate physical output is required.
  //
  // WLED logical state (bri/briLast/preset/effect/color) is untouched.
  // ------------------------------------------------------------
  void beginSafeShutdownBlank(uint8_t triggerStatus)
  {
    if (safeShutdownBlankActive) {
      return;
    }

    safeShutdownLastCanceled = false;
    safeShutdownEverTriggered = true;
    lastSafeShutdownTriggerStatus = triggerStatus;

    savedLogicalBrightness = bri;

    Serial.printf(
      "[CoreS3_Power] Safe shutdown trigger: IRQ=0x%02X%s%s\n",
      triggerStatus,
      (triggerStatus & AXP2101_PKEY_LONG_MASK) ? " LONG" : "",
      (triggerStatus & AXP2101_PKEY_NEGATIVE_MASK) ? " PRESS" : ""
    );

    Serial.printf(
      "[CoreS3_Power] Safe shutdown: WLED bri=%u, strip=%u -> physical 0\n",
      savedLogicalBrightness,
      strip.getBrightness()
    );

    // Do not race strip.service() while it is drawing a frame.
    strip.waitForIt();

    // IMPORTANT: Send the zero-brightness frame BEFORE suspend().
    // setBrightness(..., true) updates BusManager brightness but does not
    // itself request an immediate effect redraw; show() performs the bus send.
    strip.setBrightness(0, true);
    strip.show();

    waitForLedOutputComplete();

    // Prevent a later effect-service frame from replacing the shutdown BLACK.
    strip.suspend();
    strip.waitForIt();

    safeShutdownBlankActive = true;
    lastShutdownBlackRefresh = millis();

    Serial.println(
      F("[CoreS3_Power] Safe shutdown: BLACK frame sent and strip suspended")
    );
  }

  // ------------------------------------------------------------
  // Keep the physical output at zero until PMIC hard-off.
  //
  // Normally strip.suspend() is enough. Reasserting brightness 0 is
  // cheap insurance against another subsystem changing strip output
  // while the physical power key remains held.
  // ------------------------------------------------------------
  void maintainSafeShutdownBlank(unsigned long now)
  {
    if (!safeShutdownBlankActive || !powerKeyPressed) {
      return;
    }

    if (
      now - lastShutdownBlackRefresh <
      SAFE_SHUTDOWN_BLACK_REFRESH_MS
    ) {
      return;
    }

    lastShutdownBlackRefresh = now;

    strip.setBrightness(0, true);
    strip.show();

    waitForLedOutputComplete();
  }

  // ------------------------------------------------------------
  // User released the physical key before PMIC power-off.
  //
  // Restore the exact strip output brightness captured before blanking
  // and resume the effect engine. WLED's logical state was never
  // modified, so no preset/color/effect restoration is required.
  // ------------------------------------------------------------
  void cancelSafeShutdownBlank()
  {
    if (!safeShutdownBlankActive) {
      return;
    }

    Serial.println(
      F("[CoreS3_Power] Safe shutdown canceled: restoring LED output")
    );

    // Restore service first, then restore from WLED's logical brightness.
    // This avoids feeding an already gamma-corrected strip brightness back
    // through setBrightness() a second time.
    strip.resume();

    uint8_t restoreBrightness = bri;

    // bri should normally be unchanged. Keep the captured value as a safe
    // fallback if another subsystem changed bri to zero during the hold.
    if (restoreBrightness == 0 && savedLogicalBrightness > 0) {
      restoreBrightness = savedLogicalBrightness;
    }

    strip.setBrightness(restoreBrightness, true);
    strip.show();

    waitForLedOutputComplete();

    safeShutdownBlankActive = false;
    safeShutdownLastCanceled = true;

    strip.trigger();

    Serial.printf(
      "[CoreS3_Power] Safe shutdown canceled: restored bri=%u\n",
      restoreBrightness
    );
  }

  // ------------------------------------------------------------
  // Poll AXP2101 power-key status.
  //
  // Primary safe-shutdown trigger is the PMIC's own Long Press event.
  // It is intentionally independent of whether the earlier negative-edge
  // PRESS event was seen by this polling loop.
  //
  // PWRON is active-low:
  //   negative edge -> key pressed
  //   positive edge -> key released
  //   long press    -> PMIC long-press threshold reached
  //
  // IRQ Status 1 is write-1-to-clear.
  // ------------------------------------------------------------
  void servicePhysicalPowerKey()
  {
    unsigned long now = millis();

    if (!powerKeyMonitorReady) {
      // All usermod setup() calls are complete before loop() starts, so the
      // CoreS3 Display backend has already established M5GFX I2C_NUM_1.
      //
      // Retry if the first transaction happens during a transient bus use.
      if (
        !runtimePowerKeyMonitorAttempted ||
        now - lastRuntimeI2CFailureLog >= 1000
      ) {
        if (configureRuntimePowerKeyMonitor()) {
          powerKeyMonitorReady = true;
        }
        else {
          lastRuntimeI2CFailureLog = now;
        }
      }

      return;
    }

    if (
      now - lastPowerKeyPoll <
      POWER_KEY_POLL_INTERVAL_MS
    ) {
      maintainSafeShutdownBlank(now);
      return;
    }

    lastPowerKeyPoll = now;

    uint8_t status = 0;

    if (!readRuntimeRegister(
          AXP2101_ADDR,
          AXP2101_REG_IRQ_STATUS_1,
          status
        )) {
      if (now - lastRuntimeI2CFailureLog >= 1000) {
        lastRuntimeI2CFailureLog = now;

        Serial.println(
          F("[CoreS3_Power] Runtime PKEY status read FAILED on M5GFX I2C1")
        );
      }

      maintainSafeShutdownBlank(now);
      return;
    }

    uint8_t powerKeyStatus =
      status & AXP2101_PKEY_EVENT_MASK;

    if (powerKeyStatus != 0) {
      lastPowerKeyStatus = powerKeyStatus;

      Serial.printf(
        "[CoreS3_Power] PKEY IRQ: 0x%02X%s%s%s%s\n",
        powerKeyStatus,
        (powerKeyStatus & AXP2101_PKEY_POSITIVE_MASK) ? " RELEASE" : "",
        (powerKeyStatus & AXP2101_PKEY_NEGATIVE_MASK) ? " PRESS" : "",
        (powerKeyStatus & AXP2101_PKEY_LONG_MASK) ? " LONG" : "",
        (powerKeyStatus & AXP2101_PKEY_SHORT_MASK) ? " SHORT" : ""
      );

      // Clear exactly the flags we observed.
      writeRuntimeRegister(
        AXP2101_ADDR,
        AXP2101_REG_IRQ_STATUS_1,
        powerKeyStatus
      );
    }

    // PRESS edge is useful for diagnostics and as a fallback timer source.
    if (
      powerKeyStatus & AXP2101_PKEY_NEGATIVE_MASK
    ) {
      powerKeyPressed = true;
      powerKeyPressedAt = now;
    }

    // PMIC LONG is the authoritative shutdown warning.
    // Do this BEFORE release processing so a LONG event can always send
    // the final BLACK frame even if multiple status bits accumulated.
    if (
      (powerKeyStatus & AXP2101_PKEY_LONG_MASK) &&
      !safeShutdownBlankActive
    ) {
      powerKeyPressed = true;

      if (powerKeyPressedAt == 0) {
        powerKeyPressedAt = now;
      }

      beginSafeShutdownBlank(powerKeyStatus);
    }

    // Secondary fallback if PRESS was observed but LONG status was not.
    if (
      powerKeyPressed &&
      !safeShutdownBlankActive &&
      powerKeyPressedAt > 0 &&
      now - powerKeyPressedAt >= SAFE_SHUTDOWN_FALLBACK_HOLD_MS
    ) {
      Serial.println(
        F("[CoreS3_Power] Safe shutdown: PRESS timer fallback")
      );

      beginSafeShutdownBlank(
        AXP2101_PKEY_NEGATIVE_MASK
      );
    }

    // RELEASE means the user aborted before PMIC hard-off.
    if (
      powerKeyStatus & AXP2101_PKEY_POSITIVE_MASK
    ) {
      powerKeyPressed = false;
      powerKeyPressedAt = 0;

      cancelSafeShutdownBlank();

      return;
    }

    maintainSafeShutdownBlank(now);
  }

public:
  // ------------------------------------------------------------
  // WLED Usermod setup
  // ------------------------------------------------------------
  void setup() override
  {
    Serial.println();
    Serial.println(F("[CoreS3_Power] Power Enable test start"));

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

    // --------------------------------------------------------
    // Physical power-key monitoring is intentionally NOT started here.
    //
    // CoreS3_Display initializes the runtime internal bus using M5GFX
    // I2C_NUM_1 later in the usermod setup sequence. Power-key polling is
    // armed from loop() after that bus owner is active.
    // --------------------------------------------------------
    powerKeyMonitorReady = false;
    runtimePowerKeyMonitorAttempted = false;

    Serial.println(
      F("[CoreS3_Power] Power key monitor: DEFERRED until M5GFX I2C1 is active")
    );

    Serial.println(
      F("[CoreS3_Power] Safe shutdown: AXP2101 LONG IRQ primary trigger")
    );

    Serial.printf(
      "[CoreS3_Power] Safe shutdown: PRESS fallback >= %lu ms\n",
      SAFE_SHUTDOWN_FALLBACK_HOLD_MS
    );

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

    Serial.println(F("[CoreS3_Power] Power Enable test end"));
    Serial.println();
  }

  // ------------------------------------------------------------
  // Poll the CoreS3 physical power key.
  //
  // AXP2101's IRQ pin is not routed to the ESP32 on CoreS3, so this
  // lightweight I2C poll is the only firmware-visible path needed here.
  // ------------------------------------------------------------
  void loop() override
  {
    servicePhysicalPowerKey();
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

    // Physical power-key safe shutdown
    JsonArray shutdownInfo =
      user.createNestedArray("CoreS3 Safe Shutdown");

    if (!powerKeyMonitorReady) {
      shutdownInfo.add(
        runtimePowerKeyMonitorAttempted
          ? "Runtime M5GFX I2C1 monitor unavailable"
          : "Runtime M5GFX I2C1 monitor pending"
      );
    } else if (safeShutdownBlankActive) {
      shutdownInfo.add("BLACK output active - waiting for PMIC off");
    } else if (safeShutdownLastCanceled) {
      shutdownInfo.add("ARMED - last shutdown hold canceled");
    } else if (safeShutdownEverTriggered) {
      shutdownInfo.add("ARMED - shutdown BLACK previously triggered");
    } else {
      shutdownInfo.add("ARMED");
    }

    shutdownInfo.add("Trigger: AXP2101 Long Press IRQ");

    char fallbackText[48];
    snprintf(
      fallbackText,
      sizeof(fallbackText),
      "PRESS fallback: %lu ms",
      SAFE_SHUTDOWN_FALLBACK_HOLD_MS
    );

    shutdownInfo.add(fallbackText);

    char lastIrqText[40];
    snprintf(
      lastIrqText,
      sizeof(lastIrqText),
      "Last IRQ status: 0x%02X",
      lastPowerKeyStatus
    );

    shutdownInfo.add(lastIrqText);

    char triggerIrqText[40];
    snprintf(
      triggerIrqText,
      sizeof(triggerIrqText),
      "Last BLACK trigger: 0x%02X",
      lastSafeShutdownTriggerStatus
    );

    shutdownInfo.add(triggerIrqText);

    char irqText[48];
    snprintf(
      irqText,
      sizeof(irqText),
      "IRQEN1 0x%02X -> 0x%02X",
      axpIrqEnableBefore,
      axpIrqEnableAfter
    );

    shutdownInfo.add(irqText);

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
