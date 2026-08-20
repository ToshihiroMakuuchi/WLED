# Phase 10.4.6p-RMT-DIAG - M5Stack CoreS3 / WLED V17 NeoPixelBus diagnostic patch
#
# This PlatformIO PRE script preserves the verified CoreS3 NeoPixelBus/RMT
# stabilization changes and changes only the ESP32-S3 RMT memory allocation
# from 48 symbols to 96 symbols for diagnostic testing.
#
# Diagnostic purpose:
#   Investigate the observed LED-tail anomaly where LEDs beyond the configured
#   logical count can light when a frame is turned OFF.
#
# Applied behavior:
#   1) ESP32-S3 uses 96 RMT symbols per TX channel for this diagnostic test.
#      This is two ESP32-S3 RMT memory blocks (48 symbols per block).
#   2) Other ESP32 targets remain at 192 symbols.
#   3) IsReadyToUpdate() no longer calls rmt_tx_wait_all_done(..., 0), avoiding
#      the ESP-IDF 5.5 "flush timeout" log flood and its runtime overhead.
#   4) Initialize(), Update(), and destructor guard invalid handles so a failed
#      allocation does not cascade into invalid RMT API calls.
#
# IMPORTANT:
#   - Diagnostic only. Do not Git-save as the stable baseline yet.
#   - The unique marker intentionally differs from the existing 48-symbol
#     stable patch so a normal PlatformIO build will re-patch an already
#     modified NeoEsp32RmtXMethod.h from 48 -> 96.
#
# The patch remains idempotent once this diagnostic marker is present.

from pathlib import Path

Import("env")

MARKER = "CoreS3 V17 RMT diagnostic 96-symbol patch 10.4.6p"


def _replace_function(source: str, signature: str, replacement: str) -> str:
    """Replace one C++ function body by matching balanced braces."""
    sig_pos = source.find(signature)
    if sig_pos < 0:
        raise RuntimeError(f"signature not found: {signature}")

    brace_start = source.find("{", sig_pos + len(signature))
    if brace_start < 0:
        raise RuntimeError(f"opening brace not found: {signature}")

    depth = 0
    in_string = False
    in_char = False
    escape = False
    line_comment = False
    block_comment = False
    i = brace_start

    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""

        if line_comment:
            if ch == "\n":
                line_comment = False
            i += 1
            continue

        if block_comment:
            if ch == "*" and nxt == "/":
                block_comment = False
                i += 2
                continue
            i += 1
            continue

        if escape:
            escape = False
            i += 1
            continue

        if (in_string or in_char) and ch == "\\":
            escape = True
            i += 1
            continue

        if not in_string and not in_char:
            if ch == "/" and nxt == "/":
                line_comment = True
                i += 2
                continue
            if ch == "/" and nxt == "*":
                block_comment = True
                i += 2
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


def _find_target() -> Path:
    libdeps_root = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV")

    candidates = []
    for path in libdeps_root.rglob("NeoEsp32RmtXMethod.h"):
        try:
            text = path.read_text(encoding="utf-8")
        except Exception:
            continue

        if "NeoEsp32RmtMethodBase" in text and "rmt_new_tx_channel" in text:
            candidates.append(path)

    preferred = [p for p in candidates if "NeoPixelBus@src-" in str(p)]

    if len(preferred) == 1:
        return preferred[0]
    if len(candidates) == 1:
        return candidates[0]

    detail = "\n".join(f"  {p}" for p in candidates) or "  <none>"
    raise RuntimeError(
        "CoreS3 RMT patch could not uniquely locate NeoEsp32RmtXMethod.h:\n" + detail
    )


def _apply_patch() -> None:
    target = _find_target()
    text = target.read_text(encoding="utf-8")

    # Fast path: this 96-symbol diagnostic patch is already present.
    if MARKER in text:
        print(f"[CoreS3 RMT DIAG96] diagnostic patch already present: {target.name}")
        return

    destructor = f'''    ~NeoEsp32RmtMethodBase()
    {{
        // {MARKER}
        if (_channel != nullptr)
        {{
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                rmt_tx_wait_all_done(_channel, 10000 / portTICK_PERIOD_MS)
            );
            ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_disable(_channel));
        }}

        if (_led_encoder != nullptr)
        {{
            ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_del_encoder(_led_encoder));
            _led_encoder = nullptr;
        }}

        if (_channel != nullptr)
        {{
            ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_del_channel(_channel));
            _channel = nullptr;
        }}

        gpio_matrix_out(_pin, 0x100, false, false);
        pinMode(_pin, INPUT);

        free(_dataEditing);
        free(_dataSending);
    }}'''

    ready = f'''    bool IsReadyToUpdate() const
    {{
        // {MARKER}
        // ESP-IDF 5.5 emits an error log when timeout=0 is used as a busy poll.
        // Update() performs the actual completion wait before transmitting.
        return (_channel != nullptr && _led_encoder != nullptr);
    }}'''

    initialize = f'''    void Initialize()
    {{
        // {MARKER}
        rmt_tx_channel_config_t config = {{}};
        config.clk_src = RMT_CLK_SRC_DEFAULT;
        config.gpio_num = static_cast<gpio_num_t>(_pin);

#if defined(CONFIG_IDF_TARGET_ESP32S3)
        // Phase 10.4.6p-RMT-DIAG:
        // ESP32-S3 has 48 symbols per RMT memory block.
        // Use two blocks (96 symbols) to test whether the observed
        // LED-tail anomaly is caused by RMT encoder refill/underrun timing.
        config.mem_block_symbols = 96;
#else
        config.mem_block_symbols = 192;
#endif

        config.resolution_hz = T_SPEED::RmtTicksPerSecond;
        config.trans_queue_depth = 4;
        config.flags.invert_out = T_INVERTED::Inverted;
        config.flags.with_dma = false;

        esp_err_t ret = rmt_new_tx_channel(&config, &_channel);
        if (ret != ESP_OK || _channel == nullptr)
        {{
            _channel = nullptr;
            return;
        }}

        led_strip_encoder_config_t encoder_config = {{}};
        encoder_config.resolution = T_SPEED::RmtTicksPerSecond;
        _tx_config.loop_count = 0;

        ret = rmt_new_led_strip_encoder(
            &encoder_config,
            &_led_encoder,
            T_SPEED::RmtBit0,
            T_SPEED::RmtBit1
        );

        if (ret != ESP_OK || _led_encoder == nullptr)
        {{
            ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_del_channel(_channel));
            _channel = nullptr;
            _led_encoder = nullptr;
            return;
        }}

        ret = rmt_enable(_channel);
        if (ret != ESP_OK)
        {{
            ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_del_encoder(_led_encoder));
            _led_encoder = nullptr;
            ESP_ERROR_CHECK_WITHOUT_ABORT(rmt_del_channel(_channel));
            _channel = nullptr;
            return;
        }}
    }}'''

    update = f'''    void Update(bool maintainBufferConsistency)
    {{
        // {MARKER}
        if (_channel == nullptr || _led_encoder == nullptr)
        {{
            return;
        }}

        // Serialize writes: wait for the previous asynchronous RMT transfer
        // to finish before starting the next frame.
        if (ESP_OK == ESP_ERROR_CHECK_WITHOUT_ABORT(
                rmt_tx_wait_all_done(_channel, 10000 / portTICK_PERIOD_MS)))
        {{
            const esp_err_t ret = rmt_transmit(
                _channel,
                _led_encoder,
                _dataEditing,
                _sizeData,
                &_tx_config
            );

            if (ret != ESP_OK)
            {{
                return;
            }}

            if (maintainBufferConsistency)
            {{
                memcpy(_dataSending, _dataEditing, _sizeData);
            }}

            std::swap(_dataSending, _dataEditing);
        }}
    }}'''

    try:
        text = _replace_function(text, "~NeoEsp32RmtMethodBase()", destructor)
        text = _replace_function(text, "bool IsReadyToUpdate() const", ready)
        text = _replace_function(text, "void Initialize()", initialize)
        text = _replace_function(text, "void Update(bool maintainBufferConsistency)", update)
    except RuntimeError as exc:
        raise RuntimeError(f"CoreS3 RMT diagnostic patch failed for {target}: {exc}") from exc

    required = [
        MARKER,
        "config.mem_block_symbols = 96;",
        "return (_channel != nullptr && _led_encoder != nullptr);",
    ]
    for item in required:
        if item not in text:
            raise RuntimeError(
                f"CoreS3 RMT diagnostic patch verification failed: missing {item}"
            )

    target.write_text(text, encoding="utf-8", newline="\n")
    print(f"[CoreS3 RMT DIAG96] applied ESP32-S3 96-symbol diagnostic patch: {target}")


if not env.IsIntegrationDump():
    _apply_patch()
