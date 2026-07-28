#!/usr/bin/env bash
# Configure + build the Linux relay host.
#
# Use the PRESET. A bare `cmake -S . -B` defaults to Unix Makefiles and collides with the
# preset's Ninja generator.
#
# Tools OFF: nearly every target under */Code/Tools and Core/Tools is a Windows GUI program
# (afxwin.h / commctrl.h / windows.h) - WorldBuilder, GUIEdit, W3DView, ImagePacker, ParticleEditor
# and friends. The *_TOOLS options default to ON and linux64-deploy does not override them, so the
# Linux build spent thousands of objects compiling game code only to die in a tool it does not need.
# A relay ships no editors.
#
# FFMPEG=ON, despite a relay decoding no video. OFF does NOT work: Core/GameEngineDevice/
# CMakeLists.txt:305 compiles Source/VideoDevice/FFmpeg/FFmpegFile.cpp when FFMPEG is OFF
# because OpenALAudioCache needs it for AUDIO decoding, but the ffmpeg libraries are only linked
# when FFMPEG is ON. So OFF + SAGE_USE_OPENAL compiles ffmpeg-calling code and links nothing,
# and the build dies at the final link of GeneralsXZH with ~30 undefined av_*/avcodec_* symbols.
# Ubuntu 24.04 ships ffmpeg 6.x (libavcodec 60), which has the AVFrame::ch_layout API the audio
# manager requires - that is exactly why 24.04 was the right OS choice.
# GENERALS=OFF: only Zero Hour is the deployment target.
set -x
export VCPKG_ROOT=/root/vcpkg
cd /root/GeneralsX || exit 1

cmake --preset linux64-deploy \
  -DRTS_BUILD_OPTION_FFMPEG=ON \
  -DRTS_BUILD_CORE_TOOLS=OFF \
  -DRTS_BUILD_CORE_EXTRAS=OFF \
  -DRTS_BUILD_ZEROHOUR_TOOLS=OFF \
  -DRTS_BUILD_ZEROHOUR_EXTRAS=OFF \
  -DRTS_BUILD_GENERALS=OFF
CONFIG_RC=$?
echo "CONFIG_RC=$CONFIG_RC"
if [ "$CONFIG_RC" != "0" ]; then echo "=== CONFIGURE FAILED ==="; exit 1; fi

cmake --build build/linux64-deploy -j4
BUILD_RC=$?
echo "BUILD_RC=$BUILD_RC"
if [ "$BUILD_RC" != "0" ]; then echo "=== BUILD FAILED ==="; exit 1; fi

echo "=== VPS BUILD DONE ==="
find build/linux64-deploy -maxdepth 3 -type f -perm -u+x \( -name "*eneral*" -o -name "*ZH*" \) -exec ls -la {} \;
