#!/bin/bash
# Build ThorVG static library for x-os
set -e

CXX=/opt/homebrew/opt/llvm/bin/clang++
AR=/opt/homebrew/opt/llvm/bin/llvm-ar
THORVG_DIR=userspace/lib/thorvg
BUILD_DIR=build/thorvg
NEWLIB_PREFIX=/opt/x-os-newlib/x86_64-elf

mkdir -p $BUILD_DIR

CXXFLAGS=(
  --target=x86_64-unknown-none-elf
  -nostdlib -nostdinc++
  -Iuserspace/lib/libcxx_config
  -isystem /opt/homebrew/opt/llvm/include/c++/v1
  -isystem $NEWLIB_PREFIX/include
  -I$THORVG_DIR
  -I$THORVG_DIR/inc
  -I$THORVG_DIR/src/renderer
  -I$THORVG_DIR/src/renderer/cpu_engine
  -I$THORVG_DIR/src/loaders/svg
  -I$THORVG_DIR/src/loaders/sfnt
  -I$THORVG_DIR/src/loaders/raw
  -I$THORVG_DIR/src/common
  -I$THORVG_DIR/stubs
  -include $THORVG_DIR/thorvg_config.h
  -fno-exceptions -fno-rtti -fno-threadsafe-statics
  -O2 -std=c++17
  -DFP_NAN=0 -DFP_INFINITE=1 -DFP_NORMAL=4 -DFP_SUBNORMAL=5 -DFP_ZERO=3
)

# Source files to compile
SOURCES=(
  # common
  $THORVG_DIR/src/common/tvgMath.cpp
  $THORVG_DIR/src/common/tvgStr.cpp
  $THORVG_DIR/src/common/tvgCompressor.cpp
  # renderer
  $THORVG_DIR/src/renderer/tvgAccessor.cpp
  $THORVG_DIR/src/renderer/tvgAnimation.cpp
  $THORVG_DIR/src/renderer/tvgCanvas.cpp
  $THORVG_DIR/src/renderer/tvgFill.cpp
  $THORVG_DIR/src/renderer/tvgInitializer.cpp
  $THORVG_DIR/src/renderer/tvgLoaderMgr.cpp
  $THORVG_DIR/src/renderer/tvgPaint.cpp
  $THORVG_DIR/src/renderer/tvgPicture.cpp
  $THORVG_DIR/src/renderer/tvgRender.cpp
  $THORVG_DIR/src/renderer/tvgSaver.cpp
  $THORVG_DIR/src/renderer/tvgScene.cpp
  $THORVG_DIR/src/renderer/tvgShape.cpp
  $THORVG_DIR/src/renderer/tvgTaskScheduler.cpp
  $THORVG_DIR/src/renderer/tvgText.cpp
  # cpu engine
  $THORVG_DIR/src/renderer/cpu_engine/tvgSwBlendOp.cpp
  $THORVG_DIR/src/renderer/cpu_engine/tvgSwFill.cpp
  $THORVG_DIR/src/renderer/cpu_engine/tvgSwImage.cpp
  $THORVG_DIR/src/renderer/cpu_engine/tvgSwMemPool.cpp
  $THORVG_DIR/src/renderer/cpu_engine/tvgSwPostEffect.cpp
  $THORVG_DIR/src/renderer/cpu_engine/tvgSwRaster.cpp
  $THORVG_DIR/src/renderer/cpu_engine/tvgSwRenderer.cpp
  $THORVG_DIR/src/renderer/cpu_engine/tvgSwRle.cpp
  $THORVG_DIR/src/renderer/cpu_engine/tvgSwShape.cpp
  $THORVG_DIR/src/renderer/cpu_engine/tvgSwStroke.cpp
  $THORVG_DIR/src/renderer/cpu_engine/tvgSwUtil.cpp
  # svg loader
  $THORVG_DIR/src/loaders/svg/tvgSvgBuilder.cpp
  $THORVG_DIR/src/loaders/svg/tvgSvgCssStyle.cpp
  $THORVG_DIR/src/loaders/svg/tvgSvgLoader.cpp
  $THORVG_DIR/src/loaders/svg/tvgSvgPath.cpp
  $THORVG_DIR/src/loaders/svg/tvgSvgUtil.cpp
  $THORVG_DIR/src/loaders/svg/tvgXmlParser.cpp
  # sfnt loader (fonts)
  $THORVG_DIR/src/loaders/sfnt/tvgSfntLoader.cpp
  $THORVG_DIR/src/loaders/sfnt/tvgSfntReader.cpp
  $THORVG_DIR/src/loaders/sfnt/tvgTtfReader.cpp
  # raw loader (always needed by LoaderMgr)
  $THORVG_DIR/src/loaders/raw/tvgRawLoader.cpp
  # C API binding
  $THORVG_DIR/src/bindings/capi/tvgCapi.cpp
  # x-os wrapper
  $THORVG_DIR/thorvg_xos.cpp
  # C++ runtime
  userspace/lib/cpp_runtime/cxx_runtime.cpp
)

OBJS=()
for src in "${SOURCES[@]}"; do
  obj="$BUILD_DIR/$(basename ${src%.cpp}).o"
  echo "  CXX $src"
  $CXX "${CXXFLAGS[@]}" -c "$src" -o "$obj" 2>&1
  OBJS+=("$obj")
done

echo "AR thorvg.a"
$AR rcs $BUILD_DIR/thorvg.a "${OBJS[@]}"
echo ">> built $BUILD_DIR/thorvg.a"
