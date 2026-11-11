@echo off
setlocal
set "ALBION_WGC=0"
set "ALBION_GPU_TIMING=0"
set "ALBION_IO_GRAPH=0"
set "ALBION_COPY_OUTPUT=0"
set "ALBION_LEGACY_WAIT=0"
set "ALBION_PERF_LOG_MS=0"
"%~dp0albion-ui-centered.exe"
