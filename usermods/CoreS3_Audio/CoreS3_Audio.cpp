#include "wled.h"
#include <M5GFX.h>
#include <driver/i2c.h>
#include <math.h>
#include <driver/i2s.h>
#include <esp_err.h>

// ===========================================================
// CoreS3 Audio Usermod
//
// Phase 10.4.0a
//
// Purpose
//   - Access the CoreS3 internal I2C bus through the same
//     M5GFX I2C_NUM_1 owner used by Display / Touch.
//   - Detect the CoreS3 AXP2101 and ES7210 on that shared bus.
//   - Configure the built-in dual microphones when ES7210 is powered.
//   - Capture stereo PCM from I2S_NUM_1.
//   - Calculate simple Peak / RMS diagnostic levels.
//   - Report microphone activity to Serial and WLED Info.
//
// This phase intentionally does NOT:
//   - feed samples into WLED Audio Reactive,
//   - run FFT,
//   - change LED state,
//   - modify CoreS3 Display / Touch behavior,
//   - modify CoreS3 power rails,
//   - reinitialize or take ownership of the internal I2C bus.
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
// Phase 10.4.0a change
//   CoreS3 Display/Touch uses M5GFX internal I2C_NUM_1 on GPIO12/11.
//   The original Phase 10.4.0 Audio probe used Arduino Wire (I2C0),
//   so after M5GFX initialization it could no longer see the same
//   internal devices. Audio now uses lgfx::i2c transactions on
//   I2C_NUM_1 and never calls i2c/Wire begin or release.
// ===========================================================
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

  static constexpr i2s_port_t AUDIO_I2S_PORT = I2S_NUM_1;

  static constexpr int AUDIO_MCLK_PIN = 0;
  static constexpr int AUDIO_BCLK_PIN = 34;
  static constexpr int AUDIO_WS_PIN = 33;
  static constexpr int AUDIO_DATA_IN_PIN = 14;

  static constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;

  static constexpr int AUDIO_DMA_BUFFER_COUNT = 8;
  static constexpr int AUDIO_DMA_BUFFER_FRAMES = 128;

  // 256 stereo frames = 512 x int16_t = 1024 bytes.
  static constexpr size_t AUDIO_READ_FRAMES = 256;
  static constexpr size_t AUDIO_CHANNEL_COUNT = 2;
  static constexpr size_t AUDIO_READ_SAMPLES = AUDIO_READ_FRAMES * AUDIO_CHANNEL_COUNT;

  static constexpr unsigned long AUDIO_INIT_DELAY_MS = 2500;
  static constexpr unsigned long AUDIO_INIT_RETRY_MS = 1000;
  static constexpr uint8_t AUDIO_INIT_MAX_ATTEMPTS = 5;
  static constexpr unsigned long AUDIO_SERIAL_REPORT_MS = 500;

  // ---------------------------------------------------------
  // Phase 10.4.0a runtime state
  // ---------------------------------------------------------

  bool coreS3PinsValid = false;
  bool es7210Found = false;
  bool es7210Configured = false;
  bool i2sAttempted = false;
  bool i2sInstalled = false;
  bool audioReady = false;
  bool initializationFinished = false;

  uint8_t initAttemptCount = 0;

  unsigned long setupStartMs = 0;
  unsigned long lastInitAttemptMs = 0;
  unsigned long lastSerialReportMs = 0;

  esp_err_t lastI2sError = ESP_OK;
  float actualSampleRate = 0.0f;

  bool axp2101Found = false;
  bool axp2101RegistersRead = false;
  uint8_t axp2101Reg90 = 0;
  uint8_t axp2101Reg93 = 0;
  uint8_t es7210ProbeReg00 = 0;

  int16_t sampleBuffer[AUDIO_READ_SAMPLES];

  uint16_t latestPeakLeft = 0;
  uint16_t latestPeakRight = 0;
  uint16_t latestRmsLeft = 0;
  uint16_t latestRmsRight = 0;
  uint16_t latestRmsCombined = 0;

  uint32_t audioBlocksReceived = 0;
  uint32_t audioReadErrors = 0;

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
  // I2S_NUM_1 setup
  //
  // Important safety behavior:
  // If I2S_NUM_1 is already owned by another subsystem, this
  // usermod reports the conflict and DOES NOT uninstall it.
  // ---------------------------------------------------------

  bool initializeI2S()
  {
    i2s_config_t config = {};

    config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    config.sample_rate = AUDIO_SAMPLE_RATE;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = AUDIO_DMA_BUFFER_COUNT;
    config.dma_buf_len = AUDIO_DMA_BUFFER_FRAMES;
    config.use_apll = false;
    config.tx_desc_auto_clear = false;
    config.fixed_mclk = 0;
    config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    config.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

    i2sAttempted = true;

    lastI2sError = i2s_driver_install(AUDIO_I2S_PORT, &config, 0, nullptr);

    if (lastI2sError != ESP_OK) {
      Serial.printf(
        "[CoreS3_Audio] I2S_NUM_1 install failed: %s (%d)\n",
        esp_err_to_name(lastI2sError),
        (int)lastI2sError
      );

      return false;
    }

    i2sInstalled = true;

    i2s_pin_config_t pinConfig = {};

    pinConfig.mck_io_num = AUDIO_MCLK_PIN;
    pinConfig.bck_io_num = AUDIO_BCLK_PIN;
    pinConfig.ws_io_num = AUDIO_WS_PIN;
    pinConfig.data_out_num = I2S_PIN_NO_CHANGE;
    pinConfig.data_in_num = AUDIO_DATA_IN_PIN;

    lastI2sError = i2s_set_pin(AUDIO_I2S_PORT, &pinConfig);

    if (lastI2sError != ESP_OK) {
      Serial.printf(
        "[CoreS3_Audio] I2S pin setup failed: %s (%d)\n",
        esp_err_to_name(lastI2sError),
        (int)lastI2sError
      );

      i2s_driver_uninstall(AUDIO_I2S_PORT);
      i2sInstalled = false;

      return false;
    }

    lastI2sError = i2s_set_clk(
      AUDIO_I2S_PORT,
      AUDIO_SAMPLE_RATE,
      I2S_BITS_PER_SAMPLE_16BIT,
      I2S_CHANNEL_STEREO
    );

    if (lastI2sError != ESP_OK) {
      Serial.printf(
        "[CoreS3_Audio] I2S clock setup failed: %s (%d)\n",
        esp_err_to_name(lastI2sError),
        (int)lastI2sError
      );

      i2s_driver_uninstall(AUDIO_I2S_PORT);
      i2sInstalled = false;

      return false;
    }

    actualSampleRate = i2s_get_clk(AUDIO_I2S_PORT);

    Serial.printf(
      "[CoreS3_Audio] I2S_NUM_1 ready: requested=%lu Hz actual=%.1f Hz\n",
      (unsigned long)AUDIO_SAMPLE_RATE,
      actualSampleRate
    );

    Serial.printf(
      "[CoreS3_Audio] I2S pins: MCLK=%d BCLK=%d WS=%d DATA=%d\n",
      AUDIO_MCLK_PIN,
      AUDIO_BCLK_PIN,
      AUDIO_WS_PIN,
      AUDIO_DATA_IN_PIN
    );

    return true;
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

    // Read-only PMU diagnostics. Phase 10.4.0a deliberately does not
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

    if (!initializeI2S()) {
      initializationFinished = true;
      return;
    }

    audioReady = true;
    initializationFinished = true;
    lastSerialReportMs = millis();

    Serial.println(F("[CoreS3_Audio] Built-in dual microphone capture READY"));
    Serial.println(F("[CoreS3_Audio] Phase 10.4.0a diagnostic capture active"));
  }

  // ---------------------------------------------------------
  // PCM level calculation
  // ---------------------------------------------------------

  void processAudioBlock(const int16_t* samples, size_t sampleCount)
  {
    if (samples == nullptr || sampleCount < 2) {
      return;
    }

    size_t frameCount = sampleCount / AUDIO_CHANNEL_COUNT;

    if (frameCount == 0) {
      return;
    }

    // Remove block DC offset before RMS / Peak calculation.
    int64_t sumLeft = 0;
    int64_t sumRight = 0;

    for (size_t frame = 0; frame < frameCount; frame++) {
      sumLeft += samples[(frame * 2)];
      sumRight += samples[(frame * 2) + 1];
    }

    int32_t meanLeft = (int32_t)(sumLeft / (int64_t)frameCount);
    int32_t meanRight = (int32_t)(sumRight / (int64_t)frameCount);

    uint32_t peakLeft = 0;
    uint32_t peakRight = 0;

    uint64_t squareSumLeft = 0;
    uint64_t squareSumRight = 0;

    for (size_t frame = 0; frame < frameCount; frame++) {
      int32_t left = (int32_t)samples[(frame * 2)] - meanLeft;
      int32_t right = (int32_t)samples[(frame * 2) + 1] - meanRight;

      uint32_t absLeft = (uint32_t)(left < 0 ? -left : left);
      uint32_t absRight = (uint32_t)(right < 0 ? -right : right);

      if (absLeft > peakLeft) {
        peakLeft = absLeft;
      }

      if (absRight > peakRight) {
        peakRight = absRight;
      }

      squareSumLeft += (uint64_t)((int64_t)left * (int64_t)left);
      squareSumRight += (uint64_t)((int64_t)right * (int64_t)right);
    }

    double rmsLeft = sqrt((double)squareSumLeft / (double)frameCount);
    double rmsRight = sqrt((double)squareSumRight / (double)frameCount);
    double rmsCombined = sqrt(
      (double)(squareSumLeft + squareSumRight) /
      (double)(frameCount * AUDIO_CHANNEL_COUNT)
    );

    latestPeakLeft = (uint16_t)constrain((int)peakLeft, 0, 32768);
    latestPeakRight = (uint16_t)constrain((int)peakRight, 0, 32768);
    latestRmsLeft = (uint16_t)constrain((int)lround(rmsLeft), 0, 32768);
    latestRmsRight = (uint16_t)constrain((int)lround(rmsRight), 0, 32768);
    latestRmsCombined = (uint16_t)constrain((int)lround(rmsCombined), 0, 32768);

    audioBlocksReceived++;
  }

  void serviceAudioCapture()
  {
    if (!audioReady || !i2sInstalled) {
      return;
    }

    size_t bytesRead = 0;

    esp_err_t result = i2s_read(
      AUDIO_I2S_PORT,
      sampleBuffer,
      sizeof(sampleBuffer),
      &bytesRead,
      0
    );

    if (result != ESP_OK) {
      if (result != ESP_ERR_TIMEOUT) {
        audioReadErrors++;
        lastI2sError = result;
      }
      return;
    }

    if (bytesRead < (sizeof(int16_t) * AUDIO_CHANNEL_COUNT)) {
      return;
    }

    size_t samplesRead = bytesRead / sizeof(int16_t);
    samplesRead -= samplesRead % AUDIO_CHANNEL_COUNT;

    processAudioBlock(sampleBuffer, samplesRead);
  }

  void reportAudioLevel()
  {
    if (!audioReady) {
      return;
    }

    Serial.printf(
      "[CoreS3_Audio] PCM RMS=%u  L=%u R=%u  Peak L=%u R=%u  Blocks=%lu Errors=%lu\n",
      latestRmsCombined,
      latestRmsLeft,
      latestRmsRight,
      latestPeakLeft,
      latestPeakRight,
      (unsigned long)audioBlocksReceived,
      (unsigned long)audioReadErrors
    );
  }

  const char* getAudioStatusName() const
  {
    if (audioReady) {
      return "READY - PCM diagnostic capture";
    }

    if (!coreS3PinsValid && initializationFinished) {
      return "I2C PIN ERROR";
    }

    if (initializationFinished && !es7210Found) {
      return "ES7210 NOT FOUND";
    }

    if (initializationFinished && !es7210Configured) {
      return "ES7210 CONFIG FAILED";
    }

    if (initializationFinished && !i2sInstalled) {
      return "I2S INIT FAILED";
    }

    return "INITIALIZING";
  }

public:
  // ---------------------------------------------------------
  // WLED Usermod setup
  // ---------------------------------------------------------

  void setup() override
  {
    Serial.println();
    Serial.println(F("[CoreS3_Audio] Phase 10.4.0a start"));
    Serial.println(F("[CoreS3_Audio] Built-in microphone foundation"));

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

    setupStartMs = millis();
    lastInitAttemptMs = setupStartMs;
  }

  // ---------------------------------------------------------
  // WLED Usermod loop
  // ---------------------------------------------------------

  void loop() override
  {
    unsigned long now = millis();

    if (!initializationFinished) {
      if (now - setupStartMs < AUDIO_INIT_DELAY_MS) {
        return;
      }

      if (initAttemptCount == 0 || now - lastInitAttemptMs >= AUDIO_INIT_RETRY_MS) {
        lastInitAttemptMs = now;
        attemptInitialization();
      }

      return;
    }

    if (!audioReady) {
      return;
    }

    serviceAudioCapture();

    if (now - lastSerialReportMs >= AUDIO_SERIAL_REPORT_MS) {
      lastSerialReportMs = now;
      reportAudioLevel();
    }
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
    phaseInfo.add("10.4.0a");

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

    JsonArray i2sInfo = user.createNestedArray("CoreS3 Audio I2S");

    if (i2sInstalled) {
      char i2sText[64];
      snprintf(
        i2sText,
        sizeof(i2sText),
        "I2S1 Stereo 16bit %luHz",
        (unsigned long)AUDIO_SAMPLE_RATE
      );
      i2sInfo.add(i2sText);
    }
    else if (i2sAttempted) {
      char errorText[64];
      snprintf(
        errorText,
        sizeof(errorText),
        "Not ready: %s (%d)",
        esp_err_to_name(lastI2sError),
        (int)lastI2sError
      );
      i2sInfo.add(errorText);
    }
    else if (initializationFinished) {
      i2sInfo.add("Not attempted");
    }
    else {
      i2sInfo.add("Waiting");
    }

    JsonArray levelInfo = user.createNestedArray("CoreS3 Mic RMS");

    if (audioReady) {
      char levelText[64];
      snprintf(
        levelText,
        sizeof(levelText),
        "%u (L:%u R:%u)",
        latestRmsCombined,
        latestRmsLeft,
        latestRmsRight
      );
      levelInfo.add(levelText);
    }
    else {
      levelInfo.add("No PCM yet");
    }

    JsonArray peakInfo = user.createNestedArray("CoreS3 Mic Peak");

    if (audioReady) {
      char peakText[48];
      snprintf(
        peakText,
        sizeof(peakText),
        "L:%u R:%u",
        latestPeakLeft,
        latestPeakRight
      );
      peakInfo.add(peakText);
    }
    else {
      peakInfo.add("No PCM yet");
    }

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

    JsonArray pinInfo = user.createNestedArray("CoreS3 Audio Pins");
    pinInfo.add("MCLK0 BCLK34 WS33 DIN14");
  }
};

// -----------------------------------------------------------
// Register CoreS3 Audio Usermod with WLED
// -----------------------------------------------------------

static CoreS3AudioUsermod coreS3AudioUsermod;
REGISTER_USERMOD(coreS3AudioUsermod);
