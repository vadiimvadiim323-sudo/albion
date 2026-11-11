@echo off
setlocal
if "%~1"=="" (
 echo Usage: benchmark-engine.cmd "C:\path\model.engine"
 exit /b 2
)
if not exist "%~1" (
 echo Engine file not found.
 exit /b 2
)
set "TRT=C:\TensorRT-10.11.0.33\bin\trtexec.exe"
if not exist "%TRT%" set "TRT=trtexec.exe"
echo Running two 30-second tests. These do NOT measure capture or mouse latency.
echo Use the same TensorRT version as the engine. Logs are saved beside this script.
"%TRT%" --loadEngine="%~1" --useCudaGraph --warmUp=1000 --duration=30 --noDataTransfers > "%~dp0trtexec-no-transfers.log" 2>&1
if errorlevel 1 (
 echo First test failed. See trtexec-no-transfers.log. Engine was not changed.
 exit /b 1
)
"%TRT%" --loadEngine="%~1" --useCudaGraph --warmUp=1000 --duration=30 > "%~dp0trtexec-with-transfers.log" 2>&1
if errorlevel 1 (
 echo Second test failed. See trtexec-with-transfers.log. Engine was not changed.
 exit /b 1
)
echo Done. Compare GPU compute, enqueue and transfer times in both logs.
