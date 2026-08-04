#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# Each check answers a question none of the others can:
#
#   --frames      does the cell on screen match Playback.cpp, across all three
#                 modes and a run that wraps off the end of the sheet
#   --copies      did every copy land where Placement.cpp said, showing the
#                 frame the stagger said
#   --aspect      does a square cell stay square, and the right size, off 1:1
#   --seam        does any cell bleed into its neighbour under either filter
#   --key         do the four key modes do four different things
#   sweep.py      does every control actually reach the picture
#   registration  does each bundle contain exactly its own plugin
#   lipo          is the macOS build really universal
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
failures=0

step() {
	printf '\n\033[1m== %s\033[0m\n' "$1"
}

check() {
	if "$@"; then
		return 0
	fi
	printf '\033[31mFAILED: %s\033[0m\n' "$*"
	failures=$((failures + 1))
}

if [ ! -x "$BUILD/fbtest" ]; then
	echo "$BUILD/fbtest not found."
	echo "Configure with -DFLIPBOOK_BUILD_TOOLS=ON and build first:"
	echo "  cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

step "the cell on screen, against Playback.cpp"
check "$BUILD/fbtest" --frames

step "where every copy landed, against Placement.cpp"
check "$BUILD/fbtest" --copies

step "a square cell stays square off 1:1"
check "$BUILD/fbtest" --aspect

step "no cell bleeds into its neighbour"
check "$BUILD/fbtest" --seam

step "the four key modes"
check "$BUILD/fbtest" --key

step "no dead controls"
check python3 tools/sweep.py

# ---------------------------------------------------------------------------
# Registration.
#
# The failure this catches is specific and silent: `CFFGLPluginInfo` registers
# itself from a file-scope constructor and nothing references it by name, so a
# linker that drops the translation unit gives a bundle which loads, exports
# plugMain, and reports that it contains no plugins. Resolume shows an empty
# effects list and no error.
#
# It also catches the opposite mistake -- putting a registration in the shared
# library, which registers BOTH plugins into BOTH bundles.
# ---------------------------------------------------------------------------
step "each bundle contains exactly its own plugin"
for pair in "Flipbook:FB01:FB02" "Flipbook Over:FB02:FB01"; do
	name="${pair%%:*}"
	rest="${pair#*:}"
	want="${rest%%:*}"
	unwanted="${rest#*:}"

	binary="$BUILD/$name.bundle/Contents/MacOS/$name"
	if [ ! -f "$binary" ]; then
		printf '\033[31mFAILED: %s not built\033[0m\n' "$binary"
		failures=$((failures + 1))
		continue
	fi

	# Read once into variables rather than piping into `grep -q`.
	#
	# `grep -q` exits the instant it matches, which closes the pipe under the
	# still-running `nm` or `strings`; they take SIGPIPE and exit 141, and with
	# `set -o pipefail` the *pipeline* is then a failure however well the grep
	# went. That reported both bundles as unregistered when both were fine, and
	# it is only intermittent from the shell -- a short output fits the pipe
	# buffer and the writer finishes before the reader leaves.
	symbols=$(nm -gU "$binary" 2>/dev/null)
	literals=$(strings "$binary" 2>/dev/null)

	if ! printf '%s\n' "$symbols" | grep -q plugMain; then
		printf '\033[31mFAILED: %s exports no plugMain\033[0m\n' "$name"
		failures=$((failures + 1))
		continue
	fi

	# The plugin id is a four-character literal in the registration, so it is in
	# the binary's strings if and only if that translation unit survived.
	if ! printf '%s\n' "$literals" | grep -q "^$want\$"; then
		printf '\033[31mFAILED: %s does not carry its own id %s\033[0m\n' "$name" "$want"
		failures=$((failures + 1))
	elif printf '%s\n' "$literals" | grep -q "^$unwanted\$"; then
		printf '\033[31mFAILED: %s also carries %s -- both registrations linked in\033[0m\n' \
			"$name" "$unwanted"
		failures=$((failures + 1))
	else
		printf 'ok   %s carries %s and not %s\n' "$name" "$want" "$unwanted"
	fi
done

# ---------------------------------------------------------------------------
# Universal.
#
# CMake latches CMAKE_OSX_ARCHITECTURES when the first target is created, so
# setting it late is silently ignored and the build log still says success. The
# only honest answer comes from lipo. Skipped when the developer asked for a
# single-architecture build on purpose.
# ---------------------------------------------------------------------------
step "the macOS build is universal"
if grep -q "CMAKE_OSX_ARCHITECTURES:.*arm64;x86_64" "$BUILD/CMakeCache.txt" 2>/dev/null; then
	for name in "Flipbook" "Flipbook Over"; do
		binary="$BUILD/$name.bundle/Contents/MacOS/$name"
		arches=$(lipo -archs "$binary" 2>/dev/null)
		case "$arches" in
			*arm64*x86_64* | *x86_64*arm64*)
				printf 'ok   %s: %s\n' "$name" "$arches" ;;
			*)
				printf '\033[31mFAILED: %s is %s, not universal\033[0m\n' "$name" "${arches:-missing}"
				failures=$((failures + 1)) ;;
		esac
	done
else
	echo "skipped: this build was configured for one architecture"
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit "$failures"
