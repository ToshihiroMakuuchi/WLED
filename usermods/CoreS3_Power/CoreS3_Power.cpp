#include "wled.h"
#include <Wire.h>
#include <M5GFX.h>
#include <driver/i2c.h>
#include <esp_system.h>

// ============================================================
// Read-only CoreS3 power health bridge
//
// CoreS3_Display consumes these states for user-facing warning UX only.
// Power ownership and all register writes remain inside this usermod.
// ============================================================
static volatile bool coreS3PowerInitializationCompleteState = false;
static volatile bool coreS3PowerExternal5VReadyState = false;
static volatile bool coreS3PowerSafeShutdownMonitorReadyState = false;

extern "C" bool coreS3PowerInitializationComplete()
{
  return coreS3PowerInitializationCompleteState;
}

extern "C" bool coreS3PowerExternal5VReady()
{
  return coreS3PowerExternal5VReadyState;
}

extern "C" bool coreS3PowerSafeShutdownMonitorReady()
{
  return coreS3PowerSafeShutdownMonitorReadyState;
}

class CoreS3PowerUsermod : public Usermod
{
private:
  static constexpr uint8_t AW9523B_ADDR = 0x58;
  static constexpr uint8_t AXP2101_ADDR = 0x34;

  static constexpr i2c_port_t CORES3_INTERNAL_I2C_PORT = I2C_NUM_1;
  static constexpr uint32_t CORES3_INTERNAL_I2C_FREQUENCY = 400000;

  static constexpr uint8_t AXP2101_REG_STATUS1          = 0x00;
  static constexpr uint8_t AXP2101_REG_STATUS2          = 0x01;
  static constexpr uint8_t AXP2101_REG_CHIP_ID          = 0x03;
  static constexpr uint8_t AXP2101_REG_GAUGE_WDT_CTRL   = 0x18;
  static constexpr uint8_t AXP2101_REG_WDT_CTRL         = 0x19;
  static constexpr uint8_t AXP2101_REG_LOW_BAT_WARN     = 0x1A;
  static constexpr uint8_t AXP2101_REG_PWRON_STATUS     = 0x20;
  static constexpr uint8_t AXP2101_REG_PWROFF_STATUS    = 0x21;
  static constexpr uint8_t AXP2101_REG_PWROFF_ENABLE    = 0x22;
  static constexpr uint8_t AXP2101_REG_DC_OVP_UVP       = 0x23;
  static constexpr uint8_t AXP2101_REG_VOFF_SET         = 0x24;
  static constexpr uint8_t AXP2101_REG_PKEY_CONFIG      = 0x27;
  static constexpr uint8_t AXP2101_REG_ADC_CTRL         = 0x30;

  static constexpr uint8_t AXP2101_REG_ADC_BAT_H        = 0x34;
  static constexpr uint8_t AXP2101_REG_ADC_BAT_L        = 0x35;
  static constexpr uint8_t AXP2101_REG_ADC_VBUS_H       = 0x38;
  static constexpr uint8_t AXP2101_REG_ADC_VBUS_L       = 0x39;
  static constexpr uint8_t AXP2101_REG_ADC_VSYS_H       = 0x3A;
  static constexpr uint8_t AXP2101_REG_ADC_VSYS_L       = 0x3B;
  static constexpr uint8_t AXP2101_REG_BAT_PERCENT      = 0xA4;

  static constexpr uint8_t AXP2101_STATUS1_VBUS_GOOD_MASK     = 0x20;
  static constexpr uint8_t AXP2101_STATUS1_BAT_PRESENT_MASK   = 0x08;
  static constexpr uint8_t AXP2101_STATUS1_THERMAL_MASK       = 0x02;
  static constexpr uint8_t AXP2101_STATUS1_CURRENT_LIMIT_MASK = 0x01;

  static constexpr uint8_t AXP2101_PWROFF_PWRON_PULLDOWN_MASK = 0x01;
  static constexpr uint8_t AXP2101_PWROFF_SOFTWARE_MASK       = 0x02;
  static constexpr uint8_t AXP2101_PWROFF_PWRON_LOW_MASK      = 0x04;
  static constexpr uint8_t AXP2101_PWROFF_VSYS_UV_MASK        = 0x08;
  static constexpr uint8_t AXP2101_PWROFF_VBUS_OV_MASK        = 0x10;
  static constexpr uint8_t AXP2101_PWROFF_DCDC_UV_MASK        = 0x20;
  static constexpr uint8_t AXP2101_PWROFF_DCDC_OV_MASK        = 0x40;
  static constexpr uint8_t AXP2101_PWROFF_OVER_TEMP_MASK      = 0x80;

  static constexpr uint8_t AXP2101_REG_IRQ_ENABLE_1 = 0x41;
  static constexpr uint8_t AXP2101_REG_IRQ_STATUS_1 = 0x49;

  static constexpr uint8_t AXP2101_PKEY_POSITIVE_MASK = 0x01;
  static constexpr uint8_t AXP2101_PKEY_NEGATIVE_MASK = 0x02;
  static constexpr uint8_t AXP2101_PKEY_LONG_MASK     = 0x04;
  static constexpr uint8_t AXP2101_PKEY_SHORT_MASK    = 0x08;
  static constexpr uint8_t AXP2101_PKEY_EVENT_MASK    = 0x0F;

  static constexpr uint8_t AXP2101_PKEY_IRQ_ENABLE_MASK =
    AXP2101_PKEY_POSITIVE_MASK |
    AXP2101_PKEY_NEGATIVE_MASK |
    AXP2101_PKEY_LONG_MASK;

  static constexpr unsigned long POWER_KEY_POLL_INTERVAL_MS = 20;
  static constexpr unsigned long SAFE_SHUTDOWN_FALLBACK_HOLD_MS = 1500;
  static constexpr unsigned long SAFE_SHUTDOWN_BLACK_REFRESH_MS = 100;
  static constexpr unsigned long SAFE_SHUTDOWN_SHOW_WAIT_MS = 150;
  static constexpr unsigned long POWER_DIAG_INTERVAL_MS = 10000;

  static constexpr uint8_t REG_OUTPUT_P0 = 0x02;
  static constexpr uint8_t REG_OUTPUT_P1 = 0x03;
  static constexpr uint8_t REG_CONFIG_P0 = 0x04;
  static constexpr uint8_t REG_CONFIG_P1 = 0x05;
  static constexpr uint8_t REG_GCR       = 0x11;
  static constexpr uint8_t REG_LEDMODE_P0 = 0x12;
  static constexpr uint8_t REG_LEDMODE_P1 = 0x13;

  static constexpr uint8_t BUS_EN_MASK   = 0x02;
  static constexpr uint8_t BOOST_EN_MASK = 0x80;

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
  uint8_t savedLogicalBrightness = 0;

  unsigned long powerKeyPressedAt = 0;
  unsigned long lastPowerKeyPoll = 0;
  unsigned long lastShutdownBlackRefresh = 0;
  unsigned long lastRuntimeI2CFailureLog = 0;

  esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;
  bool bootPowerOnStatusValid = false;
  bool bootPowerOffStatusValid = false;
  uint8_t bootPowerOnStatus = 0;
  uint8_t bootPowerOffStatus = 0;
  unsigned long lastPowerDiag = 0;
  uint32_t powerDiagSampleCount = 0;

  const char* resetReasonText(esp_reset_reason_t reason)
  {
    switch (reason) {
      case ESP_RST_UNKNOWN:   return "UNKNOWN";
      case ESP_RST_POWERON:   return "POWERON";
      case ESP_RST_EXT:       return "EXT";
      case ESP_RST_SW:        return "SOFTWARE";
      case ESP_RST_PANIC:     return "PANIC";
      case ESP_RST_INT_WDT:   return "INT_WDT";
      case ESP_RST_TASK_WDT:  return "TASK_WDT";
      case ESP_RST_WDT:       return "WDT";
      case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
      case ESP_RST_BROWNOUT:  return "BROWNOUT";
      case ESP_RST_SDIO:      return "SDIO";
      default:                return "OTHER";
    }
  }

  const char* batteryDirectionText(uint8_t status2)
  {
    uint8_t state = (status2 >> 5) & 0x03;
    switch (state) {
      case 0: return "STANDBY";
      case 1: return "CHARGING";
      case 2: return "DISCHARGING";
      default: return "UNKNOWN";
    }
  }

  void printPowerOffSource(uint8_t status)
  {
    Serial.printf("[CoreS3_Power][BOOT_DIAG] AXP2101 PWROFF_STATUS=0x%02X\n", status);

    if (status == 0) {
      Serial.println(F("[CoreS3_Power][BOOT_DIAG] Power-off cause: NONE LATCHED / UNKNOWN"));
      return;
    }

    if (status & AXP2101_PWROFF_OVER_TEMP_MASK)
      Serial.println(F("[CoreS3_Power][BOOT_DIAG] Power-off cause: PMIC DIE OVER TEMPERATURE"));
    if (status & AXP2101_PWROFF_DCDC_OV_MASK)
      Serial.println(F("[CoreS3_Power][BOOT_DIAG] Power-off cause: DCDC OVER VOLTAGE"));
    if (status & AXP2101_PWROFF_DCDC_UV_MASK)
      Serial.println(F("[CoreS3_Power][BOOT_DIAG] Power-off cause: DCDC UNDER VOLTAGE"));
    if (status & AXP2101_PWROFF_VBUS_OV_MASK)
      Serial.println(F("[CoreS3_Power][BOOT_DIAG] Power-off cause: VBUS OVER VOLTAGE"));
    if (status & AXP2101_PWROFF_VSYS_UV_MASK)
      Serial.println(F("[CoreS3_Power][BOOT_DIAG] Power-off cause: VSYS UNDER VOLTAGE"));
    if (status & AXP2101_PWROFF_PWRON_LOW_MASK)
      Serial.println(F("[CoreS3_Power][BOOT_DIAG] Power-off cause: PWRON HELD LOW / EN MODE"));
    if (status & AXP2101_PWROFF_SOFTWARE_MASK)
      Serial.println(F("[CoreS3_Power][BOOT_DIAG] Power-off cause: SOFTWARE POWER OFF"));
    if (status & AXP2101_PWROFF_PWRON_PULLDOWN_MASK)
      Serial.println(F("[CoreS3_Power][BOOT_DIAG] Power-off cause: PWRON / POWER KEY PULL-DOWN"));
  }

  bool probeI2C(uint8_t address)
  {
    Wire.beginTransmission(address);
    return (Wire.endTransmission() == 0);
  }

  bool readRegister(uint8_t address, uint8_t reg, uint8_t &value)
  {
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;

    uint8_t count = Wire.requestFrom(address, (uint8_t)1);
    if (count != 1 || !Wire.available()) return false;

    value = Wire.read();
    return true;
  }

  bool writeRegister(uint8_t address, uint8_t reg, uint8_t value)
  {
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(value);
    return (Wire.endTransmission() == 0);
  }

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

  bool readRuntimeAdcH5L8(uint8_t highReg, uint8_t lowReg, uint16_t& value)
  {
    uint8_t high = 0;
    uint8_t low = 0;
    if (!readRuntimeRegister(AXP2101_ADDR, highReg, high)) return false;
    if (!readRuntimeRegister(AXP2101_ADDR, lowReg, low)) return false;
    value = ((uint16_t)(high & 0x1F) << 8) | low;
    return true;
  }

  bool readRuntimeAdcH6L8(uint8_t highReg, uint8_t lowReg, uint16_t& value)
  {
    uint8_t high = 0;
    uint8_t low = 0;
    if (!readRuntimeRegister(AXP2101_ADDR, highReg, high)) return false;
    if (!readRuntimeRegister(AXP2101_ADDR, lowReg, low)) return false;
    value = ((uint16_t)(high & 0x3F) << 8) | low;
    return true;
  }

  void captureBootAxpDiagnostics()
  {
    uint8_t status1 = 0;
    uint8_t status2 = 0;
    uint8_t pwroffEnable = 0;
    uint8_t dcProtection = 0;
    uint8_t voff = 0;
    uint8_t pkeyConfig = 0;
    uint8_t adcCtrl = 0;
    uint8_t gaugeWdtCtrl = 0;
    uint8_t wdtCtrl = 0;
    uint8_t lowBat = 0;
    uint8_t batteryPercent = 0;

    bootPowerOnStatusValid = readRegister(AXP2101_ADDR, AXP2101_REG_PWRON_STATUS, bootPowerOnStatus);
    bootPowerOffStatusValid = readRegister(AXP2101_ADDR, AXP2101_REG_PWROFF_STATUS, bootPowerOffStatus);
    bool status1Valid = readRegister(AXP2101_ADDR, AXP2101_REG_STATUS1, status1);
    bool status2Valid = readRegister(AXP2101_ADDR, AXP2101_REG_STATUS2, status2);
    bool pwroffEnableValid = readRegister(AXP2101_ADDR, AXP2101_REG_PWROFF_ENABLE, pwroffEnable);
    bool dcProtectionValid = readRegister(AXP2101_ADDR, AXP2101_REG_DC_OVP_UVP, dcProtection);
    bool voffValid = readRegister(AXP2101_ADDR, AXP2101_REG_VOFF_SET, voff);
    bool pkeyConfigValid = readRegister(AXP2101_ADDR, AXP2101_REG_PKEY_CONFIG, pkeyConfig);
    bool adcCtrlValid = readRegister(AXP2101_ADDR, AXP2101_REG_ADC_CTRL, adcCtrl);
    bool gaugeWdtValid = readRegister(AXP2101_ADDR, AXP2101_REG_GAUGE_WDT_CTRL, gaugeWdtCtrl);
    bool wdtCtrlValid = readRegister(AXP2101_ADDR, AXP2101_REG_WDT_CTRL, wdtCtrl);
    bool lowBatValid = readRegister(AXP2101_ADDR, AXP2101_REG_LOW_BAT_WARN, lowBat);
    bool batteryPercentValid = readRegister(AXP2101_ADDR, AXP2101_REG_BAT_PERCENT, batteryPercent);

    Serial.println(F("[CoreS3_Power][BOOT_DIAG] ========================================"));
    Serial.printf("[CoreS3_Power][BOOT_DIAG] ESP reset reason: %s (%d)\n", resetReasonText(bootResetReason), (int)bootResetReason);

    if (bootPowerOnStatusValid)
      Serial.printf("[CoreS3_Power][BOOT_DIAG] AXP2101 PWRON_STATUS=0x%02X\n", bootPowerOnStatus);
    else
      Serial.println(F("[CoreS3_Power][BOOT_DIAG] AXP2101 PWRON_STATUS read FAILED"));

    if (bootPowerOffStatusValid)
      printPowerOffSource(bootPowerOffStatus);
    else
      Serial.println(F("[CoreS3_Power][BOOT_DIAG] AXP2101 PWROFF_STATUS read FAILED"));

    if (status1Valid && status2Valid) {
      Serial.printf(
        "[CoreS3_Power][BOOT_DIAG] STATUS1=0x%02X STATUS2=0x%02X VBUS=%s BAT=%s DIR=%s LIMIT=%s THERMAL=%s\n",
        status1,
        status2,
        (status1 & AXP2101_STATUS1_VBUS_GOOD_MASK) ? "GOOD" : "NOT_GOOD",
        (status1 & AXP2101_STATUS1_BAT_PRESENT_MASK) ? "PRESENT" : "ABSENT",
        batteryDirectionText(status2),
        (status1 & AXP2101_STATUS1_CURRENT_LIMIT_MASK) ? "YES" : "NO",
        (status1 & AXP2101_STATUS1_THERMAL_MASK) ? "YES" : "NO"
      );
    }

    if (batteryPercentValid)
      Serial.printf("[CoreS3_Power][BOOT_DIAG] Battery fuel gauge: %u%%\n", batteryPercent);

    if (voffValid) {
      uint16_t voffMv = 2600 + ((uint16_t)(voff & 0x07) * 100);
      Serial.printf("[CoreS3_Power][BOOT_DIAG] VOFF_SET=0x%02X -> VSYS shutdown %u mV\n", voff, voffMv);
    }

    if (pwroffEnableValid)
      Serial.printf("[CoreS3_Power][BOOT_DIAG] PWROFF_EN=0x%02X\n", pwroffEnable);

    if (dcProtectionValid)
      Serial.printf("[CoreS3_Power][BOOT_DIAG] DC_OVP_UVP_CTRL=0x%02X\n", dcProtection);

    if (pkeyConfigValid)
      Serial.printf("[CoreS3_Power][BOOT_DIAG] PKEY_CONFIG=0x%02X\n", pkeyConfig);

    if (adcCtrlValid)
      Serial.printf("[CoreS3_Power][BOOT_DIAG] ADC_CTRL=0x%02X\n", adcCtrl);

    if (gaugeWdtValid && wdtCtrlValid) {
      Serial.printf(
        "[CoreS3_Power][BOOT_DIAG] GAUGE_WDT_CTRL=0x%02X PMIC_WDT=%s WDT_CTRL=0x%02X\n",
        gaugeWdtCtrl,
        (gaugeWdtCtrl & 0x01) ? "ENABLED" : "DISABLED",
        wdtCtrl
      );
    }

    if (lowBatValid) {
      uint8_t lowBatteryShutdownPercent = lowBat & 0x0F;
      Serial.printf(
        "[CoreS3_Power][BOOT_DIAG] LOW_BAT_WARN=0x%02X shutdown threshold=%u%%\n",
        lowBat,
        lowBatteryShutdownPercent
      );
    }

    Serial.println(F("[CoreS3_Power][BOOT_DIAG] ========================================"));
  }

  bool configureAW9523()
  {
    bool ok = true;
    ok &= writeRegister(AW9523B_ADDR, REG_CONFIG_P0, CORE_S3_CONFIG_P0);
    ok &= writeRegister(AW9523B_ADDR, REG_CONFIG_P1, CORE_S3_CONFIG_P1);
    ok &= writeRegister(AW9523B_ADDR, REG_GCR, CORE_S3_GCR);
    ok &= writeRegister(AW9523B_ADDR, REG_LEDMODE_P0, CORE_S3_LEDMODE_P0);
    ok &= writeRegister(AW9523B_ADDR, REG_LEDMODE_P1, CORE_S3_LEDMODE_P1);
    return ok;
  }

  bool enableExternal5V()
  {
    powerEnableAttempted = true;
    if (!readRegister(AW9523B_ADDR, REG_OUTPUT_P0, p0Before)) return false;
    if (!readRegister(AW9523B_ADDR, REG_OUTPUT_P1, p1Before)) return false;
    if (!configureAW9523()) return false;

    uint8_t newP1 = p1Before | BOOST_EN_MASK;
    if (!writeRegister(AW9523B_ADDR, REG_OUTPUT_P1, newP1)) return false;
    delay(10);

    uint8_t newP0 = p0Before | BUS_EN_MASK;
    if (!writeRegister(AW9523B_ADDR, REG_OUTPUT_P0, newP0)) return false;
    delay(10);

    if (!readRegister(AW9523B_ADDR, REG_OUTPUT_P0, p0After)) return false;
    if (!readRegister(AW9523B_ADDR, REG_OUTPUT_P1, p1After)) return false;

    busEnabled   = (p0After & BUS_EN_MASK) != 0;
    boostEnabled = (p1After & BOOST_EN_MASK) != 0;
    return busEnabled && boostEnabled;
  }

  bool configureRuntimePowerKeyMonitor()
  {
    runtimePowerKeyMonitorAttempted = true;
    uint8_t chipId = 0;

    if (!readRuntimeRegister(AXP2101_ADDR, AXP2101_REG_CHIP_ID, chipId)) {
      Serial.println(F("[CoreS3_Power] Runtime power key: M5GFX I2C1 AXP2101 read FAILED"));
      return false;
    }

    if (chipId != 0x4A) {
      Serial.printf("[CoreS3_Power] Runtime power key: unexpected AXP2101 chip ID 0x%02X\n", chipId);
      return false;
    }

    if (!runtimePowerKeyBusReadyLogged) {
      runtimePowerKeyBusReadyLogged = true;
      Serial.printf("[CoreS3_Power] Runtime I2C: M5GFX I2C_NUM_1 AXP2101 READY (ID=0x%02X)\n", chipId);
    }

    if (!readRuntimeRegister(AXP2101_ADDR, AXP2101_REG_IRQ_ENABLE_1, axpIrqEnableBefore)) return false;

    uint8_t newIrqEnable = axpIrqEnableBefore | AXP2101_PKEY_IRQ_ENABLE_MASK;
    if (!writeRuntimeRegister(AXP2101_ADDR, AXP2101_REG_IRQ_ENABLE_1, newIrqEnable)) return false;
    if (!readRuntimeRegister(AXP2101_ADDR, AXP2101_REG_IRQ_ENABLE_1, axpIrqEnableAfter)) return false;

    if ((axpIrqEnableAfter & AXP2101_PKEY_IRQ_ENABLE_MASK) != AXP2101_PKEY_IRQ_ENABLE_MASK) return false;

    uint8_t staleStatus = 0;
    if (readRuntimeRegister(AXP2101_ADDR, AXP2101_REG_IRQ_STATUS_1, staleStatus)) {
      uint8_t stalePowerKeyFlags = staleStatus & AXP2101_PKEY_EVENT_MASK;

      if (stalePowerKeyFlags != 0) {
        Serial.printf(
          "[CoreS3_Power][BOOT_DIAG] Stale PKEY IRQ before clear: 0x%02X%s%s%s%s\n",
          stalePowerKeyFlags,
          (stalePowerKeyFlags & AXP2101_PKEY_POSITIVE_MASK) ? " RELEASE" : "",
          (stalePowerKeyFlags & AXP2101_PKEY_NEGATIVE_MASK) ? " PRESS" : "",
          (stalePowerKeyFlags & AXP2101_PKEY_LONG_MASK) ? " LONG" : "",
          (stalePowerKeyFlags & AXP2101_PKEY_SHORT_MASK) ? " SHORT" : ""
        );

        writeRuntimeRegister(AXP2101_ADDR, AXP2101_REG_IRQ_STATUS_1, stalePowerKeyFlags);
      }
      else {
        Serial.println(F("[CoreS3_Power][BOOT_DIAG] Stale PKEY IRQ before clear: NONE"));
      }
    }

    powerKeyPressed = false;
    powerKeyPressedAt = 0;
    lastPowerKeyPoll = millis();

    Serial.printf("[CoreS3_Power] Runtime PKEY IRQEN1: 0x%02X -> 0x%02X\n", axpIrqEnableBefore, axpIrqEnableAfter);
    Serial.println(F("[CoreS3_Power] Runtime power key monitor: ARMED on M5GFX I2C1"));
    return true;
  }

  void servicePowerDiagnostics()
  {
    if (!powerKeyMonitorReady || safeShutdownBlankActive) return;

    unsigned long now = millis();
    if (lastPowerDiag != 0 && now - lastPowerDiag < POWER_DIAG_INTERVAL_MS) return;

    lastPowerDiag = now;
    powerDiagSampleCount++;

    uint8_t status1 = 0;
    uint8_t status2 = 0;
    uint8_t batteryPercent = 0;
    uint8_t pwroffStatus = 0;
    uint8_t voff = 0;
    uint8_t gaugeWdtCtrl = 0;

    uint16_t batteryMv = 0;
    uint16_t vbusMv = 0;
    uint16_t vsysMv = 0;

    bool status1Ok = readRuntimeRegister(AXP2101_ADDR, AXP2101_REG_STATUS1, status1);
    bool status2Ok = readRuntimeRegister(AXP2101_ADDR, AXP2101_REG_STATUS2, status2);
    bool batteryPercentOk = readRuntimeRegister(AXP2101_ADDR, AXP2101_REG_BAT_PERCENT, batteryPercent);
    bool pwroffOk = readRuntimeRegister(AXP2101_ADDR, AXP2101_REG_PWROFF_STATUS, pwroffStatus);
    bool voffOk = readRuntimeRegister(AXP2101_ADDR, AXP2101_REG_VOFF_SET, voff);
    bool gaugeWdtOk = readRuntimeRegister(AXP2101_ADDR, AXP2101_REG_GAUGE_WDT_CTRL, gaugeWdtCtrl);
    bool batteryVoltageOk = readRuntimeAdcH5L8(AXP2101_REG_ADC_BAT_H, AXP2101_REG_ADC_BAT_L, batteryMv);
    bool vbusVoltageOk = readRuntimeAdcH6L8(AXP2101_REG_ADC_VBUS_H, AXP2101_REG_ADC_VBUS_L, vbusMv);
    bool vsysVoltageOk = readRuntimeAdcH6L8(AXP2101_REG_ADC_VSYS_H, AXP2101_REG_ADC_VSYS_L, vsysMv);

    if (!status1Ok || !status2Ok) {
      Serial.printf(
        "[CoreS3_Power][DIAG] t=%lus sample=%lu PMIC STATUS READ FAILED\n",
        (unsigned long)(now / 1000),
        (unsigned long)powerDiagSampleCount
      );
      return;
    }

    bool batteryPresent = (status1 & AXP2101_STATUS1_BAT_PRESENT_MASK) != 0;
    bool vbusGood = (status1 & AXP2101_STATUS1_VBUS_GOOD_MASK) != 0;
    bool currentLimited = (status1 & AXP2101_STATUS1_CURRENT_LIMIT_MASK) != 0;
    bool thermalRegulation = (status1 & AXP2101_STATUS1_THERMAL_MASK) != 0;

    uint16_t voffMv = voffOk ? (2600 + ((uint16_t)(voff & 0x07) * 100)) : 0;

    Serial.printf(
      "[CoreS3_Power][DIAG] t=%lus sample=%lu S1=0x%02X S2=0x%02X VBUS=%s BAT=%s DIR=%s SOC=%d VBAT=%u VBUSmV=%u VSYS=%u VOFF=%u LIMIT=%s THERM=%s WDT=%s POFF=0x%02X bri=%u strip=%u\n",
      (unsigned long)(now / 1000),
      (unsigned long)powerDiagSampleCount,
      status1,
      status2,
      vbusGood ? "GOOD" : "NOT_GOOD",
      batteryPresent ? "PRESENT" : "ABSENT",
      batteryDirectionText(status2),
      batteryPercentOk ? (int)batteryPercent : -1,
      batteryVoltageOk ? batteryMv : 0,
      vbusVoltageOk ? vbusMv : 0,
      vsysVoltageOk ? vsysMv : 0,
      voffMv,
      currentLimited ? "YES" : "NO",
      thermalRegulation ? "YES" : "NO",
      gaugeWdtOk ? ((gaugeWdtCtrl & 0x01) ? "ON" : "OFF") : "?",
      pwroffOk ? pwroffStatus : 0,
      bri,
      strip.getBrightness()
    );
  }

  void waitForLedOutputComplete()
  {
    unsigned long waitStart = millis();
    while (strip.isUpdating() && millis() - waitStart < SAFE_SHUTDOWN_SHOW_WAIT_MS) {
      delay(1);
    }
  }

  void beginSafeShutdownBlank(uint8_t triggerStatus)
  {
    if (safeShutdownBlankActive) return;

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

    strip.waitForIt();
    strip.setBrightness(0, true);
    strip.show();
    waitForLedOutputComplete();
    strip.suspend();
    strip.waitForIt();

    safeShutdownBlankActive = true;
    lastShutdownBlackRefresh = millis();

    Serial.println(F("[CoreS3_Power] Safe shutdown: BLACK frame sent and strip suspended"));
  }

  void maintainSafeShutdownBlank(unsigned long now)
  {
    if (!safeShutdownBlankActive || !powerKeyPressed) return;
    if (now - lastShutdownBlackRefresh < SAFE_SHUTDOWN_BLACK_REFRESH_MS) return;

    lastShutdownBlackRefresh = now;
    strip.setBrightness(0, true);
    strip.show();
    waitForLedOutputComplete();
  }

  void cancelSafeShutdownBlank()
  {
    if (!safeShutdownBlankActive) return;

    Serial.println(F("[CoreS3_Power] Safe shutdown canceled: restoring LED output"));
    strip.resume();

    uint8_t restoreBrightness = bri;
    if (restoreBrightness == 0 && savedLogicalBrightness > 0) {
      restoreBrightness = savedLogicalBrightness;
    }

    strip.setBrightness(restoreBrightness, true);
    strip.show();
    waitForLedOutputComplete();

    safeShutdownBlankActive = false;
    safeShutdownLastCanceled = true;
    strip.trigger();

    Serial.printf("[CoreS3_Power] Safe shutdown canceled: restored bri=%u\n", restoreBrightness);
  }

  void servicePhysicalPowerKey()
  {
    unsigned long now = millis();

    if (!powerKeyMonitorReady) {
      if (!runtimePowerKeyMonitorAttempted || now - lastRuntimeI2CFailureLog >= 1000) {
        if (configureRuntimePowerKeyMonitor()) {
          powerKeyMonitorReady = true;
          coreS3PowerSafeShutdownMonitorReadyState = true;
        }
        else {
          lastRuntimeI2CFailureLog = now;
        }
      }
      return;
    }

    if (now - lastPowerKeyPoll < POWER_KEY_POLL_INTERVAL_MS) {
      maintainSafeShutdownBlank(now);
      return;
    }

    lastPowerKeyPoll = now;
    uint8_t status = 0;

    if (!readRuntimeRegister(AXP2101_ADDR, AXP2101_REG_IRQ_STATUS_1, status)) {
      if (now - lastRuntimeI2CFailureLog >= 1000) {
        lastRuntimeI2CFailureLog = now;
        Serial.println(F("[CoreS3_Power] Runtime PKEY status read FAILED on M5GFX I2C1"));
      }
      maintainSafeShutdownBlank(now);
      return;
    }

    uint8_t powerKeyStatus = status & AXP2101_PKEY_EVENT_MASK;

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

      writeRuntimeRegister(AXP2101_ADDR, AXP2101_REG_IRQ_STATUS_1, powerKeyStatus);
    }

    if (powerKeyStatus & AXP2101_PKEY_NEGATIVE_MASK) {
      powerKeyPressed = true;
      powerKeyPressedAt = now;
    }

    if ((powerKeyStatus & AXP2101_PKEY_LONG_MASK) && !safeShutdownBlankActive) {
      powerKeyPressed = true;
      if (powerKeyPressedAt == 0) powerKeyPressedAt = now;
      beginSafeShutdownBlank(powerKeyStatus);
    }

    if (
      powerKeyPressed &&
      !safeShutdownBlankActive &&
      powerKeyPressedAt > 0 &&
      now - powerKeyPressedAt >= SAFE_SHUTDOWN_FALLBACK_HOLD_MS
    ) {
      Serial.println(F("[CoreS3_Power] Safe shutdown: PRESS timer fallback"));
      beginSafeShutdownBlank(AXP2101_PKEY_NEGATIVE_MASK);
    }

    if (powerKeyStatus & AXP2101_PKEY_POSITIVE_MASK) {
      powerKeyPressed = false;
      powerKeyPressedAt = 0;
      cancelSafeShutdownBlank();
      return;
    }

    maintainSafeShutdownBlank(now);
  }

public:
  void setup() override
  {
    coreS3PowerInitializationCompleteState = false;
    coreS3PowerExternal5VReadyState = false;
    coreS3PowerSafeShutdownMonitorReadyState = false;

    bootResetReason = esp_reset_reason();

    Serial.println();
    Serial.println(F("[CoreS3_Power] Power Enable test start"));
    Serial.printf("[CoreS3_Power] I2C SDA=%d SCL=%d\n", i2c_sda, i2c_scl);
    Serial.printf(
      "[CoreS3_Power][BOOT_DIAG] ESP reset reason: %s (%d)\n",
      resetReasonText(bootResetReason),
      (int)bootResetReason
    );

    if (i2c_sda != 12 || i2c_scl != 11) {
      Serial.println(F("[CoreS3_Power] ERROR: Invalid CoreS3 I2C pins"));
      coreS3PowerInitializationCompleteState = true;
      return;
    }

    aw9523Found = probeI2C(AW9523B_ADDR);
    axp2101Found = probeI2C(AXP2101_ADDR);

    Serial.printf("[CoreS3_Power] AW9523B (0x58): %s\n", aw9523Found ? "FOUND" : "NOT FOUND");
    Serial.printf("[CoreS3_Power] AXP2101 (0x34): %s\n", axp2101Found ? "FOUND" : "NOT FOUND");

    if (!aw9523Found || !axp2101Found) {
      Serial.println(F("[CoreS3_Power] Power enable canceled"));
      coreS3PowerInitializationCompleteState = true;
      return;
    }

    captureBootAxpDiagnostics();

    powerEnableSuccess = enableExternal5V();
    coreS3PowerExternal5VReadyState = powerEnableSuccess;

    powerKeyMonitorReady = false;
    runtimePowerKeyMonitorAttempted = false;

    Serial.println(F("[CoreS3_Power] Power key monitor: DEFERRED until M5GFX I2C1 is active"));
    Serial.println(F("[CoreS3_Power] Safe shutdown: AXP2101 LONG IRQ primary trigger"));
    Serial.printf("[CoreS3_Power] Safe shutdown: PRESS fallback >= %lu ms\n", SAFE_SHUTDOWN_FALLBACK_HOLD_MS);
    Serial.printf("[CoreS3_Power] BUS_EN: %s\n", busEnabled ? "ON" : "OFF");
    Serial.printf("[CoreS3_Power] BOOST_EN: %s\n", boostEnabled ? "ON" : "OFF");
    Serial.printf("[CoreS3_Power] External 5V: %s\n", powerEnableSuccess ? "ENABLED" : "FAILED");
    Serial.printf("[CoreS3_Power] AW9523 P0: 0x%02X -> 0x%02X\n", p0Before, p0After);
    Serial.printf("[CoreS3_Power] AW9523 P1: 0x%02X -> 0x%02X\n", p1Before, p1After);

    coreS3PowerInitializationCompleteState = true;

    Serial.println(F("[CoreS3_Power] Power Enable test end"));
    Serial.println();
  }

  void loop() override
  {
    servicePhysicalPowerKey();
    servicePowerDiagnostics();
  }

  void addToJsonInfo(JsonObject& root) override
  {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");

    JsonArray i2cInfo = user.createNestedArray("CoreS3 I2C");
    i2cInfo.add((i2c_sda == 12 && i2c_scl == 11) ? "GPIO12 / GPIO11 OK" : "I2C PIN ERROR");

    JsonArray awInfo = user.createNestedArray("CoreS3 AW9523B");
    awInfo.add(aw9523Found ? "Found (0x58)" : "Not found");

    JsonArray axpInfo = user.createNestedArray("CoreS3 AXP2101");
    axpInfo.add(axp2101Found ? "Found (0x34)" : "Not found");

    JsonArray shutdownInfo = user.createNestedArray("CoreS3 Safe Shutdown");
    if (!powerKeyMonitorReady) {
      shutdownInfo.add(runtimePowerKeyMonitorAttempted ? "Runtime M5GFX I2C1 monitor unavailable" : "Runtime M5GFX I2C1 monitor pending");
    }
    else if (safeShutdownBlankActive) {
      shutdownInfo.add("BLACK output active - waiting for PMIC off");
    }
    else if (safeShutdownLastCanceled) {
      shutdownInfo.add("ARMED - last shutdown hold canceled");
    }
    else if (safeShutdownEverTriggered) {
      shutdownInfo.add("ARMED - shutdown BLACK previously triggered");
    }
    else {
      shutdownInfo.add("ARMED");
    }

    shutdownInfo.add("Trigger: AXP2101 Long Press IRQ");

    char fallbackText[48];
    snprintf(fallbackText, sizeof(fallbackText), "PRESS fallback: %lu ms", SAFE_SHUTDOWN_FALLBACK_HOLD_MS);
    shutdownInfo.add(fallbackText);

    char lastIrqText[40];
    snprintf(lastIrqText, sizeof(lastIrqText), "Last IRQ status: 0x%02X", lastPowerKeyStatus);
    shutdownInfo.add(lastIrqText);

    char triggerIrqText[40];
    snprintf(triggerIrqText, sizeof(triggerIrqText), "Last BLACK trigger: 0x%02X", lastSafeShutdownTriggerStatus);
    shutdownInfo.add(triggerIrqText);

    char irqText[48];
    snprintf(irqText, sizeof(irqText), "IRQEN1 0x%02X -> 0x%02X", axpIrqEnableBefore, axpIrqEnableAfter);
    shutdownInfo.add(irqText);

    JsonArray powerInfo = user.createNestedArray("CoreS3 Ext 5V");
    if (!powerEnableAttempted) powerInfo.add("Not attempted");
    else if (powerEnableSuccess) powerInfo.add("ENABLED");
    else powerInfo.add("FAILED");

    JsonArray busInfo = user.createNestedArray("CoreS3 BUS_EN");
    busInfo.add(busEnabled ? "ON" : "OFF");

    JsonArray boostInfo = user.createNestedArray("CoreS3 BOOST_EN");
    boostInfo.add(boostEnabled ? "ON" : "OFF");

    char p0Text[32];
    snprintf(p0Text, sizeof(p0Text), "0x%02X -> 0x%02X", p0Before, p0After);
    JsonArray p0Info = user.createNestedArray("CoreS3 AW P0");
    p0Info.add(p0Text);

    char p1Text[32];
    snprintf(p1Text, sizeof(p1Text), "0x%02X -> 0x%02X", p1Before, p1After);
    JsonArray p1Info = user.createNestedArray("CoreS3 AW P1");
    p1Info.add(p1Text);

    JsonArray resetInfo = user.createNestedArray("CoreS3 Last ESP Reset");
    resetInfo.add(resetReasonText(bootResetReason));

    JsonArray offInfo = user.createNestedArray("CoreS3 Last PMIC Off");
    if (bootPowerOffStatusValid) {
      char offText[24];
      snprintf(offText, sizeof(offText), "PWROFF 0x%02X", bootPowerOffStatus);
      offInfo.add(offText);
    }
    else {
      offInfo.add("Unavailable");
    }

    JsonArray onInfo = user.createNestedArray("CoreS3 PMIC Power On");
    if (bootPowerOnStatusValid) {
      char onText[24];
      snprintf(onText, sizeof(onText), "PWRON 0x%02X", bootPowerOnStatus);
      onInfo.add(onText);
    }
    else {
      onInfo.add("Unavailable");
    }
  }
};

static CoreS3PowerUsermod coreS3PowerUsermod;
REGISTER_USERMOD(coreS3PowerUsermod);
