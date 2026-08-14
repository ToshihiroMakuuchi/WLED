from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parent
LIBDEPS = ROOT / ".pio" / "libdeps" / "m5stack_cores3"

print("============================================================")
print("WLED CoreS3 V17 - NeoPixelBus RMT diagnostic patch")
print("Phase 10.4.2P-V17g-RMT-DIAG3")
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
    if "rmt_new_tx_channel" in t and "NeoEsp32RmtMethodBase" in t:
        candidates.append(p)

src_candidates = [p for p in candidates if "NeoPixelBus@src-" in str(p)]

if len(src_candidates) == 1:
    target = src_candidates[0]
elif len(candidates) == 1:
    target = candidates[0]
else:
    print("ERROR: could not uniquely identify active NeoEsp32RmtXMethod.h")
    for p in candidates:
        print(f"  {p}")
    sys.exit(2)

print("Selected target:")
print(f"  {target}")

text = target.read_text(encoding="utf-8")

if "[NPB_RMT_DIAG]" in text:
    print()
    print("Patch appears to be already applied.")
    sys.exit(0)

backup = target.with_suffix(target.suffix + ".v17g_diag3_backup")
if not backup.exists():
    shutil.copy2(target, backup)
    print("Backup:")
    print(f"  {backup}")

def replace_function(source: str, signature: str, replacement: str) -> str:
    sig_pos = source.find(signature)
    if sig_pos < 0:
        raise RuntimeError(f"signature not found: {signature}")

    brace_start = source.find("{", sig_pos + len(signature))
    if brace_start < 0:
        raise RuntimeError(f"opening brace not found: {signature}")

    depth = 0
    i = brace_start
    in_string = False
    in_char = False
    escape = False

    while i < len(source):
        ch = source[i]

        if escape:
            escape = False
            i += 1
            continue

        if ch == "\\" and (in_string or in_char):
            escape = True
            i += 1
            continue

        if not in_char and ch == '"':
            in_string = not in_string
            i += 1
            continue

        if not in_string and ch == "'":
            in_char = not in_char
            i += 1
            continue

        if not in_string and not in_char:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return source[:sig_pos] + replacement + source[i + 1:]

        i += 1

    raise RuntimeError(f"matching closing brace not found: {signature}")

try:
    text = replace_function(
        text,
        "~NeoEsp32RmtMethodBase()",
'''    ~NeoEsp32RmtMethodBase()
    {
        // Phase 10.4.2P-V17g:
        // Avoid passing a null RMT handle to ESP-IDF.
        if (_channel != nullptr)
        {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                rmt_tx_wait_all_done(_channel, 10000 / portTICK_PERIOD_MS)
            );
            ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_disable(_channel));
            ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_del_channel(_channel));
            _channel = nullptr;
        }

        gpio_matrix_out(_pin, 0x100, false, false);
        pinMode(_pin, INPUT);

        free(_dataEditing);
        free(_dataSending);
    }'''
    )

    text = replace_function(
        text,
        "bool IsReadyToUpdate() const",
'''    bool IsReadyToUpdate() const
    {
        if (_channel == nullptr)
        {
            return false;
        }

        return (ESP_OK == rmt_tx_wait_all_done(_channel, 0));
    }'''
    )

    text = replace_function(
        text,
        "void Initialize()",
'''    void Initialize()
    {
        rmt_tx_channel_config_t config = {};
        config.clk_src = RMT_CLK_SRC_DEFAULT;
        config.gpio_num = static_cast<gpio_num_t>(_pin);

#if defined(CONFIG_IDF_TARGET_ESP32S3)
        // ESP32-S3 has 48 symbols per RMT TX memory block.
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
            return;
        }

        Serial.printf(
            "[NPB_RMT_DIAG] pin=%u READY\\n",
            static_cast<unsigned>(_pin)
        );
    }'''
    )

    text = replace_function(
        text,
        "void Update(bool maintainBufferConsistency)",
'''    void Update(bool maintainBufferConsistency)
    {
        // If RMT initialization failed, keep this bus inert instead of
        // repeatedly calling ESP-IDF with an invalid channel handle.
        if (_channel == nullptr || _led_encoder == nullptr)
        {
            return;
        }

        if (ESP_OK == ESP_ERROR_CHECK_WITHOUT_ABORT(
                rmt_tx_wait_all_done(_channel, 10000 / portTICK_PERIOD_MS)))
        {
            rmt_transmit(
                _channel,
                _led_encoder,
                _dataEditing,
                _sizeData,
                &_tx_config
            );

            if (maintainBufferConsistency)
            {
                memcpy(_dataSending, _dataEditing, _sizeData);
            }

            std::swap(_dataSending, _dataEditing);
        }
    }'''
    )

except RuntimeError as e:
    print(f"ERROR: {e}")
    print("No file was written.")
    sys.exit(3)

target.write_text(text, encoding="utf-8", newline="\n")

verify = target.read_text(encoding="utf-8")
checks = [
    "[NPB_RMT_DIAG]",
    "config.mem_block_symbols = 48;",
    "DISABLED: rmt_new_tx_channel failed",
    "if (_channel == nullptr || _led_encoder == nullptr)",
]

missing = [c for c in checks if c not in verify]
if missing:
    print("ERROR: verification failed:")
    for c in missing:
        print(f"  missing: {c}")
    sys.exit(4)

print()
print("SUCCESS: V17g-RMT-DIAG3 patch applied.")
print()
print("Next:")
print("  1. Do NOT Clean")
print("  2. PlatformIO Build")
print("  3. PlatformIO Upload")
print("  4. Capture every [NPB_RMT_DIAG] line")
