#!/bin/bash
#
# Builds libdashbls (with the blst backend CMake fetches) for Apple platforms,
# for consumption by rust-bindings' "apple" feature.
#
# Usage: apple.rust.deps.sh <rust-target-triple>
#
# Artefacts are placed in build/artefacts/<triple>/: libdashbls.a and
# libmimalloc-secure.a. The former contains the blst objects; no external
# arithmetic library (the relic-era GMP dependency is gone).

set -e

TARGET=$1

MACOS="macosx"
IPHONEOS="iphoneos"
IPHONESIMULATOR="iphonesimulator"

BUILD="build"

LOGICALCPU_MAX=$(sysctl -n hw.logicalcpu_max)

prepare() {
    # shellcheck disable=SC2039,SC2164
    pushd ${BUILD}

    download_cmake_toolchain() {
        if [ ! -s "ios.toolchain.cmake" ]; then
            SHA256_HASH="d02857ff6bd64f1d7109ca59c3e4f3b2f89d0663c412146e6977c679801b3243"
            echo "Downloading ios.toolchain.cmake"
            curl -o "ios.toolchain.cmake" https://raw.githubusercontent.com/leetal/ios-cmake/c55677a4445b138c9ef2650d3c21f22cc78c2357/ios.toolchain.cmake
            DOWNLOADED_HASH=$(shasum -a 256 ios.toolchain.cmake | cut -f 1 -d " ")
            if [ "$SHA256_HASH" != "$DOWNLOADED_HASH" ]; then
              echo "Error: sha256 checksum of ios.toolchain.cmake mismatch" >&2
              exit 1
            fi
        fi
    }

    download_cmake_toolchain

    # shellcheck disable=SC2039,SC2164
    popd # build
}

# Maps (platform, arch) onto the leetal/ios-cmake PLATFORM value.
cmake_platform() {
    PLATFORM=$1
    ARCH=$2
    if [[ $PLATFORM = "$IPHONEOS" ]]; then
        echo "OS64"
    elif [[ $PLATFORM = "$IPHONESIMULATOR" && $ARCH = "x86_64" ]]; then
        echo "SIMULATOR64"
    elif [[ $PLATFORM = "$IPHONESIMULATOR" && $ARCH = "arm64" ]]; then
        echo "SIMULATORARM64"
    elif [[ $PLATFORM = "$MACOS" && $ARCH = "x86_64" ]]; then
        echo "MAC"
    elif [[ $PLATFORM = "$MACOS" && $ARCH = "arm64" ]]; then
        echo "MAC_ARM64"
    fi
}

build_bls_arch() {
    PLATFORM=$1
    ARCH=$2
    PFX=${PLATFORM}-${ARCH}
    CMAKE_PLATFORM=$(cmake_platform "$PLATFORM" "$ARCH")
    BUILDDIR=${BUILD}/bls-"${PFX}"

    rm -rf "$BUILDDIR"

    cmake -S . -B "$BUILDDIR" \
        -DCMAKE_TOOLCHAIN_FILE="$(pwd)/${BUILD}/ios.toolchain.cmake" \
        -DPLATFORM="${CMAKE_PLATFORM}" \
        -DDEPLOYMENT_TARGET=13.0 \
        -DENABLE_BITCODE=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_BLS_TESTS=0 \
        -DBUILD_BLS_BENCHMARKS=0 \
        -DBUILD_BLS_JS_BINDINGS=0 \
        -DBUILD_BLS_PYTHON_BINDINGS=0

    cmake --build "$BUILDDIR" --parallel "$LOGICALCPU_MAX" --target dashbls
}

build_target() {
    BUILD_IN=$1
    echo "Build target: $BUILD_IN"
    ARCH=""
    PLATFORM=""
    # shellcheck disable=SC2039
    if [[ $BUILD_IN = "x86_64-apple-ios" ]]; then
      ARCH=x86_64
      PLATFORM=$IPHONESIMULATOR
    elif [[ $BUILD_IN = "aarch64-apple-ios" ]]; then
      ARCH=arm64
      PLATFORM=$IPHONEOS
    elif [[ $BUILD_IN = "aarch64-apple-ios-sim" ]]; then
      ARCH=arm64
      PLATFORM=$IPHONESIMULATOR
    elif [[ $BUILD_IN = "x86_64-apple-darwin" ]]; then
      ARCH=x86_64
      PLATFORM=$MACOS
    elif [[ $BUILD_IN = "aarch64-apple-darwin" ]]; then
      ARCH=arm64
      PLATFORM=$MACOS
    fi
    build_bls_arch "$PLATFORM" "$ARCH"
    PFX="${PLATFORM}"-"${ARCH}"
    rm -rf "build/artefacts/${BUILD_IN}"
    mkdir -p "build/artefacts/${BUILD_IN}"
    cp "build/bls-${PFX}/src/libdashbls.a" "build/artefacts/${BUILD_IN}"
    cp "build/bls-${PFX}/depends/mimalloc/libmimalloc-secure.a" "build/artefacts/${BUILD_IN}"
}

mkdir -p ${BUILD}
prepare
build_target "$TARGET"
