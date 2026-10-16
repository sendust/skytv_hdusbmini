#!/bin/sh
# Extract the Cypress FX2 firmware for the SKYTV HD USB mini (1c6c:04c9) from the
# vendor's Windows driver package. The firmware is not redistributed in this repository.
#
# usage: tools/extract_firmware.sh <SKYTV HD USBmini_v2-7-2p3.zip | skytvhdusbmini_driver*.exe | skyhdmc.sys> [out.fw]
#
# needs: unzip, 7z (package 7zip), unshield  ->  sudo apt-get install -y unzip 7zip unshield
set -eu

FW_OFFSET=$((0x1ee58))          # offset of the 8051 image inside x86/skyhdmc.sys (driver 1.3.9.97)
FW_SIZE=16384
SYS_SHA256=6dea5546aed524110fa1f5a62b446c7d62a41a6f0f5ad21141b317252a0ece50
FW_SHA256=50f84422e8699fed3f0388b9a529a9c4b90edad9595ae3b955adf481aec9c85a

in=${1:?usage: $0 <driver zip | driver exe | skyhdmc.sys> [out.fw]}
out=${2:-dvb-usb-skytv-hdusbmini.fw}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

case "$in" in
  *.zip)
    unzip -q "$in" -d "$work/zip"
    exe=$(find "$work/zip" -iname '*driver*.exe' | head -n 1)
    [ -n "$exe" ] || { echo "driver installer not found in $in" >&2; exit 1; }
    ;;
  *.exe|*.EXE) exe=$in ;;
  *.sys|*.SYS) sys=$in ;;
  *) echo "unknown input: $in" >&2; exit 1 ;;
esac

if [ -z "${sys:-}" ]; then
  7z x -y -o"$work/is" "$exe" >/dev/null
  cab=$(find "$work/is" -iname 'data1.cab' | head -n 1)
  [ -n "$cab" ] || { echo "InstallShield data1.cab not found" >&2; exit 1; }
  unshield -d "$work/cab" x "$cab" >/dev/null
  sys=$(find "$work/cab" -path '*x86*' -iname 'skyhdmc.sys' | head -n 1)
  [ -n "$sys" ] || { echo "x86/skyhdmc.sys not found" >&2; exit 1; }
fi

got=$(sha256sum "$sys" | cut -d' ' -f1)
[ "$got" = "$SYS_SHA256" ] || echo "warning: skyhdmc.sys sha256 $got differs from the tested driver 1.3.9.97" >&2

dd if="$sys" of="$out" bs=1 skip="$FW_OFFSET" count="$FW_SIZE" 2>/dev/null
got=$(sha256sum "$out" | cut -d' ' -f1)
if [ "$got" = "$FW_SHA256" ]; then
  echo "OK: $out ($FW_SIZE bytes, sha256 matches)"
  echo "install: sudo cp $out /lib/firmware/dvb-usb-skytv-hdusbmini.fw"
else
  echo "error: extracted firmware sha256 $got does not match $FW_SHA256" >&2
  exit 1
fi
