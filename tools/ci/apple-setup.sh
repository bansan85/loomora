#!/usr/bin/env bash
# Prepares an Apple job of .github/workflows/apple.yml (T126) on the macOS runner:
#   1. prints, and saves in <log dir>/toolchain.txt, every version the procedures macos.md and ios.md
#      need: the runner image, macOS, Xcode (all those installed), clang, Swift, the SDKs, the simulator
#      runtimes, CMake, Ninja, Python;
#   2. makes a Python virtual environment with jsonschema (tools/check_probe.py needs it; the Python of the
#      image is managed by Homebrew, pip refuses to install into it) and puts it first in PATH for the
#      next steps of the job, so that `python` and `python3` are the ones of the environment.
#
# usage: tools/ci/apple-setup.sh <log dir>
# It never fails because a version cannot be read: the facts are for the log, not a gate.
set -u
log_dir="${1:?usage: apple-setup.sh <log dir>}"
mkdir -p "$log_dir"
facts="$log_dir/toolchain.txt"

show() {
  # show LABEL COMMAND...: the command, its output (stderr included) and its exit code.
  local label="$1"
  shift
  {
    echo "--- $label"
    printf '$'
    printf ' %q' "$@"
    echo
    "$@" 2>&1
    echo "--- exit code: $?"
  } | tee -a "$facts"
}

: > "$facts"
{
  echo "===== RUNNER IMAGE ====="
  echo "runner: ${RUNNER_OS:-?} ${RUNNER_ARCH:-?}, name ${RUNNER_NAME:-?}"
  echo "image: ${ImageOS:-?} version ${ImageVersion:-?}"
} | tee -a "$facts"
show "sw_vers" sw_vers
show "uname -m" uname -m
echo "===== XCODE, CLANG, SWIFT, SDKS =====" | tee -a "$facts"
show "xcodebuild -version" xcodebuild -version
show "xcode-select -p" xcode-select -p
show "Xcode versions installed" bash -c 'ls -d /Applications/Xcode*'
show "xcrun clang --version" xcrun clang --version
show "xcrun swift --version" xcrun swift --version
for sdk in macosx iphoneos iphonesimulator; do
  show "SDK $sdk: version" xcrun --sdk "$sdk" --show-sdk-version
  show "SDK $sdk: platform version" xcrun --sdk "$sdk" --show-sdk-platform-version
  show "SDK $sdk: path" xcrun --sdk "$sdk" --show-sdk-path
done
echo "===== SIMULATORS =====" | tee -a "$facts"
show "simctl runtimes" xcrun simctl list runtimes
show "simctl device types (iPhone)" bash -c 'xcrun simctl list devicetypes | grep iPhone'
echo "===== BUILD TOOLS =====" | tee -a "$facts"
show "cmake --version" cmake --version
show "ninja --version" ninja --version
show "python3 --version (image)" python3 --version

echo "===== PYTHON ENVIRONMENT (jsonschema) =====" | tee -a "$facts"
venv="${RUNNER_TEMP:-/tmp}/loomora-venv"
python3 -m venv "$venv"
"$venv/bin/python" -m pip install --disable-pip-version-check --quiet jsonschema
show "python in the environment" "$venv/bin/python" --version
show "jsonschema" "$venv/bin/python" -c 'import jsonschema; print(jsonschema.__version__)'
if [ -n "${GITHUB_PATH:-}" ]; then
  echo "$venv/bin" >> "$GITHUB_PATH"
fi
exit 0
