#!/usr/bin/env bash
set -euo pipefail

MODE="app"
ERASE=0
BAUD="921600"
CHIP="esp32s3"
PORT=""
BUILD_DIR="build/esp32.esp32.esp32s3"
FLASH_ARGS_FILE=""
SPIFFS_IMAGE=""
SPIFFS_OFFSET="0x610000"
SPIFFS_SIZE=""
SPIFFS_DIR="data"
SPIFFS_BLOCK_SIZE="4096"
SPIFFS_PAGE_SIZE="256"
PARTITIONS_FILE="partitions.csv"
AUTO_SPIFFS=0
OTA_PACKAGE=0
OTA_OUT="build/PylontechMonitoring.ino.bin"
OTA_UPLOAD_IP=""
OTA_SPIFFS_IP=""
BUILD_SPIFFS=0
ESPOTA_PORT="3232"
ESPOTA_HOST_IP=""
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: ./espserial.sh --port <serial-port> [options]

Modes:
  (default)             Flash only app binary (fast update)
  --full                Flash bootloader + partitions + boot_app0 + app
  --full-with-spiffs    Full flash + auto-build and flash SPIFFS from data/
  --ota-package         Prepare OTA .bin package from current app binary only
  --ota-upload <ip>     Prepare OTA package and upload via otaup to device IP
  --ota-spiffs <ip>     Build SPIFFS from data/ and upload it over the network
                        via espota (ArduinoOTA, filesystem). Combine with
                        --ota-upload <ip> to push app + filesystem in one run.
  --build-spiffs        Only (re)build the SPIFFS image from data/ (no flashing)

Options:
  --port <port>         Serial port (required), e.g. /dev/tty.usbmodem123
  --baud <value>        Baud rate (default: 921600)
  --chip <name>         Chip type (default: esp32s3)
  --build-dir <path>    Build folder (default: build/esp32.esp32.esp32s3)
  --flash-args <path>   Override flash_args file path
  --erase               Erase full flash before writing
  --spiffs-image <bin>  Optional SPIFFS image to flash in --full mode
  --spiffs-dir <path>   Source folder for SPIFFS build (default: data)
  --spiffs-offset <hex> SPIFFS offset (default: 0x610000)
  --partitions <path>   Partition CSV used to read SPIFFS size/offset
  --ota-out <path>      Output path for OTA package (default: build/PylontechMonitoring.ino.bin)
  --espota-port <port>  ArduinoOTA port for --ota-spiffs (default: 3232)
  --host-ip <ip>        Local source IP advertised to the device for the espota
                        back-connection. Auto-detected from the route to the
                        device when omitted (needed on hosts with several NICs).
  --dry-run             Print commands only
  -h, --help            Show this help

Examples:
  ./espserial.sh --port /dev/tty.usbmodem2101
  ./espserial.sh --port /dev/tty.usbmodem2101 --full
  ./espserial.sh --port /dev/tty.usbmodem2101 --full-with-spiffs
  ./espserial.sh --port /dev/tty.usbmodem2101 --ota-package
  ./espserial.sh --ota-upload 192.168.8.64
  ./espserial.sh --ota-spiffs 192.168.8.64
  ./espserial.sh --ota-upload 192.168.8.64 --ota-spiffs 192.168.8.64
  ./espserial.sh --build-spiffs
  ./espserial.sh --port /dev/tty.usbmodem2101 --full --erase
  ./espserial.sh --port /dev/tty.usbmodem2101 --full --spiffs-image build/spiffs.bin
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --full)
      MODE="full"
      shift
      ;;
    --full-with-spiffs)
      MODE="full"
      AUTO_SPIFFS=1
      shift
      ;;
    --ota-package)
      OTA_PACKAGE=1
      shift
      ;;
    --ota-upload)
      OTA_UPLOAD_IP="${2:-}"
      OTA_PACKAGE=1
      shift 2
      ;;
    --ota-spiffs)
      OTA_SPIFFS_IP="${2:-}"
      shift 2
      ;;
    --build-spiffs)
      BUILD_SPIFFS=1
      shift
      ;;
    --espota-port)
      ESPOTA_PORT="${2:-}"
      shift 2
      ;;
    --host-ip)
      ESPOTA_HOST_IP="${2:-}"
      shift 2
      ;;
    --erase)
      ERASE=1
      shift
      ;;
    --port)
      PORT="${2:-}"
      shift 2
      ;;
    --baud)
      BAUD="${2:-}"
      shift 2
      ;;
    --chip)
      CHIP="${2:-}"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="${2:-}"
      shift 2
      ;;
    --flash-args)
      FLASH_ARGS_FILE="${2:-}"
      shift 2
      ;;
    --spiffs-image)
      SPIFFS_IMAGE="${2:-}"
      shift 2
      ;;
    --spiffs-dir)
      SPIFFS_DIR="${2:-}"
      shift 2
      ;;
    --spiffs-offset)
      SPIFFS_OFFSET="${2:-}"
      shift 2
      ;;
    --partitions)
      PARTITIONS_FILE="${2:-}"
      shift 2
      ;;
    --ota-out)
      OTA_OUT="${2:-}"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

# Determine whether the application binary is involved at all.
# SPIFFS-only actions (--build-spiffs / --ota-spiffs without app flashing)
# skip the serial flash and the flash_args/app requirements.
SPIFFS_ONLY=0
if [[ "$OTA_PACKAGE" -eq 0 && "$MODE" != "full" ]]; then
  if [[ -n "$OTA_SPIFFS_IP" || "$BUILD_SPIFFS" -eq 1 ]]; then
    SPIFFS_ONLY=1
  fi
fi
NEED_APP=1
[[ "$SPIFFS_ONLY" -eq 1 ]] && NEED_APP=0

if [[ -z "$PORT" ]]; then
  if [[ "$OTA_PACKAGE" -eq 0 && "$SPIFFS_ONLY" -eq 0 ]]; then
    echo "Error: --port is required" >&2
    usage
    exit 1
  fi
fi

if [[ -z "$FLASH_ARGS_FILE" ]]; then
  FLASH_ARGS_FILE="$BUILD_DIR/flash_args"
fi

if [[ "$PARTITIONS_FILE" != /* ]]; then
  if [[ -f "$BUILD_DIR/$PARTITIONS_FILE" ]]; then
    PARTITIONS_FILE="$BUILD_DIR/$PARTITIONS_FILE"
  else
    PARTITIONS_FILE="$PARTITIONS_FILE"
  fi
fi

if [[ "$NEED_APP" -eq 1 && ! -f "$FLASH_ARGS_FILE" ]]; then
  echo "Error: flash_args file not found: $FLASH_ARGS_FILE" >&2
  echo "Hint: compile first and check --build-dir" >&2
  exit 1
fi

resolve_esptool() {
  # Prefer module invocation to avoid broken shebang wrappers in paths with spaces.
  if [[ -x ".venv/bin/python" ]] && .venv/bin/python -m esptool version >/dev/null 2>&1; then
    echo ".venv/bin/python -m esptool"
    return
  fi

  if command -v python3 >/dev/null 2>&1 && python3 -m esptool version >/dev/null 2>&1; then
    echo "python3 -m esptool"
    return
  fi

  if command -v esptool.py >/dev/null 2>&1; then
    echo "esptool.py"
    return
  fi

  if command -v esptool >/dev/null 2>&1; then
    echo "esptool"
    return
  fi

  # Fallback: Arduino-ESP32 bundled esptool binary
  local arduino_esptool
  arduino_esptool="$(ls -1 "$HOME"/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | tail -n 1 || true)"
  if [[ -n "$arduino_esptool" && -x "$arduino_esptool" ]]; then
    echo "$arduino_esptool"
    return
  fi

  echo "Error: esptool not found (esptool.py / esptool / python3 -m esptool)." >&2
  exit 1
}

resolve_mkspiffs() {
  if command -v mkspiffs >/dev/null 2>&1; then
    echo "mkspiffs"
    return
  fi

  local arduino_mkspiffs
  arduino_mkspiffs="$(ls -1 "$HOME"/Library/Arduino15/packages/esp32/tools/mkspiffs/*/mkspiffs 2>/dev/null | tail -n 1 || true)"
  if [[ -n "$arduino_mkspiffs" && -x "$arduino_mkspiffs" ]]; then
    echo "$arduino_mkspiffs"
    return
  fi

  echo "Error: mkspiffs not found." >&2
  exit 1
}

resolve_otaup_mode() {
  if command -v otaup >/dev/null 2>&1; then
    echo "direct"
    return
  fi

  # Fallback for setups where otaup is a zsh function in ~/.zshrc
  if command -v zsh >/dev/null 2>&1 && zsh -ic 'type otaup >/dev/null 2>&1'; then
    echo "zsh-func"
    return
  fi

  echo "Error: otaup not found in PATH and not available as zsh function." >&2
  exit 1
}

parse_spiffs_partition() {
  [[ -f "$PARTITIONS_FILE" ]] || return

  local parsed
  parsed="$(awk -F',' '
    /^[[:space:]]*#/ {next}
    NF < 5 {next}
    {
      name=$1; subType=$3; offset=$4; size=$5;
      gsub(/[[:space:]]/, "", name);
      gsub(/[[:space:]]/, "", subType);
      gsub(/[[:space:]]/, "", offset);
      gsub(/[[:space:]]/, "", size);
      if (subType=="spiffs") {
        print offset " " size;
        exit;
      }
    }
  ' "$PARTITIONS_FILE")"

  if [[ -n "$parsed" ]]; then
    SPIFFS_OFFSET="${parsed%% *}"
    SPIFFS_SIZE="${parsed##* }"
  fi
}

if [[ "$NEED_APP" -eq 1 ]]; then

if [[ "$DRY_RUN" -eq 1 ]]; then
  ESPTOOL=("esptool.py")
else
  ESPTOOL_CMD="$(resolve_esptool)"
  IFS=' ' read -r -a ESPTOOL <<< "$ESPTOOL_CMD"
fi

declare -a FLASH_OPTS=()
declare -a FLASH_ITEMS=()
APP_ADDR=""
APP_FILE=""

line_no=0
while IFS= read -r line || [[ -n "$line" ]]; do
  line_no=$((line_no + 1))

  [[ -z "$line" ]] && continue

  if [[ $line_no -eq 1 ]]; then
    IFS=' ' read -r -a FLASH_OPTS <<< "$line"
    continue
  fi

  addr="${line%% *}"
  file_rel="${line#* }"
  file_path="$file_rel"

  if [[ ! "$file_path" = /* ]]; then
    file_path="$BUILD_DIR/$file_path"
  fi

  if [[ ! -f "$file_path" ]]; then
    echo "Error: binary from flash_args not found: $file_path" >&2
    exit 1
  fi

  FLASH_ITEMS+=("$addr" "$file_path")

  if [[ "$file_path" == *.ino.bin ]]; then
    APP_ADDR="$addr"
    APP_FILE="$file_path"
  fi
done < "$FLASH_ARGS_FILE"

if [[ ${#FLASH_OPTS[@]} -eq 0 || ${#FLASH_ITEMS[@]} -eq 0 ]]; then
  echo "Error: invalid flash_args format in $FLASH_ARGS_FILE" >&2
  exit 1
fi

if [[ -z "$APP_ADDR" || -z "$APP_FILE" ]]; then
  APP_ADDR="0x10000"
  APP_FILE="$BUILD_DIR/PylontechMonitoring.ino.bin"
  if [[ ! -f "$APP_FILE" ]]; then
    echo "Error: app binary not found and no app entry in flash_args: $APP_FILE" >&2
    exit 1
  fi
fi

fi  # NEED_APP

run_cmd() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf 'DRY-RUN: '
    printf '%q ' "$@"
    printf '\n'
  else
    "$@"
  fi
}

build_spiffs_image() {
  parse_spiffs_partition

  if [[ -z "$SPIFFS_SIZE" ]]; then
    echo "Error: Could not determine SPIFFS size from partitions file: $PARTITIONS_FILE" >&2
    echo "Hint: use --partitions <path> or provide --spiffs-image manually." >&2
    exit 1
  fi

  if [[ -z "$SPIFFS_IMAGE" ]]; then
    SPIFFS_IMAGE="build/spiffs.bin"
  fi

  if [[ ! -d "$SPIFFS_DIR" ]]; then
    echo "Error: SPIFFS source directory not found: $SPIFFS_DIR" >&2
    exit 1
  fi

  local image_dir
  image_dir="$(dirname "$SPIFFS_IMAGE")"
  if [[ -n "$image_dir" && ! -d "$image_dir" ]]; then
    mkdir -p "$image_dir"
  fi

  local mkspiffs_cmd=("mkspiffs")
  if [[ "$DRY_RUN" -eq 0 ]]; then
    local MKSPIFFS_CMD
    MKSPIFFS_CMD="$(resolve_mkspiffs)"
    IFS=' ' read -r -a mkspiffs_cmd <<< "$MKSPIFFS_CMD"
  fi

  echo "Building SPIFFS image: $SPIFFS_IMAGE (size $SPIFFS_SIZE from $PARTITIONS_FILE)"
  run_cmd "${mkspiffs_cmd[@]}" -c "$SPIFFS_DIR" -b "$SPIFFS_BLOCK_SIZE" -p "$SPIFFS_PAGE_SIZE" -s "$SPIFFS_SIZE" "$SPIFFS_IMAGE"
}

resolve_espota() {
  local py=""
  if [[ -x ".venv/bin/python" ]]; then
    py=".venv/bin/python"
  elif command -v python3 >/dev/null 2>&1; then
    py="python3"
  else
    echo "Error: python3 not found (needed to run espota.py)." >&2
    exit 1
  fi

  # Prefer the official Arduino-ESP32 espota.py (stable location).
  local espota
  espota="$(ls -1 "$HOME"/Library/Arduino15/packages/esp32/hardware/esp32/*/tools/espota.py 2>/dev/null | sort -V | tail -n 1 || true)"
  if [[ -z "$espota" || ! -f "$espota" ]]; then
    espota="$(command -v espota.py || true)"
  fi

  if [[ -z "$espota" || ! -f "$espota" ]]; then
    echo "Error: espota.py not found (Arduino-ESP32 tools)." >&2
    exit 1
  fi

  # Populate a global array so paths with spaces stay intact.
  ESPOTA_CMD_ARR=("$py" "$espota")
}

upload_spiffs_ota() {
  local ip="$1"

  if [[ -z "$ip" ]]; then
    echo "Error: --ota-spiffs requires a device IP" >&2
    exit 1
  fi

  if [[ "$DRY_RUN" -eq 0 && ! -f "$SPIFFS_IMAGE" ]]; then
    echo "Error: SPIFFS image not found: $SPIFFS_IMAGE" >&2
    exit 1
  fi

  local espota_cmd=("python3" "espota.py")
  if [[ "$DRY_RUN" -eq 0 ]]; then
    ESPOTA_CMD_ARR=()
    resolve_espota
    espota_cmd=("${ESPOTA_CMD_ARR[@]}")
  fi

  # Determine the local source IP advertised to the device. On a multi-NIC host
  # espota.py may auto-pick the wrong interface, so the device cannot open the
  # back-connection ("No response from device"). Derive it from the route.
  local host_ip="$ESPOTA_HOST_IP"
  if [[ -z "$host_ip" ]]; then
    local iface
    iface="$(route -n get "$ip" 2>/dev/null | awk '/interface:/{print $2}')"
    if [[ -n "$iface" ]]; then
      host_ip="$(ipconfig getifaddr "$iface" 2>/dev/null || true)"
    fi
  fi

  local host_args=()
  if [[ -n "$host_ip" ]]; then
    host_args=(-I "$host_ip")
    echo "Advertising host IP $host_ip to device for back-connection"
  fi

  echo "Uploading SPIFFS image to $ip:$ESPOTA_PORT via espota (ArduinoOTA filesystem)"
  run_cmd "${espota_cmd[@]}" "${host_args[@]}" -i "$ip" -p "$ESPOTA_PORT" -s -f "$SPIFFS_IMAGE"
}

if [[ "$OTA_PACKAGE" -eq 1 ]]; then
  ota_dir="$(dirname "$OTA_OUT")"
  if [[ -n "$ota_dir" && ! -d "$ota_dir" ]]; then
    mkdir -p "$ota_dir"
  fi

  echo "Preparing OTA package: $OTA_OUT"
  run_cmd cp "$APP_FILE" "$OTA_OUT"

  if command -v shasum >/dev/null 2>&1; then
    run_cmd shasum -a 256 "$OTA_OUT"
    if [[ "$DRY_RUN" -eq 0 ]]; then
      shasum -a 256 "$OTA_OUT" > "$OTA_OUT.sha256"
      echo "Checksum written: $OTA_OUT.sha256"
    fi
  elif command -v sha256sum >/dev/null 2>&1; then
    run_cmd sha256sum "$OTA_OUT"
    if [[ "$DRY_RUN" -eq 0 ]]; then
      sha256sum "$OTA_OUT" > "$OTA_OUT.sha256"
      echo "Checksum written: $OTA_OUT.sha256"
    fi
  else
    echo "Warning: no sha256 tool found (shasum/sha256sum)."
  fi

  echo "OTA package ready: $OTA_OUT"

  if [[ -n "$OTA_UPLOAD_IP" ]]; then
    if [[ "$DRY_RUN" -eq 1 ]]; then
      OTAUP_MODE="direct"
    else
      OTAUP_MODE="$(resolve_otaup_mode)"
    fi

    echo "Uploading OTA package to $OTA_UPLOAD_IP via otaup"
    if [[ "$OTAUP_MODE" == "direct" ]]; then
      run_cmd otaup "$OTA_UPLOAD_IP"
    else
      # Quote IP safely for shell command string passed to zsh -ic
      printf -v OTAUP_IP_QUOTED '%q' "$OTA_UPLOAD_IP"
      run_cmd zsh -ic "otaup ${OTAUP_IP_QUOTED}"
    fi
  else
    echo "Upload this file via /service or your otaup workflow."
  fi

  # Optionally also push the SPIFFS filesystem image over the network.
  if [[ -n "$OTA_SPIFFS_IP" ]]; then
    build_spiffs_image
    upload_spiffs_ota "$OTA_SPIFFS_IP"
  fi

  exit 0
fi

# SPIFFS-only actions (no app flashing): build the image and optionally
# upload it via espota (ArduinoOTA filesystem), then exit.
if [[ "$SPIFFS_ONLY" -eq 1 ]]; then
  build_spiffs_image
  if [[ -n "$OTA_SPIFFS_IP" ]]; then
    upload_spiffs_ota "$OTA_SPIFFS_IP"
  else
    echo "SPIFFS image ready: $SPIFFS_IMAGE"
  fi
  exit 0
fi

if [[ "$AUTO_SPIFFS" -eq 1 || "$BUILD_SPIFFS" -eq 1 ]]; then
  build_spiffs_image
fi

BASE_ARGS=("--chip" "$CHIP" "--port" "$PORT" "--baud" "$BAUD")

if [[ "$ERASE" -eq 1 ]]; then
  echo "Erase flash..."
  run_cmd "${ESPTOOL[@]}" "${BASE_ARGS[@]}" erase-flash
fi

if [[ "$MODE" == "full" ]]; then
  echo "Full flash mode: writing bootloader, partitions, boot_app0, app"
  FULL_ITEMS=("${FLASH_ITEMS[@]}")

  if [[ -n "$SPIFFS_IMAGE" ]]; then
    if [[ ! -f "$SPIFFS_IMAGE" ]]; then
      echo "Error: SPIFFS image not found: $SPIFFS_IMAGE" >&2
      exit 1
    fi
    FULL_ITEMS+=("$SPIFFS_OFFSET" "$SPIFFS_IMAGE")
    echo "Including SPIFFS image at $SPIFFS_OFFSET: $SPIFFS_IMAGE"
  fi

  run_cmd "${ESPTOOL[@]}" "${BASE_ARGS[@]}" write-flash -z "${FLASH_OPTS[@]}" "${FULL_ITEMS[@]}"
else
  echo "App flash mode: writing firmware only"
  run_cmd "${ESPTOOL[@]}" "${BASE_ARGS[@]}" write-flash -z "${FLASH_OPTS[@]}" "$APP_ADDR" "$APP_FILE"
fi

echo "Done."