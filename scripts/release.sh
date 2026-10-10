#!/usr/bin/env bash
# Make a release of the emulator.
#
#   scripts/release.sh VERSION [--wsl] [--push]
#
#   VERSION   e.g. 1.1 (the tag will be v1.1; the About tab shows 1.1)
#   --wsl     on Windows: also build the Linux package, in WSL (it needs gcc, make, pkg-config,
#             libsdl2-dev and libasound2-dev there)
#   --push    after the local packages: tag vVERSION and push the tag to GitHub (origin). GitHub
#             Actions (.github/workflows/build.yml) then builds Windows, Linux and macOS and
#             publishes the release with the three packages. Asks before pushing.
#
# Without --push nothing leaves this computer: the packages for this system (and Linux with
# --wsl) are made in dist/, to try before publishing. Each is a clean build of the current
# commit, in darwin/build-release-<os>/, so a running Emu_Darwin is not disturbed.
#
# Run it from Git Bash or MSYS2 on Windows (MSYS2 mingw64 gcc, as for a normal build), or from a
# shell on Linux or macOS. The licence text of the Droid Sans font (Apache 2.0) is fetched from
# apache.org once and kept in build/; set APACHE_LICENSE to use a local copy instead.

set -euo pipefail

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

ver=""
wsl=0
push=0
for a in "$@"; do
	case "$a" in
		--wsl) wsl=1 ;;
		--push) push=1 ;;
		-h | --help) usage ;;
		-*) echo "release.sh: unknown option $a" >&2; usage ;;
		*) ver=$a ;;
	esac
done
[ -n "$ver" ] || usage
if ! [[ "$ver" =~ ^[0-9]+(\.[0-9]+)*$ ]]; then
	echo "release.sh: VERSION is numbers and dots, like 1.1 (got: $ver)" >&2
	exit 1
fi
tag="v$ver"

cd "$(dirname "$0")/.."

# --- what this is built from --------------------------------------------------------------------
if [ ! -f darwin/third_party/nuklear/nuklear.h ]; then
	echo "release.sh: Nuklear is missing; run: git submodule update --init" >&2
	exit 1
fi
if ! git diff --quiet || ! git diff --cached --quiet; then
	echo "release.sh: there are uncommitted changes; a release is built from a commit" >&2
	git status --short >&2
	exit 1
fi
if git rev-parse -q --verify "refs/tags/$tag" > /dev/null; then
	echo "release.sh: the tag $tag exists already; choose another version" >&2
	exit 1
fi

case "$(uname -s)" in
	MINGW* | MSYS*) os=windows; exe=.exe ;;
	Linux) os=linux; exe= ;;
	Darwin) os=macos; exe= ;;
	*) echo "release.sh: unknown system $(uname -s)" >&2; exit 1 ;;
esac
# started from Git Bash rather than an MSYS2 shell: MSYS2's compiler, in its default place
if [ "$os" = windows ] && ! command -v gcc > /dev/null; then
	for env in ucrt64 mingw64; do
		if [ -d "/c/msys64/$env/bin" ]; then
			export PATH="/c/msys64/$env/bin:/c/msys64/usr/bin:$PATH"  # this shell only
			break
		fi
	done
fi

if [ -z "${APACHE_LICENSE:-}" ]; then
	APACHE_LICENSE=build/licenses/Apache-2.0.txt
	if [ ! -s "$APACHE_LICENSE" ]; then
		mkdir -p build/licenses
		echo "fetching the Apache 2.0 licence text (the font's) from apache.org, once"
		curl -fsSL https://www.apache.org/licenses/LICENSE-2.0.txt -o "$APACHE_LICENSE"
	fi
fi
export APACHE_LICENSE

# --- this system's package --------------------------------------------------------------------
build="build-release-$os"
echo "== $os: building $ver"
rm -rf "darwin/$build"
make -C darwin BUILD="$build" EXE="$build/Emu_Darwin$exe" VERSION="$ver" -j4
"darwin/$build/Emu_Darwin$exe" -newdisk "darwin/$build/smoke.img:1" > /dev/null  # it starts
bash scripts/package.sh "$os" "$ver" "darwin/$build/Emu_Darwin$exe"

# --- Linux, from Windows through WSL ------------------------------------------------------------
if [ "$wsl" = 1 ]; then
	if [ "$os" != windows ]; then
		echo "release.sh: --wsl is for Windows; ignored" >&2
	else
		# WSL sees a drive X: as /mnt/x
		win=$(cygpath -m "$PWD")
		here="/mnt/$(echo "${win:0:1}" | tr 'A-Z' 'a-z')${win:2}"
		case "$APACHE_LICENSE" in
			/* | ?:*) lic=$(cygpath -m "$APACHE_LICENSE"); lic="/mnt/$(echo "${lic:0:1}" | tr 'A-Z' 'a-z')${lic:2}" ;;
			*) lic="$here/$APACHE_LICENSE" ;;
		esac
		echo "== linux (WSL): building $ver"
		wsl.exe bash -c "set -e; cd '$here'; rm -rf darwin/build-release-linux; \
			make -C darwin BUILD=build-release-linux EXE=build-release-linux/Emu_Darwin VERSION='$ver' -j4; \
			darwin/build-release-linux/Emu_Darwin -newdisk darwin/build-release-linux/smoke.img:1 > /dev/null; \
			APACHE_LICENSE='$lic' bash scripts/package.sh linux '$ver' darwin/build-release-linux/Emu_Darwin"
	fi
fi

echo
echo "Packages made here:"
ls -l dist/Emu_Darwin-"$ver"-*

# --- publish ----------------------------------------------------------------------------------
if [ "$push" = 1 ]; then
	remote=$(git remote get-url origin 2> /dev/null || true)
	if [ -z "$remote" ]; then
		echo "release.sh: no remote 'origin' to push to (git remote add origin URL)" >&2
		exit 1
	fi
	echo
	echo "Tag $tag on $(git rev-parse --short HEAD) and push it to $remote?"
	echo "GitHub Actions will then build all three systems and publish the release $tag."
	read -r -p "Push? [y/N] " yes
	if [ "$yes" = y ] || [ "$yes" = Y ]; then
		git tag -a "$tag" -m "Emu_Darwin $ver"
		git push origin HEAD "$tag"
		echo "Pushed. The build and the release: the repository's Actions tab, then Releases."
	else
		echo "Not pushed; nothing was tagged."
	fi
else
	echo
	echo "To publish: scripts/release.sh $ver --push   (or: git tag $tag && git push origin $tag)"
fi
