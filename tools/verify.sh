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
#   --presets     does every factory preset put its copies on the raster
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

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# The effect build of the background pass, spliced in by Flipbook.cpp.
VARIANTS = {
	"kBackgroundFragmentShader": [
		( "effect", "#define FLIPBOOK_OVER_INPUT 1\n" ),
	],
}

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
		for label, defines in VARIANTS.get( name, [] ):
			# The plugin splices these in after the #version line, which has to
			# stay first. Each build is a separate compile and can fail alone.
			head, rest = body.split( "\n", 1 )
			emit( name + "_" + label, head + "\n" + defines + rest )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders: every one through a real GLSL compiler"
check shaders_compile

step "the cell on screen, against Playback.cpp"
# Needs no GPU, so it goes first: a machine that cannot make a GL context
# can still run it.
check "$BUILD/fbtest" --rate

check "$BUILD/fbtest" --frames

step "where every copy landed, against Placement.cpp"
check "$BUILD/fbtest" --copies

step "a square cell stays square off 1:1"
check "$BUILD/fbtest" --aspect

step "no cell bleeds into its neighbour"
check "$BUILD/fbtest" --seam

step "the four key modes"
check "$BUILD/fbtest" --key

step "every factory preset frames itself"
check "$BUILD/fbtest" --presets

step "no dead controls"
check python3 tools/sweep.py --build "$BUILD"

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

	if ! grep -q plugMain <<<"$symbols"; then
		printf '\033[31mFAILED: %s exports no plugMain\033[0m\n' "$name"
		failures=$((failures + 1))
		continue
	fi

	# The plugin id is a four-character literal in the registration, so it is in
	# the binary's strings if and only if that translation unit survived.
	if ! grep -qx "$want" <<<"$literals"; then
		printf '\033[31mFAILED: %s does not carry its own id %s\033[0m\n' "$name" "$want"
		failures=$((failures + 1))
	elif grep -qx "$unwanted" <<<"$literals"; then
		printf '\033[31mFAILED: %s also carries %s -- both registrations linked in\033[0m\n' \
			"$name" "$unwanted"
		failures=$((failures + 1))
	else
		printf 'ok   %s carries %s and not %s\n' "$name" "$want" "$unwanted"
	fi
done

# ---------------------------------------------------------------------------
# The OpenFX bundle's plist.
#
# This exists because it went wrong. cmake/InfoOFX.plist.in was copied from
# another repo with that plugin's name hardcoded into CFBundleExecutable, and
# NOTHING caught it: the bundle assembles, the binary is universal, the OFX
# entry point exports, ofxprobe loads it and renders through it. It fails only
# at release time, in codesign, with a message that names a "subcomponent" and
# never mentions the plist.
#
# So the check is the release step itself, run here where it is cheap: does the
# plist name the binary that is actually there, and does an ad-hoc sign of the
# bundle succeed. On a copy of the bundle, so a verify run never leaves a
# signature on the build tree that the release job did not put there.
# ---------------------------------------------------------------------------
if [ -d "$BUILD/Flipbook.ofx.bundle" ]; then
	step "the OpenFX bundle signs"

	plist="$BUILD/Flipbook.ofx.bundle/Contents/Info.plist"
	named=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$plist" 2>/dev/null)
	if [ ! -f "$BUILD/Flipbook.ofx.bundle/Contents/MacOS/$named" ]; then
		printf '\033[31mFAILED: Info.plist names "%s", which is not in Contents/MacOS\033[0m\n' "$named"
		failures=$((failures + 1))
	else
		scratch="${TMPDIR:-/tmp}/flipbook-signcheck.ofx.bundle"
		rm -rf "$scratch"
		cp -R "$BUILD/Flipbook.ofx.bundle" "$scratch"
		if codesign --force --sign - --timestamp=none "$scratch" >/dev/null 2>&1; then
			printf 'ok   CFBundleExecutable is %s, and the bundle ad-hoc signs\n' "$named"
		else
			printf '\033[31mFAILED: the OpenFX bundle will not codesign\033[0m\n'
			codesign --force --sign - --timestamp=none "$scratch" 2>&1 | sed 's/^/       /'
			failures=$((failures + 1))
		fi
		rm -rf "$scratch"
	fi
fi

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
