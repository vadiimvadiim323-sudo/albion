@echo off
rem build_modular.cmd
setlocal
set "BACKEND=trt"
if not "%~1"=="" set "BACKEND=%~1"
if /I not "%BACKEND%"=="trt" if /I not "%BACKEND%"=="winml" goto :usage
set "WGC=false"
if /I "%~2"=="wgc" set "WGC=true"
if not "%~2"=="" if /I not "%~2"=="wgc" goto :usage
if not "%~3"=="" goto :usage
set "TRT=true"
if /I "%BACKEND%"=="winml" set "TRT=false"
if not defined ALBION_CONFIGURATION set "ALBION_CONFIGURATION=Release"
if /I not "%ALBION_CONFIGURATION%"=="Release" if /I not "%ALBION_CONFIGURATION%"=="Debug" goto :usage
if not defined ALBION_TOOLSET set "ALBION_TOOLSET=v145"
if not defined ALBION_COPY_RUNTIME_ASSETS set "ALBION_COPY_RUNTIME_ASSETS=false"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
 echo Visual Studio Installer and C++ Build Tools are required.
 exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT exit /b 1
call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
pushd "%~dp0"
set "VARIANT=%BACKEND%-wgc-%WGC%-%ALBION_CONFIGURATION%"
msbuild "..\vendor\keyauth\library.vcxproj" /nologo /v:minimal /m:2 /t:Build /p:Configuration=%ALBION_CONFIGURATION% /p:Platform=x64 /p:PlatformToolset=%ALBION_TOOLSET% "/p:OutDir=%CD%\keyauth-lib\%ALBION_CONFIGURATION%\\"
if errorlevel 1 goto :failed
msbuild WindowsProject1.vcxproj /nologo /v:minimal /m:2 /t:Build /p:Configuration=%ALBION_CONFIGURATION% /p:Platform=x64 /p:PlatformToolset=%ALBION_TOOLSET% /p:AlbionTensorRT=%TRT% /p:AlbionD3DInterop=%TRT% /p:AlbionWgc=%WGC% /p:AlbionCopyRuntimeAssets=%ALBION_COPY_RUNTIME_ASSETS% /p:MultiProcessorCompilation=true /p:CL_MPCount=2 "/p:OutDir=%CD%\x64\Modular-%VARIANT%\\" "/p:IntDir=%CD%\obj\Modular-%VARIANT%\\"
if errorlevel 1 goto :failed
echo Built: %CD%\x64\Modular-%VARIANT%\WindowsProject1.exe
echo Build only. No application, capture, authentication or mouse output was launched.
popd
exit /b 0
:failed
set "RC=%ERRORLEVEL%"
if "%RC%"=="0" set "RC=1"
popd
exit /b %RC%
:usage
echo Usage: build_modular.cmd [trt^|winml] [wgc]
echo Optional: ALBION_CONFIGURATION=Release or Debug; ALBION_COPY_RUNTIME_ASSETS=true.
exit /b 2
