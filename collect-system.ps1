param([string]$Model = '')
$ErrorActionPreference = 'Continue'
$log = Join-Path $PSScriptRoot ('albion-system-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
$lines = New-Object 'System.Collections.Generic.List[string]'
function AddLine([string]$Text) { $script:lines.Add($Text); Write-Host $Text }
AddLine 'Albion diagnostic: system inventory (run on affected PC).'
AddLine ('Time: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))
AddLine ('App directory: ' + $PSScriptRoot)
try { $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop; AddLine ('OS: ' + $os.Caption + ' version=' + $os.Version + ' build=' + $os.BuildNumber) } catch { AddLine ('OS query error: ' + $_.Exception.Message) }
try { Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { AddLine ('Display GPU: ' + $_.Name + '; driver=' + $_.DriverVersion) } } catch { AddLine ('GPU query error: ' + $_.Exception.Message) }
$nv = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
if (-not $nv) { $nvPath = Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'; if (Test-Path -LiteralPath $nvPath) { $nv = Get-Item -LiteralPath $nvPath } }
if ($nv) {
  AddLine '--- nvidia-smi ---'
  try { $nvPath = if ($nv.Source) { $nv.Source } else { $nv.FullName }; $gpuLines = & $nvPath 2>&1; foreach ($line in $gpuLines) { AddLine ([string]$line) } } catch { AddLine ('nvidia-smi error: ' + $_.Exception.Message) }
} else { AddLine 'nvidia-smi.exe not found; NVIDIA driver may be absent or PATH unavailable.' }
AddLine '--- Runtime DLLs next to this script / app ---'
foreach ($name in @('nvinfer_10.dll','cudart64_110.dll','nvonnxparser_10.dll')) {
  $path = Join-Path $PSScriptRoot $name
  if (Test-Path -LiteralPath $path) { $item = Get-Item -LiteralPath $path; AddLine ($name + ': present; file version=' + $item.VersionInfo.FileVersion + '; bytes=' + $item.Length) }
  else { AddLine ($name + ': not in app folder (may still be available from PATH). ONNX parser is needed for engine building, not ordinary runtime.') }
}
if ($Model) {
 try { $item = Get-Item -LiteralPath $Model -ErrorAction Stop; $hash = Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256 -ErrorAction Stop; AddLine ('Model: ' + $item.FullName + '; bytes=' + $item.Length + '; SHA256=' + $hash.Hash) }
 catch { AddLine ('Model inventory error: ' + $_.Exception.Message) }
}
try { [System.IO.File]::WriteAllLines($log, $lines, (New-Object System.Text.UTF8Encoding($false))); Write-Host ('Saved: ' + $log) }
catch { Write-Host ('Could not save log: ' + $_.Exception.Message); exit 1 }
