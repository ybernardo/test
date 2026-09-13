#!/bin/bash -e

CMD42_RELEASE='6.18.39+rpt-rpi-v8'
CMD42_BOOT_DIR="${ROOTFS_DIR}/boot/firmware"
CMD42_CONFIG="${CMD42_BOOT_DIR}/config.txt"

install -m 0644 files/linux-image-cmd42.deb \
  "${ROOTFS_DIR}/tmp/linux-image-cmd42.deb"

on_chroot <<'CHROOT'
apt-get install -y /tmp/linux-image-cmd42.deb
rm -f /tmp/linux-image-cmd42.deb
CHROOT

test -d "${ROOTFS_DIR}/lib/modules/${CMD42_RELEASE}"
test -f "${ROOTFS_DIR}/boot/initrd.img-${CMD42_RELEASE}"
test -d "${CMD42_BOOT_DIR}"
test -f "${CMD42_CONFIG}"

install -m 0755 files/kernel8-cmd42.img \
  "${CMD42_BOOT_DIR}/kernel8-cmd42.img"
install -m 0755 "${ROOTFS_DIR}/boot/initrd.img-${CMD42_RELEASE}" \
  "${CMD42_BOOT_DIR}/initramfs8-cmd42"
install -m 0755 files/sd-cmd42 \
  "${ROOTFS_DIR}/usr/local/sbin/sd-cmd42"
install -m 0755 files/cmd42-rescue \
  "${ROOTFS_DIR}/usr/local/sbin/cmd42-rescue"
install -m 0644 files/motd "${ROOTFS_DIR}/etc/motd"

cat >>"${CMD42_CONFIG}" <<'CONFIG'

[pi4]
kernel=kernel8-cmd42.img
initramfs initramfs8-cmd42 followkernel
CONFIG

on_chroot <<'CHROOT'
systemctl enable ssh
CHROOT

