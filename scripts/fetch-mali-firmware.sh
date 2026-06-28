#!/bin/bash
# Clone linux-firmware for built-in Mali firmware (no initramfs)
# Run from kernel source root: ./scripts/fetch-mali-firmware.sh
# Clones linux-firmware into kernel tree root

set -e

SRCTREE="$(cd "$(dirname "$0")/.." && pwd)"
FIRMWARE_DIR="$SRCTREE/linux-firmware"

if [ -d "$FIRMWARE_DIR/.git" ]; then
	echo "linux-firmware already cloned at $FIRMWARE_DIR"
	(cd "$FIRMWARE_DIR" && git pull)
else
	git clone --depth 1 https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git "$FIRMWARE_DIR"
fi

echo "Ensure .config has:"
echo '  CONFIG_EXTRA_FIRMWARE="arm/mali/arch10.8/mali_csffw.bin"'
echo '  CONFIG_EXTRA_FIRMWARE_DIR="'$FIRMWARE_DIR'"'
