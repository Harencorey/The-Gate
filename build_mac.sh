#!/bin/bash
# Build The Gate (AU + VST3 + Standalone) on macOS, install the plug-ins and run the tests.
#   bash build_mac.sh                 -> downloads JUCE 8.0.15 automatically
#   bash build_mac.sh /path/to/JUCE   -> uses your own JUCE checkout instead
set -euo pipefail
cd "$(dirname "$0")"

command -v cmake >/dev/null || { echo "CMake not found. Install it with:  brew install cmake"; exit 1; }
xcode-select -p >/dev/null 2>&1 || { echo "Xcode command line tools missing. Run:  xcode-select --install"; exit 1; }

JUCE_ARG=()
if [ $# -ge 1 ]; then JUCE_ARG=(-DJUCE_DIR="$1"); fi

cmake -B build-mac -DCMAKE_BUILD_TYPE=Release "${JUCE_ARG[@]+"${JUCE_ARG[@]}"}"
cmake --build build-mac --config Release --target TheGate_AU TheGate_VST3 TheGate_Standalone GateTests GateProcessorTests -j "$(sysctl -n hw.ncpu)"

echo "Running tests..."
./build-mac/GateTests
./build-mac/GateProcessorTests_artefacts/Release/GateProcessorTests

killall -9 AudioComponentRegistrar 2>/dev/null || true

echo
echo "Installed:"
ls -d ~/Library/Audio/Plug-Ins/Components/"The Gate.component" ~/Library/Audio/Plug-Ins/VST3/"The Gate.vst3" 2>/dev/null || true
echo "Standalone app: $(pwd)/build-mac/TheGate_artefacts/Release/Standalone/The Gate.app"
echo "Rescan plug-ins in your DAW and look for Norka DSP > The Gate.  (Logic: auval -v aufx Gate Nrka)"
