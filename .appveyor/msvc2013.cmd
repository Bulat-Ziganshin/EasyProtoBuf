@echo off
setlocal EnableExtensions

rem AppVeyor exposes PLATFORM from the two-entry matrix in appveyor.yml.
if /I "%PLATFORM%"=="x64" (
    set "EASYPB_VS_ARCH=amd64"
) else if /I "%PLATFORM%"=="Win32" (
    set "EASYPB_VS_ARCH=x86"
) else (
    echo ERROR: unsupported AppVeyor PLATFORM "%PLATFORM%".
    exit /b 1
)

set "EASYPB_VCVARS=%VS120COMNTOOLS%..\..\VC\vcvarsall.bat"
if not exist "%EASYPB_VCVARS%" (
    echo ERROR: Visual Studio 2013 vcvarsall.bat was not found:
    echo   %EASYPB_VCVARS%
    exit /b 1
)

echo ============================================================
echo EasyProtoBuf legacy MSVC job
echo   AppVeyor image: %APPVEYOR_BUILD_WORKER_IMAGE%
echo   Platform:       %PLATFORM%
echo   vcvarsall arch: %EASYPB_VS_ARCH%
echo ============================================================

call "%EASYPB_VCVARS%" %EASYPB_VS_ARCH%
if errorlevel 1 exit /b %errorlevel%

echo.
echo ---- Toolchain ------------------------------------------------
cmake --version
if errorlevel 1 exit /b %errorlevel%
where cl
if errorlevel 1 exit /b %errorlevel%
cl > msvc-version.txt 2>&1
type msvc-version.txt
del msvc-version.txt

rem Fail explicitly if the image ever stops selecting the VS2013 compiler.
> msvc-v120-check.cpp echo #if !defined(_MSC_VER) ^|^| _MSC_VER != 1800
>>msvc-v120-check.cpp echo #error Expected Visual Studio 2013 / MSVC 18.0 / _MSC_VER 1800
>>msvc-v120-check.cpp echo #endif
>>msvc-v120-check.cpp echo int main^(^) { return 0; }
cl /nologo /EHsc /W3 msvc-v120-check.cpp /Fe:msvc-v120-check.exe
if errorlevel 1 exit /b %errorlevel%
msvc-v120-check.exe
if errorlevel 1 exit /b %errorlevel%
del msvc-v120-check.cpp msvc-v120-check.obj msvc-v120-check.exe

echo.
echo ---- Builds ---------------------------------------------------
for %%C in (Debug Release MinSizeRel) do (
    call :build_and_test %%C
    if errorlevel 1 exit /b 1
)

exit /b 0

:build_and_test
set "EASYPB_CONFIG=%~1"
set "EASYPB_BUILD_DIR=build-vs2013-%PLATFORM%-%EASYPB_CONFIG%"

echo.
echo ============================================================
echo %PLATFORM% / %EASYPB_CONFIG%
echo ============================================================

if exist "%EASYPB_BUILD_DIR%" rmdir /S /Q "%EASYPB_BUILD_DIR%"
mkdir "%EASYPB_BUILD_DIR%"
if errorlevel 1 exit /b %errorlevel%

pushd "%EASYPB_BUILD_DIR%"

rem NMake deliberately avoids dependence on CMake's deprecated/removed
rem "Visual Studio 12 2013" generator. vcvarsall above selects the real v120
rem compiler and the requested x86/x64 target environment.
cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=%EASYPB_CONFIG% "-DEASYPB_CXX_FLAGS=/W3" ..
if errorlevel 1 goto :build_failed

cmake --build .
if errorlevel 1 goto :build_failed

ctest --output-on-failure
if errorlevel 1 goto :build_failed

popd
exit /b 0

:build_failed
set "EASYPB_RESULT=%errorlevel%"
popd
exit /b %EASYPB_RESULT%
