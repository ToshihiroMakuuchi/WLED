from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parent
LIBDEPS = ROOT / ".pio" / "libdeps" / "m5stack_cores3"

print("============================================================")
print("WLED CoreS3 V17 - NeoPixelBus RMT diagnostic patch")
print("Phase 10.4.2P-V17g-RMT-DIAG2")
print("============================================================")

if not LIBDEPS.exists():
    print(f"ERROR: libdeps directory not found: {LIBDEPS}")
    sys.exit(1)

candidates = []
for p in LIBDEPS.rglob("NeoEsp32RmtXMethod.h"):
    try:
        t = p.read_text(encoding="utf-8")
    except Exception:
        continue
    if "rmt_new_tx_channel" in t and "class NeoEsp32RmtMethodBase" in t:
        candidates.append(p)

if not candidates:
    print("ERROR: active NeoEsp32RmtXMethod.h was not found.")
    sys.exit(2)

src_candidates = [p for p in candidates if "NeoPixelBus@src-" in str(p)]

if len(src_candidates) == 1:
    target = src_candidates[0]
elif len(candidates) == 1:
    target = candidates[0]
else:
    print("ERROR: multiple candidate files found; refusing to guess:")
    for p in candidates:
        print(f"  {p}")
    sys.exit(3)

print("Selected target:")
print(f"  {target}")

text = target.read_text(encoding="utf-8")

if "[NPB_RMT_DIAG]" in text:
    print()
    print("Patch is already applied.")
    sys.exit(0)

backup = target.with_suffix(target.suffix + ".v17g_diag2_backup")
if not backup.exists():
    shutil.copy2(target, backup)
    print("Backup:")
    print(f"  {backup}")

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
        // Never pass a null RMT handle into the ESP-IDF driver.
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
    print("ERROR: expected destructor block was not found.")
    sys.exit(4)

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
    print("ERROR: expected IsReadyToUpdate() block was not found.")
    sys.exit(5)

text = text.replace(old_ready, new_ready, 1)

start = text.find("    void Initialize()\n    {")
end = text.find("\n    void Update(bool maintainBufferConsistency)", start)

if start < 0 or end < 0:
    print("ERROR: Initialize()/Update() boundary was not found.")
    sys.exit(6)

new_initialize = '''    void Initialize()
    {
        rmt_tx_channel_config_t config = {};
        config.clk_src = RMT_CLK_SRC_DEFAULT;
        config.gpio_num = static_cast<gpio_num_t>(_pin);

#if defined(CONFIG_IDF_TARGET_ESP32S3)
        // ESP32-S3 has 48 RMT symbols per TX memory block.
        // Use one block per NeoPixelBus instance.
        config.mem_block_symbols = 48;
#else
        config.mem_block_symbols = 192;
#endif

        config.resolution_hz = T_SPEED::RmtTicksPerSecond;
        config.trans_queue_depth = 4;
        config.flags.invert_out = T_INVERTED::Inverted;
        config.flags.with_dma = false;

        esp_err_t ret = rmt_new_tx_channel(&config, &_channel);

        Serial.printf(
            "[NPB_RMT_DIAG] pin=%u new_tx=%d (%s) channel=%p mem=%u\\n",
            static_cast<unsigned>(_pin),
            static_cast<int>(ret),
            esp_err_to_name(ret),
            static_cast<void*>(_channel),
            static_cast<unsigned>(config.mem_block_symbols)
        );

        if (ret != ESP_OK || _channel == nullptr)
        {
            Serial.printf(
                "[NPB_RMT_DIAG] pin=%u DISABLED: rmt_new_tx_channel failed\\n",
                static_cast<unsigned>(_pin)
            );
            _channel = nullptr;
            return;
        }

        led_strip_encoder_config_t encoder_config = {};
        encoder_config.resolution = T_SPEED::RmtTicksPerSecond;

        _tx_config.loop_count = 0;

        ret = rmt_new_led_strip_encoder(
            &encoder_config,
            &_led_encoder,
            T_SPEED::RmtBit0,
            T_SPEED::RmtBit1
        );

        Serial.printf(
            "[NPB_RMT_DIAG] pin=%u encoder=%d (%s) encoder_ptr=%p\\n",
            static_cast<unsigned>(_pin),
            static_cast<int>(ret),
            esp_err_to_name(ret),
            static_cast<void*>(_led_encoder)
        );

        if (ret != ESP_OK || _led_encoder == nullptr)
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

        ret = rmt_enable(_channel);

        Serial.printf(
            "[NPB_RMT_DIAG] pin=%u enable=%d (%s)\\n",
            static_cast<unsigned>(_pin),
            static_cast<int>(ret),
            esp_err_to_name(ret)
        );

        if (ret != ESP_OK)
        {
            Serial.printf(
                "[NPB_RMT_DIAG] pin=%u DISABLED: rmt_enable failed\\n",
                static_cast<unsigned>(_pin)
            );
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
        // If initialization failed, keep this bus inert.
        if (_channel == nullptr || _led_encoder == nullptr)
        {
            return;
        }

        // AddLog(2,"..");
'''

if old_update_head not in text:
    print("ERROR: expected Update() block was not found.")
    sys.exit(7)

text = text.replace(old_update_head, new_update_head, 1)

target.write_text(text, encoding="utf-8", newline="\n")

verify = target.read_text(encoding="utf-8")
checks = [
    "[NPB_RMT_DIAG]",
    "config.mem_block_symbols = 48;",
    "if (_channel == nullptr)",
    "if (_channel == nullptr || _led_encoder == nullptr)",
]

missing = [c for c in checks if c not in verify]
if missing:
    print("ERROR: post-write verification failed:")
    for c in missing:
        print(f"  missing: {c}")
    sys.exit(8)

print()
print("SUCCESS: V17g-RMT-DIAG2 patch applied.")
print()
print("Next steps:")
print("  1. Do NOT Clean")
print("  2. PlatformIO Build")
print("  3. PlatformIO Upload")
print("  4. Capture every line containing [NPB_RMT_DIAG]")
