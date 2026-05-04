#!/bin/bash
#
# Apple-Silicon-only xcframework: bundles arm64-ios (device) +
# arm64-ios-simulator. Skips x64-ios-simulator since Apple Silicon
# Macs run the simulator natively as arm64. The upstream
# `create_xcframework.sh` builds a fat universal simulator slice; this
# variant skips that step so a single dev machine can produce a
# usable xcframework for local SPM consumption without paying the
# ~25-minute x86_64 build cost.
#
# Output: build/apple/valhalla-wrapper.xcframework
#
set -e

cd "$(dirname "$0")/.."

if [ ! -d build/apple/arm64-ios/install/lib ]; then
    echo "Missing arm64-ios install. Run: ./build.sh --ios arm64-ios"
    exit 1
fi
if [ ! -d build/apple/arm64-ios-simulator/install/lib ]; then
    echo "Missing arm64-ios-simulator install. Run: ./build.sh --ios arm64-ios-simulator"
    exit 1
fi

libtool -static -o build/apple/arm64-ios/libvalhalla_all.a \
    build/apple/arm64-ios/install/lib/*.a

libtool -static -o build/apple/arm64-ios-simulator/libvalhalla_all.a \
    build/apple/arm64-ios-simulator/install/lib/*.a

rm -rf build/apple/valhalla-wrapper.xcframework

xcodebuild -create-xcframework \
    -library build/apple/arm64-ios/libvalhalla_all.a \
        -headers build/apple/arm64-ios/install/include \
    -library build/apple/arm64-ios-simulator/libvalhalla_all.a \
        -headers build/apple/arm64-ios-simulator/install/include \
    -output build/apple/valhalla-wrapper.xcframework

echo "Wrote build/apple/valhalla-wrapper.xcframework"
