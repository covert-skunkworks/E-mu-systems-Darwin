#!/usr/bin/env bash
# Pack a built Emu_Darwin for release: the program, README, the licence notices
# of what it carries (Musashi, SoftFloat, Nuklear, the Droid Sans font), and the empty Firmware/
# and Disks/ folders the user fills. Never firmware, disk images or settings.
#
# usage, from the project root after `make -C darwin`:
#   scripts/package.sh OS VERSION [PROGRAM]     OS: windows, linux or macos
#
# PROGRAM is the built program (default: darwin/Emu_Darwin.exe on Windows, darwin/Emu_Darwin
# elsewhere). Writes into dist/:
#
#   Emu_Darwin-VERSION-windows-x64.zip       Emu_Darwin.exe, statically linked
#   Emu_Darwin-VERSION-linux-x64.tar.gz      tar keeps the program executable
#   Emu_Darwin-VERSION-macos-universal.zip   Emu_Darwin.app (arm64 + x86_64), made with ditto,
#                                            which keeps the bundle's signature and links
#
# Each holds one folder of the same name. The GitHub workflow (.github/workflows/build.yml) and
# scripts/release.sh run it. The Windows zip is made with zip, or with PowerShell where zip is not
# installed.
#
# macOS: the bundle gets an Info.plist (with the microphone note macOS asks for before the
# Darwin's inputs can record), the icon from darwin/res/darwin.ico, and an ad-hoc signature.
# The program inside finds Firmware/ and Disks/ beside the bundle (plat.c). The release build
# links SDL2 statically; a build against Homebrew's SDL2 is packed too, with a warning, but runs
# only where Homebrew's SDL2 is installed.
#
# The font's licence (Apache 2.0) has to travel with it; its text is fetched from apache.org
# unless APACHE_LICENSE names a local copy.

set -euo pipefail

os=${1:?usage: package.sh OS VERSION [PROGRAM]}
ver=${2:?usage: package.sh OS VERSION [PROGRAM]}
case "$os" in
	windows) arch=x64; prog=${3:-darwin/Emu_Darwin.exe} ;;
	linux) arch=x64; prog=${3:-darwin/Emu_Darwin} ;;
	macos) arch=universal; prog=${3:-darwin/Emu_Darwin} ;;
	*) echo "package.sh: unknown OS $os" >&2; exit 1 ;;
esac
name="Emu_Darwin-$ver-$os-$arch"
out="dist/$name"

rm -rf "$out" "dist/$name.zip" "dist/$name.tar.gz"
mkdir -p "$out/licenses" "$out/Firmware" "$out/Disks"

# the two folders the user fills, each saying what goes in it
cat > "$out/Firmware/PUT FIRMWARE HERE.txt" <<'EOF'
Put the Darwin's firmware here: the ROM images of your own unit, as zip files named
emu_darwin_v2_50.zip (version 2.5) and / or emu_darwin_v2_02.zip (version 2.02).
None is included with the emulator. With both here, the settings window (click the E-MU logo,
Options page) chooses which one runs; otherwise the newest is used.
EOF
cat > "$out/Disks/DISK IMAGES GO HERE.txt" <<'EOF'
The Darwin's hard disks are image files (.img). The settings window (click the E-MU logo, Disks
page, "New...") creates them here unless you choose another folder, and clicking the drive bay
on the panel picks one. A new image is formatted on the Darwin itself: System, then Disk.
Images can live anywhere; the emulator remembers where.
EOF

case "$os" in
	windows) cp "$prog" "$out/Emu_Darwin.exe" ;;
	linux) cp "$prog" "$out/Emu_Darwin" ;;
	macos)
		app="$out/Emu_Darwin.app"
		mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
		cp "$prog" "$app/Contents/MacOS/Emu_Darwin"
		cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>                  <string>Emu_Darwin</string>
  <key>CFBundleDisplayName</key>           <string>Emu_Darwin</string>
  <key>CFBundleExecutable</key>            <string>Emu_Darwin</string>
  <key>CFBundleIdentifier</key>            <string>local.emu-darwin</string>
  <key>CFBundlePackageType</key>           <string>APPL</string>
  <key>CFBundleShortVersionString</key>    <string>$ver</string>
  <key>CFBundleVersion</key>               <string>$ver</string>
  <key>CFBundleIconFile</key>              <string>AppIcon</string>
  <key>LSMinimumSystemVersion</key>        <string>11.0</string>
  <key>NSHighResolutionCapable</key>       <false/>
  <key>NSMicrophoneUsageDescription</key>  <string>The emulated Darwin records its analog inputs from your audio inputs.</string>
</dict>
</plist>
EOF
		# the Finder icon from the Windows icon; the app works without it if this fails
		icons="$out.iconset"
		( sips -s format png darwin/res/darwin.ico --out "$out.png" > /dev/null &&
			mkdir -p "$icons" &&
			for s in 16 32 128 256; do
				sips -z $s $s "$out.png" --out "$icons/icon_${s}x${s}.png" > /dev/null
			done &&
			iconutil -c icns "$icons" -o "$app/Contents/Resources/AppIcon.icns" ) || echo "package.sh: icon skipped"
		rm -rf "$icons" "$out.png"
		codesign --force --deep --sign - "$app"
		if otool -L "$app/Contents/MacOS/Emu_Darwin" | grep -E '^[[:space:]]' | grep -vqE '^[[:space:]]*(/System/Library/|/usr/lib/)'; then
			echo "package.sh: warning: the program uses libraries outside macOS (Homebrew's SDL2?);" >&2
			echo "            it runs only where they are installed. The release build links SDL2 statically." >&2
		fi
		;;
esac

cp release/README.txt "$out/README.txt"
cp darwin/musashi/readme.txt "$out/licenses/Musashi.txt"
cp darwin/musashi/softfloat/README.txt "$out/licenses/SoftFloat.txt"
cp darwin/third_party/nuklear/LICENSE "$out/licenses/Nuklear.txt"
if [ -n "${APACHE_LICENSE:-}" ]; then
	cp "$APACHE_LICENSE" "$out/licenses/DroidSans-Apache-2.0.txt"
else
	curl -fsSL https://www.apache.org/licenses/LICENSE-2.0.txt -o "$out/licenses/DroidSans-Apache-2.0.txt"
fi

# never ship firmware, disk images or anyone's settings: the user brings their own firmware, and
# the emulator makes the rest. The folders above only take files named here, so this is a guard
# against a later change to this script.
bad=$(find "$out" -type f \( -iname '*.zip' -o -iname '*.img' -o -iname '*.hda' -o -iname '*.bin' \
	-o -iname '*.ini' -o -iname '*.log' -o -iname 'emu_darwin_v*' \) -print)
if [ -n "$bad" ]; then
	echo "package.sh: refusing to pack firmware, disk images or settings:" >&2
	echo "$bad" >&2
	exit 1
fi

cd dist
case "$os" in
	windows)
		echo "dist/$name.zip:"
		if command -v zip > /dev/null; then
			zip -qr "$name.zip" "$name"
		else
			powershell.exe -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$PWD/$name")' -DestinationPath '$(cygpath -w "$PWD/$name.zip")'"
		fi
		;;
	linux)
		echo "dist/$name.tar.gz:"
		tar czf "$name.tar.gz" "$name"
		;;
	macos)
		echo "dist/$name.zip:"
		ditto -c -k --keepParent "$name" "$name.zip"
		;;
esac
find "$name" | sort
rm -rf "$name"
