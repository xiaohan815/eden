#!/usr/bin/env bash
# Build the local Apple Silicon checkout using the dependencies prepared here.
set -euo pipefail

eden_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
eden_build="${EDEN_BUILD_DIR:-$eden_root/build/macos-core}"
eden_cmake="${EDEN_CMAKE:-$eden_root/.cache/build-tools/bin/cmake}"
eden_ninja="${EDEN_NINJA:-$eden_root/.cache/build-tools/bin/ninja}"
eden_brew="$(brew --prefix)"
eden_boost="${EDEN_BOOST_PREFIX:-$eden_root/.cache/system-deps/boost/1.92.0}"
eden_moltenvk="${EDEN_MOLTENVK_LIBRARY:-/Applications/eden.app/Contents/Frameworks/libMoltenVK.dylib}"

[[ -x "$eden_cmake" ]] || eden_cmake="$(command -v cmake)"
[[ -x "$eden_ninja" ]] || eden_ninja="$(command -v ninja)"
[[ -d "$eden_boost/include" ]] || eden_boost="$eden_brew/opt/boost"
[[ -f "$eden_moltenvk" ]] || { echo "Missing MoltenVK: $eden_moltenvk" >&2; exit 1; }

"$eden_cmake" -S "$eden_root" -B "$eden_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_MAKE_PROGRAM="$eden_ninja" \
    -DCMAKE_PREFIX_PATH="$eden_boost;$eden_brew/opt/qtbase;$eden_brew/opt/qtcharts;$eden_brew/opt/qttools" \
    -DCPM_SOURCE_CACHE="$eden_root/.cache/cpm" -DBoost_USE_STATIC_LIBS=ON \
    -DQt6_DIR="$eden_brew/opt/qtbase/lib/cmake/Qt6" \
    -DQt6Charts_DIR="$eden_brew/opt/qtcharts/lib/cmake/Qt6Charts" \
    -DQt6LinguistTools_DIR="$eden_brew/opt/qttools/lib/cmake/Qt6LinguistTools" \
    -DYUZU_QT_BASE_TRANSLATIONS="$eden_brew/opt/qttranslations/share/qt/translations" \
    -DGLSLANGVALIDATOR="$eden_brew/bin/glslangValidator" \
    -DOPENSSL_ROOT_DIR="$eden_brew/opt/openssl@3" \
    -DMOLTENVK_LIBRARY="$eden_moltenvk" \
    -DENABLE_QT=ON -DENABLE_LTO=OFF -DENABLE_LIBUSB=OFF \
    -DENABLE_WEB_SERVICE=OFF -DENABLE_UPDATE_CHECKER=OFF \
    -DENABLE_QT_TRANSLATION=ON -DYUZU_USE_QT_MULTIMEDIA=OFF \
    -DYUZU_MACOS_ADHOC_SIGN=ON \
    -DYUZU_USE_QT_WEB_ENGINE=OFF -DYUZU_USE_BUNDLED_QT=OFF \
    -DYUZU_USE_BUNDLED_OPENSSL=OFF -DYUZU_USE_BUNDLED_MOLTENVK=OFF \
    -DYUZU_USE_BUNDLED_FFMPEG=OFF -DYUZU_DISABLE_LLVM=ON \
    -Dzstd_FORCE_BUNDLED=ON -DDYNARMIC_TESTS=ON \
    -DDYNARMIC_TESTS_USE_UNICORN=OFF -DYUZU_TESTS=OFF -DYUZU_ROOM=OFF

"$eden_cmake" --build "$eden_build" --target yuzu dynarmic_tests --parallel "${EDEN_JOBS:-16}"
"$eden_build/bin/dynarmic_tests" '[a64]~[.]~[unicorn]' --abort
printf '\nBuilt app: %s\n' "$eden_build/bin/eden.app"
