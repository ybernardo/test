#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat >&2 <<'USAGE'
Uso:
  sudo ./install-rescue.sh KERNEL_IMAGE SD_CMD42_BINARY

Instala o kernel CMD42 e os utilitarios em Raspberry Pi OS 64-bit.
O kernel deve corresponder aos modulos e ao initramfs instalados no sistema.
USAGE
}

if [[ ${EUID} -ne 0 || $# -ne 2 ]]; then
  usage
  exit 1
fi

KERNEL_SOURCE="$(realpath "$1")"
UTILITY_SOURCE="$(realpath "$2")"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOOT_DIR=/boot/firmware
CONFIG_FILE="${BOOT_DIR}/config.txt"
KERNEL_TARGET="${BOOT_DIR}/kernel8-cmd42.img"
INITRAMFS_TARGET="${BOOT_DIR}/initramfs8-cmd42"
EXPECTED_RELEASE='6.18.39+rpt-rpi-v8'
BACKUP_SUFFIX=".before-cmd42"

[[ -f "${KERNEL_SOURCE}" ]] || { echo "Kernel inexistente: ${KERNEL_SOURCE}" >&2; exit 1; }
[[ -x "${UTILITY_SOURCE}" ]] || { echo "Utilitario invalido: ${UTILITY_SOURCE}" >&2; exit 1; }
[[ -f "${CONFIG_FILE}" ]] || { echo "Nao foi encontrado ${CONFIG_FILE}." >&2; exit 1; }
[[ -d "/lib/modules/${EXPECTED_RELEASE}" ]] || {
  echo "Modulos ${EXPECTED_RELEASE} nao estao instalados." >&2
  exit 1
}
[[ -f "/boot/initrd.img-${EXPECTED_RELEASE}" ]] || {
  echo "Initramfs ${EXPECTED_RELEASE} nao foi encontrado." >&2
  exit 1
}

MODEL="$(tr -d '\0' </proc/device-tree/model 2>/dev/null || true)"
if [[ "${MODEL}" != *"Raspberry Pi 4"* ]]; then
  echo "Plataforma recusada: esperado Raspberry Pi 4; detectado '${MODEL:-desconhecido}'." >&2
  exit 1
fi

findmnt -rn "${BOOT_DIR}" >/dev/null || {
  echo "A particao de boot nao esta montada em ${BOOT_DIR}." >&2
  exit 1
}

if [[ ! -e "${CONFIG_FILE}${BACKUP_SUFFIX}" ]]; then
  cp -a "${CONFIG_FILE}" "${CONFIG_FILE}${BACKUP_SUFFIX}"
fi
if [[ -e "${BOOT_DIR}/kernel8.img" && ! -e "${BOOT_DIR}/kernel8.img${BACKUP_SUFFIX}" ]]; then
  cp -a "${BOOT_DIR}/kernel8.img" "${BOOT_DIR}/kernel8.img${BACKUP_SUFFIX}"
fi

install -m 0755 "${KERNEL_SOURCE}" "${KERNEL_TARGET}"
install -m 0755 "/boot/initrd.img-${EXPECTED_RELEASE}" "${INITRAMFS_TARGET}"
install -m 0755 "${UTILITY_SOURCE}" /usr/local/sbin/sd-cmd42
install -m 0755 "${SCRIPT_DIR}/cmd42-rescue" /usr/local/sbin/cmd42-rescue

if ! grep -qxF 'kernel=kernel8-cmd42.img' "${CONFIG_FILE}" ||
   ! grep -qxF 'initramfs initramfs8-cmd42 followkernel' "${CONFIG_FILE}"; then
  printf '\n[pi4]\nkernel=kernel8-cmd42.img\ninitramfs initramfs8-cmd42 followkernel\n' >>"${CONFIG_FILE}"
fi

sync

cat <<'DONE'
Instalacao concluida.

Reinicie e valide:
  uname -a
  vcgencmd get_config str | grep kernel

Menu simplificado:
  sudo cmd42-rescue
DONE
