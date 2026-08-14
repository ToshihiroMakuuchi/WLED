#include "wled.h"
#include <M5GFX.h>
#include <driver/i2c.h>

// Phase 10.4.2P-V17e diagnostics
#include <esp_system.h>
#include <esp_heap_caps.h>

// ===========================================================
// CoreS3 Audio Usermod
//
// Phase 10.4.2P-V17e
//
// Purpose
//   - Access the CoreS3 internal I2C bus through the same
//     M5GFX I2C_NUM_1 owner used by Display / Touch.
//   - Detect the CoreS3 AXP2101 and ES7210 on that shared bus.
//   - Configure and verify the built-in ES7210 dual microphones.
//   - Reserve CoreS3 internal audio GPIO0 (MCLK) and GPIO14 (DIN)
//     in WLED PinManager so they cannot be assigned to other features.
//   - Publish codec readiness to WLED Audio Reactive.
//   - Leave I2S_NUM_1 / PCM / FFT ownership exclusively to Audio Reactive.
//   - Report codec / integration readiness to Serial and WLED Info.
//   - Diagnose V17 unexpected resets without changing the audio architecture.
//
// This phase intentionally does NOT:
//   - install or read I2S,
//   - run FFT itself,
//   - change LED state,
//   - modify CoreS3 Display / Touch behavior,
//   - modify CoreS3 power rails,
//   - reinitialize or take ownership of the internal I2C bus.
//
// Audio ownership in Phase 10.4.2:
//   CoreS3_Audio : fixed-pin reservation + ES7210 + I2C diagnostics
//                  + codec-ready signal
//   AudioReactive: I2S_NUM_1 + PCM + AGC + FFT + audio effects
//
// CoreS3 internal audio hardware
//   ES7210 I2C : 0x40
//   I2C SDA    : GPIO12
//   I2C SCL    : GPIO11
//   I2S MCLK   : GPIO0
//   I2S BCLK   : GPIO34
//   I2S WS     : GPIO33
//   I2S DATA   : GPIO14
//   I2S Port   : I2S_NUM_1
//   Channels   : Stereo (MIC1 / MIC2)
//
// The ES7210 register configuration and CoreS3 audio pin mapping
// follow the M5Stack M5Unified CoreS3 microphone implementation.
//
// Phase 10.4.2 change
//   CoreS3 Display/Touch uses M5GFX internal I2C_NUM_1 on GPIO12/11.
//   The original Phase 10.4.0 Audio probe used Arduino Wire (I2C0),
//   so after M5GFX initialization it could no longer see the same
//   internal devices. Audio now uses lgfx::i2c transactions on
//   I2C_NUM_1 and never calls i2c/Wire begin or release.
//
// Phase 10.4.2P-V17e diagnostic change
//   - Print the previous ESP-IDF reset reason at every boot.
//   - Print PSRAM / heap state at boot.
//   - Print a lightweight runtime heartbeat every 10 seconds.
//   - Do not change the current V17 platform, PSRAM, RMT, I2S,
//     codec, Display, Touch, or AudioReactive behavior.
// ===========================================================

static volatile bool coreS3AudioCodecReadyState = false;

extern "C" bool coreS3AudioCodecReady()
{
  return coreS3AudioCodecReadyState;
}

#if defined(WLED_M5STACK_CORES3_AUDIO)
extern "C" bool coreS3AudioReactiveSourceReady();
#endif

class CoreS3AudioUsermod : public Usermod
{
private:
  // ---------------------------------------------------------
  // CoreS3 hardware constants
  // ---------------------------------------------------------

  static constexpr uint8_t ES7210_ADDR = 0x40;
  static constexpr uint8_t AXP2101_ADDR = 0x34;
  static constexpr int CORES3_I2C_SDA = 12;
  static constexpr int CORES3_I2C_SCL = 11;
  static constexpr i2c_port_t CORES3_INTERNAL_I2C_PORT = I2C_NUM_1;
  static constexpr uint32_t CORES3_INTERNAL_I2C_FREQUENCY = 400000;

  static constexpr int AUDIO_MCLK_PIN = 0;
  static constexpr int AUDIO_BCLK_PIN = 34;
  static constexpr int AUDIO_WS_PIN = 33;
  static constexpr int AUDIO_DATA_IN_PIN = 14;
  static constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
  static constexpr unsigned long AUDIO_INIT_DELAY_MS = 2500;
  static constexpr unsigned long AUDIO_INIT_RETRY_MS = 1000;
  static constexpr uint8_t AUDIO_INIT_MAX_ATTEMPTS = 5;

  // ---------------------------------------------------------
  // Phase 10.4.2 runtime state
  // ---------------------------------------------------------

  bool coreS3PinsValid = false;
  bool audioPinsReserved = false;
  bool es7210Found = false;
  bool es7210Configured = false;
  bool initializationFinished = false;
  uint8_t initAttemptCount = 0;

  unsigned long setupStartMs = 0;
  unsigned long lastInitAttemptMs = 0;

  bool axp2101Found = false;
  bool axp2101RegistersRead = false;
  uint8_t axp2101Reg90 = 0;
  uint8_t axp2101Reg93 = 0;
  uint8_t es7210ProbeReg00 = 0;

  // ---------------------------------------------------------
  // Phase 10.4.2P-V17e boot / runtime diagnostics
  // ---------------------------------------------------------

  static constexpr unsigned long DIAG_INTERVAL_MS = 10000;
  unsigned long lastDiagMs = 0;

  const char* getResetReasonName(esp_reset_reason_t reason) const
  {
    switch (reason) {
      case ESP_RST_UNKNOWN:
        return "UNKNOWN";

      case ESP_RST_POWERON:
        return "POWERON";

      case ESP_RST_EXT:
        return "EXTERNAL";

      case ESP_RST_SW:
        return "SOFTWARE";

      case ESP_RST_PANIC:
        return "PANIC";

      case ESP_RST_INT_WDT:
        return "INTERRUPT_WDT";

      case ESP_RST_TASK_WDT:
        return "TASK_WDT";

      case ESP_RST_WDT:
        return "OTHER_WDT";

      case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP";

      case ESP_RST_BROWNOUT:
        return "BROWNOUT";

      case ESP_RST_SDIO:
        return "SDIO";

      case ESP_RST_USB:
        return "USB";

      case ESP_RST_JTAG:
        return "JTAG";

      case ESP_RST_EFUSE:
        return "EFUSE";

      case ESP_RST_PWR_GLITCH:
        return "POWER_GLITCH";

      case ESP_RST_CPU_LOCKUP:
        return "CPU_LOCKUP";

      default:
        return "OTHER";
    }
  }

  void printBootDiagnostics()
  {
    const esp_reset_reason_t reason = esp_reset_reason();

    const size_t psramTotal =
      heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    const size_t psramFree =
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    const size_t psramLargest =
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    const uint32_t heapFree =
      esp_get_free_heap_size();

    const uint32_t internalHeapFree =
      esp_get_free_internal_heap_size();

    const uint32_t minimumHeap =
      esp_get_minimum_free_heap_size();

    Serial.println();
    Serial.println(F("============================================================"));
    Serial.println(F("[CoreS3_DIAG] Phase 10.4.2P-V17e boot diagnostics"));

    Serial.printf(
      "[CoreS3_DIAG] Reset reason: %d (%s)\n",
      static_cast<int>(reason),
      getResetReasonName(reason)
    );

    Serial.printf(
      "[CoreS3_DIAG] PSRAM total : %lu bytes (%.2f MB)\n",
      static_cast<unsigned long>(psramTotal),
      static_cast<double>(psramTotal) / 1048576.0
    );

    Serial.printf(
      "[CoreS3_DIAG] PSRAM free  : %lu bytes (%.2f MB)\n",
      static_cast<unsigned long>(psramFree),
      static_cast<double>(psramFree) / 1048576.0
    );

    Serial.printf(
      "[CoreS3_DIAG] PSRAM largest block: %lu bytes\n",
      static_cast<unsigned long>(psramLargest)
    );

    Serial.printf(
      "[CoreS3_DIAG] Heap free   : %lu bytes\n",
      static_cast<unsigned long>(heapFree)
    );

    Serial.printf(
      "[CoreS3_DIAG] Internal heap free: %lu bytes\n",
      static_cast<unsigned long>(internalHeapFree)
    );

    Serial.printf(
      "[CoreS3_DIAG] Minimum free heap : %lu bytes\n",
      static_cast<unsigned long>(minimumHeap)
    );

    Serial.println(F("============================================================"));
    Serial.println();
  }

  void printRuntimeDiagnostics(unsigned long now)
  {
    const size_t psramFree =
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    const uint32_t heapFree =
      esp_get_free_heap_size();

    const uint32_t internalHeapFree =
      esp_get_free_internal_heap_size();

    const uint32_t minimumHeap =
      esp_get_minimum_free_heap_size();

    Serial.printf(
      "[CoreS3_DIAG] Alive=%lus Heap=%lu Internal=%lu MinHeap=%lu PSRAM_Free=%lu\n",
      now / 1000UL,
      static_cast<unsigned long>(heapFree),
      static_cast<unsigned long>(internalHeapFree),
      static_cast<unsigned long>(minimumHeap),
      static_cast<unsigned long>(psramFree)
    );
  }

  // ---------------------------------------------------------
  // CoreS3 internal audio pin reservation
  //
  // GPIO0  = ES7210 MCLK (output)
  // GPIO14 = ES7210 DIN  (input)
  //
  // GPIO33/34 are already unavailable to WLED on this CoreS3 build
  // and appear as "System" in Pin Info, so only GPIO0/14 need an
  // explicit PinManager reservation.
  //
  // PinOwner::UM_Audioreactive is used intentionally so WLED Pin Info
  // reports these as Usermod-owned and all normal pin selectors treat
  // them as unavailable.
  // ---------------------------------------------------------

  void neutralizePersistedGpio0Button()
  {
    if (PinManager::getPinOwner(AUDIO_MCLK_PIN) == PinOwner::Button) {
      PinManager::deallocatePin(AUDIO_MCLK_PIN, PinOwner::Button);
    }

    for (auto& button : buttons) {
      if (button.pin == AUDIO_MCLK_PIN) {
        button.pin = -1;
        button.type = BTN_TYPE_NONE;
        button.pressedBefore = false;
        button.longPressed = false;
        button.pressedTime = 0;
        button.waitTime = 0;
      }
    }
  }

  bool reserveInternalAudioPins()
  {
    neutralizePersistedGpio0Button();

    if (PinManager::isPinAllocated(AUDIO_MCLK_PIN, PinOwner::UM_Audioreactive) &&
        PinManager::isPinAllocated(AUDIO_DATA_IN_PIN, PinOwner::UM_Audioreactive)) {
      return true;
    }

    if (PinManager::isPinAllocated(AUDIO_MCLK_PIN) ||
        PinManager::isPinAllocated(AUDIO_DATA_IN_PIN)) {
      Serial.printf(
        "[CoreS3_Audio] ERROR: internal audio pin conflict MCLK0=%s DIN14=%s\n",
        PinManager::getPinOwnerName(AUDIO_MCLK_PIN),
        PinManager::getPinOwnerName(AUDIO_DATA_IN_PIN)
      );
      return false;
    }

    const managed_pin_type audioPins[] = {
      { AUDIO_MCLK_PIN, true  },  // ES7210 master clock output
      { AUDIO_DATA_IN_PIN, false } // ES7210 PCM data input
    };

    if (!PinManager::allocateMultiplePins(
          audioPins,
          sizeof(audioPins) / sizeof(audioPins[0]),
          PinOwner::UM_Audioreactive
        )) {
      Serial.println(F("[CoreS3_Audio] ERROR: failed to reserve GPIO0/GPIO14 for internal audio"));
      return false;
    }

    return true;
  }

  // ---------------------------------------------------------
  // Shared CoreS3 internal I2C helpers
  //
  // IMPORTANT:
  // M5GFX owns the CoreS3 internal bus as I2C_NUM_1 on GPIO12/11.
  // Audio must use that same owner after Display initialization.
  // Do not call Wire.begin(), i2c_driver_install(), lgfx::i2c::init(),
  // release(), or otherwise reinitialize the bus here.
  // ---------------------------------------------------------

  bool readRegister(uint8_t address, uint8_t reg, uint8_t& value)
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

  bool writeRegister(uint8_t address, uint8_t reg, uint8_t value)
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

  // ---------------------------------------------------------
  // ES7210 microphone configuration
  //
  // MIC1 / MIC2 are enabled for the two built-in microphones.
  // MIC3 / MIC4 remain powered down.
  // ---------------------------------------------------------

  bool configureES7210()
  {
    struct RegisterValue {
      uint8_t reg;
      uint8_t value;
    };

    // Reset before applying the CoreS3 microphone profile.
    if (!writeRegister(ES7210_ADDR, 0x00, 0xFF)) {
      return false;
    }

    delay(1);

    static constexpr RegisterValue registers[] = {
      { 0x00, 0x41 },
      { 0x01, 0x1F },
      { 0x06, 0x00 },
      { 0x07, 0x20 },
      { 0x08, 0x10 },
      { 0x09, 0x30 },
      { 0x0A, 0x30 },
      { 0x20, 0x0A },
      { 0x21, 0x2A },
      { 0x22, 0x0A },
      { 0x23, 0x2A },
      { 0x02, 0xC1 },
      { 0x04, 0x01 },
      { 0x05, 0x00 },
      { 0x11, 0x60 },
      { 0x40, 0x42 },
      { 0x41, 0x70 },
      { 0x42, 0x70 },
      { 0x43, 0x1B },
      { 0x44, 0x1B },
      { 0x45, 0x00 },
      { 0x46, 0x00 },
      { 0x47, 0x00 },
      { 0x48, 0x00 },
      { 0x49, 0x00 },
      { 0x4A, 0x00 },
      { 0x4B, 0x00 },
      { 0x4C, 0xFF },
      { 0x01, 0x14 }
    };

    for (const auto& item : registers) {
      if (!writeRegister(ES7210_ADDR, item.reg, item.value)) {
        return false;
      }
    }

    // Verify a few stable key values instead of assuming the writes succeeded.
    uint8_t clockControl = 0;
    uint8_t mic1Gain = 0;
    uint8_t mic2Gain = 0;
    uint8_t mic12Power = 0;
    uint8_t mic34Power = 0;

    if (!readRegister(ES7210_ADDR, 0x01, clockControl) ||
        !readRegister(ES7210_ADDR, 0x43, mic1Gain) ||
        !readRegister(ES7210_ADDR, 0x44, mic2Gain) ||
        !readRegister(ES7210_ADDR, 0x4B, mic12Power) ||
        !readRegister(ES7210_ADDR, 0x4C, mic34Power)) {
      return false;
    }

    return clockControl == 0x14 &&
           mic1Gain == 0x1B &&
           mic2Gain == 0x1B &&
           mic12Power == 0x00 &&
           mic34Power == 0xFF;
  }

  // ---------------------------------------------------------
  // Deferred initialization
  //
  // Audio initialization is delayed until all WLED usermods have
  // completed setup. This avoids depending on usermod registration
  // order while the existing CoreS3 Display / Power startup remains
  // unchanged.
  // ---------------------------------------------------------

  void attemptInitialization()
  {
    if (!audioPinsReserved) {
      Serial.println(F("[CoreS3_Audio] ERROR: audio pin reservation unavailable; codec initialization blocked"));
      initializationFinished = true;
      return;
    }

    initAttemptCount++;

    Serial.printf(
      "[CoreS3_Audio] Initialization attempt %u/%u\n",
      initAttemptCount,
      AUDIO_INIT_MAX_ATTEMPTS
    );

    if (i2c_sda != CORES3_I2C_SDA || i2c_scl != CORES3_I2C_SCL) {
      Serial.printf(
        "[CoreS3_Audio] ERROR: invalid CoreS3 I2C pins SDA=%d SCL=%d\n",
        i2c_sda,
        i2c_scl
      );

      coreS3PinsValid = false;
      initializationFinished = true;
      return;
    }

    coreS3PinsValid = true;

    // Read-only PMU diagnostics. Phase 10.4.2 deliberately does not
    // change the ES7210 power rail; Power ownership remains separate.
    // Reading AXP2101 register 0x90 simultaneously verifies that the
    // shared M5GFX I2C_NUM_1 bus is reachable.
    axp2101RegistersRead = false;
    axp2101Found = readRegister(AXP2101_ADDR, 0x90, axp2101Reg90);

    if (axp2101Found) {
      axp2101RegistersRead =
        readRegister(AXP2101_ADDR, 0x93, axp2101Reg93);

      Serial.printf(
        "[CoreS3_Audio] AXP2101 via M5GFX I2C1: FOUND%s\n",
        axp2101RegistersRead ? "" : " (REG93 unavailable)"
      );

      if (axp2101RegistersRead) {
        Serial.printf(
          "[CoreS3_Audio] AXP2101 audio rail diagnostic: REG90=0x%02X REG93=0x%02X\n",
          axp2101Reg90,
          axp2101Reg93
        );
      }
    }
    else {
      Serial.println(F("[CoreS3_Audio] AXP2101 via M5GFX I2C1: NOT FOUND"));
    }

    // ES7210 register 0x00 is RESET_CTL and is safe to read.
    // This probe does not modify the codec or its power rail.
    es7210Found = readRegister(ES7210_ADDR, 0x00, es7210ProbeReg00);

    Serial.printf(
      "[CoreS3_Audio] ES7210 via M5GFX I2C1 (0x40): %s",
      es7210Found ? "FOUND" : "NOT FOUND"
    );

    if (es7210Found) {
      Serial.printf(" (REG00=0x%02X)", es7210ProbeReg00);
    }

    Serial.println();

    if (!es7210Found) {
      if (initAttemptCount >= AUDIO_INIT_MAX_ATTEMPTS) {
        Serial.println(
          F("[CoreS3_Audio] ES7210 unavailable after retries; microphone initialization stopped")
        );
        initializationFinished = true;
      }

      return;
    }

    es7210Configured = configureES7210();

    Serial.printf(
      "[CoreS3_Audio] ES7210 configuration: %s\n",
      es7210Configured ? "VERIFIED" : "FAILED"
    );

    if (!es7210Configured) {
      initializationFinished = true;
      return;
    }

    coreS3AudioCodecReadyState = true;
    initializationFinished = true;

    Serial.println(F("[CoreS3_Audio] ES7210 READY"));
    Serial.println(F("[CoreS3_Audio] Waiting for AudioReactive to claim I2S_NUM_1"));
  }

  const char* getAudioStatusName() const
  {
    if (!initializationFinished) {
      return "INITIALIZING";
    }

    if (!audioPinsReserved) {
      return "AUDIO PIN RESERVATION ERROR";
    }

    if (!coreS3PinsValid) {
      return "I2C PIN ERROR";
    }

    if (!es7210Found) {
      return "ES7210 NOT FOUND";
    }

    if (!es7210Configured) {
      return "ES7210 CONFIG FAILED";
    }

#if defined(WLED_M5STACK_CORES3_AUDIO)
    if (coreS3AudioReactiveSourceReady()) {
      return "READY - AudioReactive";
    }

    return "CODEC READY - waiting AudioReactive";
#else
    return "AUDIOREACTIVE BUILD FLAG MISSING";
#endif
  }

public:
  // ---------------------------------------------------------
  // WLED Usermod setup
  // ---------------------------------------------------------

  void setup() override
  {
    Serial.println();
    Serial.println(F("[CoreS3_Audio] Phase 10.4.2P-V17e start"));

    // Diagnostic only: records why the previous boot ended and confirms
    // that the V17 Quad-PSRAM configuration is active.
    printBootDiagnostics();

    Serial.println(F("[CoreS3_Audio] Built-in microphone / Audio Reactive integration"));

    Serial.printf(
      "[CoreS3_Audio] WLED I2C pins: SDA=%d SCL=%d\n",
      i2c_sda,
      i2c_scl
    );

    Serial.println(
      F("[CoreS3_Audio] Internal codec access: M5GFX I2C_NUM_1 GPIO12/GPIO11")
    );

    Serial.println(
      F("[CoreS3_Audio] Audio will not reinitialize the shared internal I2C bus")
    );

    Serial.println(F("[CoreS3_Audio] Audio hardware initialization is deferred"));

    audioPinsReserved = reserveInternalAudioPins();

    Serial.printf(
      "[CoreS3_Audio] Internal audio pin reservation: %s (GPIO0=MCLK GPIO14=DIN)\n",
      audioPinsReserved ? "READY" : "FAILED"
    );

    setupStartMs = millis();
    lastInitAttemptMs = setupStartMs;

    // Start heartbeat timing from the end of Usermod setup.
    lastDiagMs = setupStartMs;
  }

  // ---------------------------------------------------------
  // WLED Usermod loop
  // ---------------------------------------------------------

  void loop() override
  {
    const unsigned long now = millis();

    // Phase 10.4.2P-V17e runtime heartbeat.
    // Intentionally executed before the deferred-initialization returns
    // so a long-running initialization problem remains visible.
    if (now - lastDiagMs >= DIAG_INTERVAL_MS) {
      lastDiagMs = now;
      printRuntimeDiagnostics(now);
    }

    if (!initializationFinished) {
      if (now - setupStartMs < AUDIO_INIT_DELAY_MS) {
        return;
      }

      if (initAttemptCount == 0 ||
          now - lastInitAttemptMs >= AUDIO_INIT_RETRY_MS) {
        lastInitAttemptMs = now;
        attemptInitialization();
      }

      return;
    }

    // No periodic PCM work here.
    // Audio Reactive owns I2S1 sampling and FFT processing.
  }

  // ---------------------------------------------------------
  // WLED Info diagnostics
  // ---------------------------------------------------------

  void addToJsonInfo(JsonObject& root) override
  {
    JsonObject user = root["u"];

    if (user.isNull()) {
      user = root.createNestedObject("u");
    }

    JsonArray phaseInfo = user.createNestedArray("CoreS3 Audio Phase");
    phaseInfo.add("10.4.2P-V17e");

    JsonArray statusInfo = user.createNestedArray("CoreS3 Audio");
    statusInfo.add(getAudioStatusName());

    JsonArray i2cInfo = user.createNestedArray("CoreS3 Audio I2C");
    i2cInfo.add("M5GFX I2C1 GPIO12/GPIO11 400kHz");

    JsonArray codecInfo = user.createNestedArray("CoreS3 ES7210");

    if (!initializationFinished && !es7210Found) {
      codecInfo.add("Waiting for probe");
    }
    else if (!es7210Found) {
      codecInfo.add("Not found on M5GFX I2C1 (0x40)");
    }
    else if (!es7210Configured) {
      codecInfo.add("Found - config failed");
    }
    else {
      codecInfo.add("Found / configured / verified on I2C1");
    }

    JsonArray integrationInfo = user.createNestedArray("CoreS3 Audio Integration");

#if defined(WLED_M5STACK_CORES3_AUDIO)
    integrationInfo.add(
      coreS3AudioReactiveSourceReady()
        ? "AudioReactive source READY"
        : (coreS3AudioCodecReadyState
            ? "Codec ready / waiting I2S1 source"
            : "Waiting for ES7210 codec")
    );
#else
    integrationInfo.add("AudioReactive CoreS3 build flag missing");
#endif

    JsonArray i2sInfo = user.createNestedArray("CoreS3 Audio I2S");
    i2sInfo.add("AudioReactive owner: I2S1 Stereo 16bit 16000Hz");

    JsonArray pmuInfo = user.createNestedArray("CoreS3 Audio PMU");

    if (!axp2101Found) {
      pmuInfo.add("AXP2101 not found on M5GFX I2C1");
    }
    else if (!axp2101RegistersRead) {
      pmuInfo.add("AXP2101 found / registers unavailable");
    }
    else {
      char pmuText[48];

      snprintf(
        pmuText,
        sizeof(pmuText),
        "REG90=0x%02X REG93=0x%02X",
        axp2101Reg90,
        axp2101Reg93
      );

      pmuInfo.add(pmuText);
    }

    JsonArray pinReservationInfo =
      user.createNestedArray("CoreS3 Audio Pin Reservation");

    if (audioPinsReserved) {
      pinReservationInfo.add("GPIO0 MCLK / GPIO14 DIN RESERVED (Usermod)");
    }
    else {
      char reservationText[80];

      snprintf(
        reservationText,
        sizeof(reservationText),
        "FAILED: GPIO0=%s GPIO14=%s",
        PinManager::getPinOwnerName(AUDIO_MCLK_PIN),
        PinManager::getPinOwnerName(AUDIO_DATA_IN_PIN)
      );

      pinReservationInfo.add(reservationText);
    }

    JsonArray pinInfo = user.createNestedArray("CoreS3 Audio Pins");
    pinInfo.add("FIXED: MCLK0 BCLK34 WS33 DIN14");

    JsonArray diagInfo = user.createNestedArray("CoreS3 V17 Diagnostics");

    char resetText[48];
    const esp_reset_reason_t resetReason = esp_reset_reason();

    snprintf(
      resetText,
      sizeof(resetText),
      "Reset=%d %s",
      static_cast<int>(resetReason),
      getResetReasonName(resetReason)
    );

    diagInfo.add(resetText);

    char psramText[64];
    const size_t psramTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    snprintf(
      psramText,
      sizeof(psramText),
      "PSRAM=%lu total / %lu free",
      static_cast<unsigned long>(psramTotal),
      static_cast<unsigned long>(psramFree)
    );

    diagInfo.add(psramText);
  }
};

// -----------------------------------------------------------
// Register CoreS3 Audio Usermod with WLED
// -----------------------------------------------------------

static CoreS3AudioUsermod coreS3AudioUsermod;
REGISTER_USERMOD(coreS3AudioUsermod);
