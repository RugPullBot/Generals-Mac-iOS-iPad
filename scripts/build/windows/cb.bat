@echo off
set VCPKG_ROOT=C:\dev\vcpkg
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" %2
set "CM=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d C:\dev\GeneralsX
rem RTS_DEBUG_MULTI_INSTANCE lets this build run alongside retail Generals / Generals Online.
rem Both use the SAME named mutex ("unique to Generals"), so the second one to start exits 1.
rem On Windows this flag ONLY changes the mutex name - the narrow per-instance socket bind it
rem also enables is #ifndef _WIN32, so networking is untouched. It does not move sourceID either,
rem because the SimID digest hashes the cmake FILES, not the configure options.
"%CM%" --preset %1 -DRTS_BUILD_CORE_TOOLS=OFF -DRTS_BUILD_ZEROHOUR_TOOLS=OFF -DRTS_BUILD_GENERALS_TOOLS=OFF -DRTS_DEBUG_MULTI_INSTANCE=ON
echo ===== CONFIGURE EXIT: %errorlevel% =====
"%CM%" --build build/%1 --config Release -j 16 -- -k 0
echo ===== BUILD EXIT: %errorlevel% =====
