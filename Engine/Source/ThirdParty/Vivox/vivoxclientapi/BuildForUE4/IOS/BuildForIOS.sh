#!/bin/sh

# Copyright Epic Games, Inc. All Rights Reserved.

SCRIPT_DIR=$(cd $(dirname $0) && pwd)

BUILD_DIR="${SCRIPT_DIR}/../../IOS/Build"

if [ -d "${BUILD_DIR}" ]; then
	rm -rf "${BUILD_DIR}"
fi
mkdir -pv "${BUILD_DIR}"

cd "${BUILD_DIR}"
../../../../../../Extras/ThirdPartyNotUE/CMake/bin/cmake -G "Xcode" -DVIVOXSDK_PATH=../../vivox-sdk/Include -DCMAKE_XCODE_ATTRIBUTE_SDKROOT=iphoneos -DCMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET=8.0 -DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO -DCMAKE_XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY=1,2 -DCMAKE_XCODE_ATTRIBUTE_BITCODE_GENERATION_MODE=bitcode -DUSE_LOGIN_SESSION_AUDIO_SETTINGS=1 -DVALIDATE_AUDIO_DEVICE_SELECTION=1 "${SCRIPT_DIR}/.."

function build()
{
	CONFIGURATION=$1
	xcodebuild -project vivoxclientapi.xcodeproj -configuration $CONFIGURATION -destination generic/platform=iOS
}

build RelWithDebInfo
rm -rf ../Release
mv -v ../RelWithDebInfo ../Release
build Debug
cd "${SCRIPT_DIR}"
rm -rf "${BUILD_DIR}"
