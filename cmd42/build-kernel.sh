#!/usr/bin/env bash
set -Eeuo pipefail

CMD42_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMD42_WORK_DIR="${RUNNER_TEMP:-/tmp}/cmd42-kernel-build"
CMD42_PACKAGE_VERSION='1:6.18.39-1+rpt1'
CMD42_KERNEL_RELEASE='6.18.39+rpt-rpi-v8'
CMD42_IMAGE_PACKAGE='linux-image-6.18.39+rpt-rpi-v8'

rm -rf "${CMD42_WORK_DIR}"
mkdir -p "${CMD42_WORK_DIR}/packages" "${CMD42_WORK_DIR}/artifact"
cd "${CMD42_WORK_DIR}/packages"

apt download "linux-source-6.18=${CMD42_PACKAGE_VERSION}"
apt download "${CMD42_IMAGE_PACKAGE}:arm64=${CMD42_PACKAGE_VERSION}"

CMD42_SOURCE_DEB="$(find "${PWD}" -maxdepth 1 -type f -name 'linux-source-6.18_*.deb' -print -quit)"
CMD42_IMAGE_DEB="$(find "${PWD}" -maxdepth 1 -type f -name 'linux-image-6.18.39+rpt-rpi-v8_*.deb' -print -quit)"
test -n "${CMD42_SOURCE_DEB}" && test -n "${CMD42_IMAGE_DEB}"

mkdir -p "${CMD42_WORK_DIR}/source-package" "${CMD42_WORK_DIR}/image-package"
dpkg-deb -x "${CMD42_SOURCE_DEB}" "${CMD42_WORK_DIR}/source-package"
dpkg-deb -x "${CMD42_IMAGE_DEB}" "${CMD42_WORK_DIR}/image-package"

cd "${CMD42_WORK_DIR}"
tar -xf source-package/usr/src/linux-source-6.18.tar.xz
cd linux-source-6.18

cp "${CMD42_WORK_DIR}/image-package/boot/config-${CMD42_KERNEL_RELEASE}" .config
patch -p1 < "${CMD42_REPO_ROOT}/cmd42/patches/expose-locked-sd.patch"

scripts/config --set-str LOCALVERSION '+rpt-rpi-v8'
scripts/config --disable LOCALVERSION_AUTO
scripts/config --set-str SYSTEM_TRUSTED_KEYS ''
scripts/config --set-str SYSTEM_REVOCATION_KEYS ''

make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
rm -f include/config/kernel.release
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- prepare

CMD42_ACTUAL_RELEASE="$(make -s ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- kernelrelease)"
if [[ "${CMD42_ACTUAL_RELEASE}" != "${CMD42_KERNEL_RELEASE}" ]]; then
  echo "Kernel release inesperado: ${CMD42_ACTUAL_RELEASE}" >&2
  exit 1
fi

make -j"$(nproc)" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image.gz

install -m 0644 arch/arm64/boot/Image.gz   "${CMD42_WORK_DIR}/artifact/kernel8-cmd42-${CMD42_KERNEL_RELEASE}.img.gz"
install -m 0644 .config   "${CMD42_WORK_DIR}/artifact/config-${CMD42_KERNEL_RELEASE}-cmd42"
install -m 0644 "${CMD42_REPO_ROOT}/cmd42/patches/expose-locked-sd.patch"   "${CMD42_WORK_DIR}/artifact/expose-locked-sd.patch"
aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -std=gnu11 \
  -fstack-protector-strong -D_FORTIFY_SOURCE=3 \
  -Wl,-z,relro,-z,now \
  -o "${CMD42_WORK_DIR}/artifact/sd-cmd42" \
  "${CMD42_REPO_ROOT}/cmd42/sd-cmd42.c"
aarch64-linux-gnu-strip "${CMD42_WORK_DIR}/artifact/sd-cmd42"
install -m 0755 "${CMD42_REPO_ROOT}/cmd42/cmd42-rescue" \
  "${CMD42_WORK_DIR}/artifact/cmd42-rescue"
install -m 0755 "${CMD42_REPO_ROOT}/cmd42/install-rescue.sh" \
  "${CMD42_WORK_DIR}/artifact/install-rescue.sh"
install -m 0644 "${CMD42_REPO_ROOT}/cmd42/README.md" \
  "${CMD42_WORK_DIR}/artifact/README.md"
install -m 0644 "${CMD42_IMAGE_DEB}" \
  "${CMD42_WORK_DIR}/artifact/linux-image-${CMD42_KERNEL_RELEASE}.deb"

cd "${CMD42_WORK_DIR}/artifact"
sha256sum kernel8-cmd42-* config-* expose-locked-sd.patch sd-cmd42 \
  cmd42-rescue install-rescue.sh README.md linux-image-*.deb > SHA256SUMS
printf '%s\n' "${CMD42_KERNEL_RELEASE}" > KERNEL_RELEASE

echo "Artefatos gerados em ${CMD42_WORK_DIR}/artifact"
ls -lh "${CMD42_WORK_DIR}/artifact"
