#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE_KO="${ROOT_DIR}/kernel/idw_kmd_core.ko"
INSTALLER_EXE="${ROOT_DIR}/installer/inteldriverinstaller.exe"
CLIENT_BIN="${ROOT_DIR}/user/idw_client"
SHIM_BIN="${ROOT_DIR}/user/idw_win_shim"
STAGE_FILE="/tmp/idw_wddm_stage.bin"
SCAN_DIR="/tmp"
PE_PATH=""
INSTALLER_EXTRA_ARGS=()
CLIENT_EXTRA_ARGS=()
SHIM_EXTRA_ARGS=()
SINCE_MARK="$(date -Iseconds)"
LOADED_BY_SCRIPT=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--scan <dir>] [--stage <file>] [--pe <path_to_pe64>] [--installer-args "<args>"] [--client-args "<args>"] [--shim-args "<args>"] [-- <client_args...>] [--help|-h]
  --scan <dir>             Directory forwarded to inteldriverinstaller.exe --scan (default: /tmp)
  --stage <file>           Staging output path written by inteldriverinstaller.exe and read by
                           idw_client and idw_win_shim (default: /tmp/idw_wddm_stage.bin)
  --pe <path>              Optional PE64 binary passed to inteldriverinstaller.exe --inspect
                           and forwarded to user/idw_win_shim --pe
  --installer-args "<str>" Extra flags/arguments forwarded to inteldriverinstaller.exe
  --client-args "<str>"    Extra flags/arguments forwarded to user/idw_client
  --shim-args "<str>"      Extra flags/arguments forwarded to user/idw_win_shim
  -- <client_args...>      All remaining arguments after -- are forwarded verbatim to user/idw_client
  --help, -h               Show this help message and exit
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --scan)
            if [[ $# -lt 2 || -z "${2}" ]]; then
                echo "[Error] --scan requires a non-empty directory path" >&2
                usage >&2
                exit 2
            fi
            SCAN_DIR="$2"
            shift 2
            ;;
        --stage)
            if [[ $# -lt 2 || -z "${2}" ]]; then
                echo "[Error] --stage requires a non-empty file path" >&2
                usage >&2
                exit 2
            fi
            STAGE_FILE="$2"
            shift 2
            ;;
        --pe)
            if [[ $# -lt 2 || -z "${2}" ]]; then
                echo "[Error] --pe requires a non-empty file path" >&2
                usage >&2
                exit 2
            fi
            PE_PATH="$2"
            shift 2
            ;;
        --installer-args)
            if [[ $# -lt 2 ]]; then
                echo "[Error] --installer-args requires an argument string" >&2
                usage >&2
                exit 2
            fi
            if [[ -n "${2}" ]]; then
                read -r -a parsed_installer_args <<< "$2"
                INSTALLER_EXTRA_ARGS+=("${parsed_installer_args[@]}")
            fi
            shift 2
            ;;
        --client-args)
            if [[ $# -lt 2 ]]; then
                echo "[Error] --client-args requires an argument string" >&2
                usage >&2
                exit 2
            fi
            if [[ -n "${2}" ]]; then
                read -r -a parsed_client_args <<< "$2"
                CLIENT_EXTRA_ARGS+=("${parsed_client_args[@]}")
            fi
            shift 2
            ;;
        --shim-args)
            if [[ $# -lt 2 ]]; then
                echo "[Error] --shim-args requires an argument string" >&2
                usage >&2
                exit 2
            fi
            if [[ -n "${2}" ]]; then
                read -r -a parsed_shim_args <<< "$2"
                SHIM_EXTRA_ARGS+=("${parsed_shim_args[@]}")
            fi
            shift 2
            ;;
        --)
            shift
            CLIENT_EXTRA_ARGS+=("$@")
            break
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            CLIENT_EXTRA_ARGS+=("$@")
            break
            ;;
    esac
done

cleanup() {
    local exit_code=$?
    set +e

    if [[ -w /proc/dynamic_debug/control ]] && grep -q "^idw_kmd_core " /proc/modules 2>/dev/null; then
        echo 'module idw_kmd_core -p' | sudo tee /proc/dynamic_debug/control >/dev/null 2>&1
    fi

    if [[ "${LOADED_BY_SCRIPT}" -eq 1 ]] && grep -q "^idw_kmd_core " /proc/modules 2>/dev/null; then
        echo "==> Unloading idw_kmd_core"
        if ! sudo rmmod idw_kmd_core; then
            echo "[Warn] rmmod idw_kmd_core failed; verify no processes still hold /dev/inteldriverwin_onlinux open." >&2
        fi
    fi

    echo "==> Final dmesg trace (idw_kmd_core / idw_gtt_mapper)"
    if sudo dmesg -T --since "${SINCE_MARK}" >/dev/null 2>&1; then
        sudo dmesg -T --since "${SINCE_MARK}" | grep -E "idw_kmd_core|idw_gtt_mapper" || true
    else
        sudo dmesg -T | grep -E "idw_kmd_core|idw_gtt_mapper" | tail -n 40 || true
    fi

    exit "${exit_code}"
}

trap cleanup EXIT

if [[ ! -d "${SCAN_DIR}" ]]; then
    echo "[Error] Scan directory does not exist: ${SCAN_DIR}" >&2
    exit 1
fi

STAGE_DIR="$(dirname "${STAGE_FILE}")"
if [[ ! -d "${STAGE_DIR}" ]]; then
    echo "[Error] Staging parent directory does not exist: ${STAGE_DIR}" >&2
    exit 1
fi

if [[ -n "${PE_PATH}" && ! -s "${PE_PATH}" ]]; then
    echo "[Error] PE64 file missing or empty: ${PE_PATH}" >&2
    exit 1
fi

if [[ ! -f "${MODULE_KO}" ]]; then
    echo "[Error] Missing kernel module ${MODULE_KO}; build kernel module first." >&2
    exit 1
fi

if [[ ! -x "${CLIENT_BIN}" ]]; then
    echo "[Error] Missing executable ${CLIENT_BIN}; build user binaries first." >&2
    exit 1
fi

if [[ ! -x "${SHIM_BIN}" ]]; then
    echo "[Error] Missing executable ${SHIM_BIN}; build user binaries first." >&2
    exit 1
fi

if [[ ! -f "${INSTALLER_EXE}" ]]; then
    echo "[Error] Missing ${INSTALLER_EXE}; build installer first." >&2
    exit 1
fi

echo "==> 1. Loading idw_kmd_core.ko"
if ! grep -q "^idw_kmd_core " /proc/modules; then
    if [[ -e /sys/module/dynamic_debug/parameters/boot_arg ]] || [[ -f /proc/dynamic_debug/control ]]; then
        sudo insmod "${MODULE_KO}" dyndbg="+p"
    else
        sudo insmod "${MODULE_KO}"
    fi
    LOADED_BY_SCRIPT=1
else
    echo "    idw_kmd_core is already loaded; leaving existing instance attached."
fi

if [[ -w /proc/dynamic_debug/control ]]; then
    echo 'module idw_kmd_core +p' | sudo tee /proc/dynamic_debug/control >/dev/null
fi

if command -v wine >/dev/null 2>&1; then
    INSTALLER_ARGS=(--scan "Z:${SCAN_DIR}" --stage "Z:${STAGE_FILE}")
    if [[ -n "${PE_PATH}" ]]; then
        INSTALLER_ARGS+=(--inspect "Z:${PE_PATH}")
    fi
    if [[ "${#INSTALLER_EXTRA_ARGS[@]}" -gt 0 ]]; then
        INSTALLER_ARGS+=("${INSTALLER_EXTRA_ARGS[@]}")
    fi
    echo "==> 2. Running inteldriverinstaller.exe ${INSTALLER_ARGS[*]}"
    wine "${INSTALLER_EXE}" "${INSTALLER_ARGS[@]}"
else
    INSTALLER_ARGS=(--scan "${SCAN_DIR}" --stage "${STAGE_FILE}")
    if [[ -n "${PE_PATH}" ]]; then
        INSTALLER_ARGS+=(--inspect "${PE_PATH}")
    fi
    if [[ "${#INSTALLER_EXTRA_ARGS[@]}" -gt 0 ]]; then
        INSTALLER_ARGS+=("${INSTALLER_EXTRA_ARGS[@]}")
    fi
    echo "==> 2. Running inteldriverinstaller.exe ${INSTALLER_ARGS[*]}"
    "${INSTALLER_EXE}" "${INSTALLER_ARGS[@]}"
fi

CLIENT_ARGS=(--stage "${STAGE_FILE}")
if [[ "${#CLIENT_EXTRA_ARGS[@]}" -gt 0 ]]; then
    CLIENT_ARGS+=("${CLIENT_EXTRA_ARGS[@]}")
fi
echo "==> 3. Running idw_client ${CLIENT_ARGS[*]}"
"${CLIENT_BIN}" "${CLIENT_ARGS[@]}"

SHIM_ARGS=(--stage "${STAGE_FILE}")
if [[ -n "${PE_PATH}" ]]; then
    SHIM_ARGS+=(--pe "${PE_PATH}")
fi
if [[ "${#SHIM_EXTRA_ARGS[@]}" -gt 0 ]]; then
    SHIM_ARGS+=("${SHIM_EXTRA_ARGS[@]}")
fi
echo "==> 4. Running idw_win_shim ${SHIM_ARGS[*]}"
"${SHIM_BIN}" "${SHIM_ARGS[@]}"
