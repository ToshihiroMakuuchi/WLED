from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parent

TARGET = (
    ROOT
    / ".pio"
    / "libdeps"
    / "m5stack_cores3"
    / "NeoPixelBus"
    / "src"
    / "internal"
    / "methods"
    / "ESP"
    / "ESP32"
    / "NeoEsp32RmtXMethod.h"
)

print("============================================================")
print("WLED CoreS3 V17 - NeoPixelBus RMT diagnostic patch")
print("Phase 10.4.2P-V17g-RMT-DIAG")
print("============================================================")
print(f"Target: {TARGET}")

if not TARGET.exists():
    print("ERROR: target file not found.")
    sys.exit(1)

text = TARGET.read_text(encoding="utf-8")

marker = "[NPB_RMT_DIAG]"
if marker in text:
    print("Patch is already applied.")
    sys.exit(0)

backup = TARGET.with_suffix(TARGET.suffix + ".v17g_diag_backup")
if not backup.exists():
    shutil.copy2(TARGET, backup)
    print(f"Backup: {backup}")

old_destructor = '''    ~NeoEsp32RmtMethodBase()
    {
        // wait until the last send finishes before destructing everything
        // arbitrary time out of 10 seconds

        ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_tx_wait_all_done(_channel, 10000 / portTICK_PERIOD_MS));
        ESP_ERROR_CHECK(rmt_disable(_channel));
        ESP_ERROR_CHECK(rmt_del_channel(_channel));

        gpio_matrix_out(_pin, 0x100, false, false);
        pinMode(_pin, INPUT);

        free(_dataEditing);
        free(_dataSending);
    }
'''

new_destructor = '''    ~NeoEsp32RmtMethodBase()
    {
        // Phase 10.4.2P-V17g:
        // Do not call IDF RMT APIs with an invalid/null channel handle.
        if (_channel != nullptr)
        {
            ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_tx_wait_all_done(_channel, 10000 / portTICK_PERIOD_MS));
            ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_disable(_channel));
            ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_del_channel(_channel));
            _channel = nullptr;
        }

        gpio_matrix_out(_pin, 0x100, false, false);
        pinMode(_pin, INPUT);

        free(_dataEditing);
        free(_dataSending);
    }
'''

if old_destructor not in text:
    print("ERROR: destructor block not found.")
    sys.exit(2)

text = text.replace(old_destructor, new_destructor, 1)

old_ready = '''    bool IsReadyToUpdate() const
    {
        return (ESP_OK == rmt_tx_wait_all_done(_channel, 0));
    }
'''

new_ready = '''    bool IsReadyToUpdate() const
    {
        if (_channel == nullptr)
        {
            return false;
        }
        return (ESP_OK == rmt_tx_wait_all_done(_channel, 0));
    }
'''

if old_ready not in text:
    print("ERROR: IsReadyToUpdate block not found.")
    sys.exit(3)

text = text.replace(old_ready, new_ready, 1)

start = text.find("    void Initialize()\n    {")
end = text.find("\n    void Update(bool maintainBufferConsistency)", start)

if start < 0 or end < 0:
    print("ERROR: Initialize() boundaries not found.")
    sys.exit(4)

new_initialize = '''    void Initialize()
    {
        rmt_tx_channel_config_t config = {};
        config.clk_src = RMT_CLK_SRC_DEFAULT;
        config.gpio_num = static_cast<gpio_num_t>(_pin);

#if defined(CONFIG_IDF_TARGET_ESP32S3)
        // ESP32-S3 owns 48 symbols per RMT TX memory block.
        config.mem_block_symbols = 48;
#else
        config.mem_block_symbols = 192;
#endif

        config.resolution_hz = T_SPEED::RmtTicksPerSecond;
        config.trans_queue_depth = 4;
        config.flags.invert_out = T_INVERTED::Inverted;
        config.flags.with_dma = false;

        esp_err_t channelResult = rmt_new_tx_channel(&config, &_channel);

        Serial.printf(
            "[NPB_RMT_DIAG] pin=%u new_tx=%d (%s) channel=%p mem=%u\\n",
            static_cast<unsigned>(_pin),
            static_cast<int>(channelResult),
            esp_err_to_name(channelResult),
            static_cast<void*>(_channel),
            static_cast<unsigned>(config.mem_block_symbols)
        );

        if (channelResult != ESP_OK || _channel == nullptr)
        {
            Serial.printf(
                "[NPB_RMT_DIAG] pin=%u DISABLED: RMT TX channel allocation failed\\n",
                static_cast<unsigned>(_pin)
            );
            _channel = nullptr;
            return;
        }

        led_strip_encoder_config_t encoder_config = {};
        encoder_config.resolution = T_SPEED::RmtTicksPerSecond;

        _tx_config.loop_count = 0;

        esp_err_t encoderResult =
            rmt_new_led_strip_encoder(
                &encoder_config,
                &_led_encoder,
                T_SPEED::RmtBit0,
                T_SPEED::RmtBit1
            );

        Serial.printf(
            "[NPB_RMT_DIAG] pin=%u encoder=%d (%s) encoder_ptr=%p\\n",
            static_cast<unsigned>(_pin),
            static_cast<int>(encoderResult),
            esp_err_to_name(encoderResult),
            static_cast<void*>(_led_encoder)
        );

        if (encoderResult != ESP_OK || _led_encoder == nullptr)
        {
            Serial.printf(
                "[NPB_RMT_DIAG] pin=%u DISABLED: encoder creation failed\\n",
                static_cast<unsigned>(_pin)
            );

            ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_del_channel(_channel));
            _channel = nullptr;
            _led_encoder = nullptr;
            return;
        }

        esp_err_t enableResult = rmt_enable(_channel);

        Serial.printf(
            "[NPB_RMT_DIAG] pin=%u enable=%d (%s)\\n",
            static_cast<unsigned>(_pin),
            static_cast<int>(enableResult),
            esp_err_to_name(enableResult)
        );

        if (enableResult != ESP_OK)
        {
            Serial.printf(
                "[NPB_RMT_DIAG] pin=%u DISABLED: rmt_enable failed\\n",
                static_cast<unsigned>(_pin)
            );

            rmt_del_encoder(_led_encoder);
            _led_encoder = nullptr;
            ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_del_channel(_channel));
            _channel = nullptr;
            return;
        }

        Serial.printf(
            "[NPB_RMT_DIAG] pin=%u READY\\n",
            static_cast<unsigned>(_pin)
        );
    }
'''

text = text[:start] + new_initialize + text[end:]

old_update_head = '''    void Update(bool maintainBufferConsistency)
    {
        // AddLog(2,"..");
'''

new_update_head = '''    void Update(bool maintainBufferConsistency)
    {
        // Phase 10.4.2P-V17g:
        // A failed RMT allocation leaves this bus disabled instead of
        // repeatedly calling ESP-IDF with a null handle.
        if (_channel == nullptr || _led_encoder == nullptr)
        {
            return;
        }

        // AddLog(2,"..");
'''

if old_update_head not in text:
    print("ERROR: Update() head not found.")
    sys.exit(5)

text = text.replace(old_update_head, new_update_head, 1)

TARGET.write_text(text, encoding="utf-8", newline="\n")

verify = TARGET.read_text(encoding="utf-8")
required = [
    "[NPB_RMT_DIAG] pin=%u new_tx=",
    "config.mem_block_symbols = 48;",
    "if (_channel == nullptr)",
    "if (_channel == nullptr || _led_encoder == nullptr)",
]

missing = [item for item in required if item not in verify]
if missing:
    print("ERROR: verification failed:")
    for item in missing:
        print(f"  missing: {item}")
    sys.exit(6)

print()
print("SUCCESS: V17g RMT diagnostic/fail-safe patch applied.")
print()
print("Expected boot log examples:")
print("  [NPB_RMT_DIAG] pin=2 new_tx=0 (ESP_OK) channel=... mem=48")
print("  [NPB_RMT_DIAG] pin=2 encoder=0 (ESP_OK) encoder_ptr=...")
print("  [NPB_RMT_DIAG] pin=2 enable=0 (ESP_OK)")
print("  [NPB_RMT_DIAG] pin=2 READY")
print()
print("or, on failure:")
print("  [NPB_RMT_DIAG] pin=<GPIO> new_tx=<error> (...) channel=0x0 mem=48")
print("  [NPB_RMT_DIAG] pin=<GPIO> DISABLED: RMT TX channel allocation failed")
print()
print("Next:")
print("  1. PlatformIO Build")
print("  2. PlatformIO Upload")
print("  3. Capture all [NPB_RMT_DIAG] lines from boot")
