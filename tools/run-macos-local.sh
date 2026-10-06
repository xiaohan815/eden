#!/usr/bin/env bash
# Run the local build with a separate config, retaining existing game data/keys.
set -euo pipefail

eden_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
eden_build="${EDEN_BUILD_DIR:-$eden_root/build/macos-core}"
eden_binary="$eden_build/bin/eden.app/Contents/MacOS/eden"
eden_config="${EDEN_TEST_CONFIG_DIR:-$eden_root/.cache/game-test/config}"

[[ -x "$eden_binary" ]] || { echo "Run tools/build-macos-local.sh first." >&2; exit 1; }
mkdir -p "$eden_config/eden"
if [[ ! -f "$eden_config/eden/qt-config.ini" && -f "$HOME/.config/eden/qt-config.ini" ]]; then
    cp "$HOME/.config/eden/qt-config.ini" "$eden_config/eden/qt-config.ini"
fi

export XDG_CONFIG_HOME="$eden_config"
exec "$eden_binary" "$@"
