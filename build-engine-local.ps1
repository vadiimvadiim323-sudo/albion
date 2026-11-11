param(
  [string]$Onnx = '',
  [string]$Engine = '',
  [string]$TrtExec = 'C:\TensorRT-10.11.0.33\bin\trtexec.exe',
  [int]$Device = 0,
  [switch]$Fp16,
  [string]$MinShapes = '',
  [string]$OptShapes = '',
  [string]$MaxShapes = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Onnx) { $Onnx = Read-Host 'Full path to trusted ONNX model' }
if (-not $Engine) { $Engine = Read-Host 'Full path for NEW .engine (must not exist)' }
$Onnx = $Onnx.Trim().Trim('"'); $Engine = $Engine.Trim().Trim('"'); $TrtExec = $TrtExec.Trim().Trim('"')
if (-not (Test-Path -LiteralPath $Onnx -PathType Leaf)) { throw 'ONNX file not found.' }
if (-not (Test-Path -LiteralPath $TrtExec -PathType Leaf)) { throw 'trtexec.exe not found. Install matching TensorRT or pass -TrtExec full path.' }
if ([System.IO.Path]::GetExtension($Onnx).ToLowerInvariant() -ne '.onnx') { throw 'Input must be an .onnx file.' }
if ([System.IO.Path]::GetExtension($Engine).ToLowerInvariant() -ne '.engine') { throw 'Output must have .engine extension.' }
if ($Device -lt 0) { throw 'Device index cannot be negative.' }
$Onnx = (Resolve-Path -LiteralPath $Onnx).ProviderPath
$TrtExec = (Resolve-Path -LiteralPath $TrtExec).ProviderPath
$Engine = [System.IO.Path]::GetFullPath($Engine)
$dir = [System.IO.Path]::GetDirectoryName($Engine)
if (-not (Test-Path -LiteralPath $dir -PathType Container)) { throw 'Output directory does not exist.' }
if (Test-Path -LiteralPath $Engine) { throw 'Refusing to overwrite existing engine.' }
$shapes = @($MinShapes, $OptShapes, $MaxShapes)
$present = @($shapes | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count
if ($present -ne 0 -and $present -ne 3) { throw 'For dynamic input provide all of -MinShapes, -OptShapes and -MaxShapes.' }
$token = [guid]::NewGuid().ToString('N')
$temp = Join-Path $dir ('.albion-build-' + $token + '.engine')
$base = Join-Path $dir ('engine-build-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $token.Substring(0,8))
$stdout = $base + '.stdout.log'; $stderr = $base + '.stderr.log'; $info = $base + '.info.log'
$arguments = @(('--onnx="' + $Onnx + '"'), ('--saveEngine="' + $temp + '"'), ('--device=' + $Device), '--skipInference')
if ($Fp16) { $arguments += '--fp16' }
if ($present -eq 3) { $arguments += @(('--minShapes="' + $MinShapes + '"'), ('--optShapes="' + $OptShapes + '"'), ('--maxShapes="' + $MaxShapes + '"')) }
$root = Split-Path -Parent (Split-Path -Parent $TrtExec)
$oldPath = $env:PATH
try {
  $env:PATH = (Join-Path $root 'lib') + ';' + (Join-Path $root 'bin') + ';' + $oldPath
  @(('ONNX: ' + $Onnx), ('ONNX SHA256: ' + (Get-FileHash -LiteralPath $Onnx -Algorithm SHA256).Hash),
    ('Output: ' + $Engine), ('trtexec: ' + $TrtExec), ('trtexec file version: ' + (Get-Item -LiteralPath $TrtExec).VersionInfo.FileVersion),
    ('Device: ' + $Device), ('Flags: ' + ($arguments -join ' ')), 'No automatic portable/version-compatible flags. Local engine only.') |
    Out-File -LiteralPath $info -Encoding utf8
  Write-Host 'Building engine on this PC. This can take several minutes...'
  Write-Host ('Logs: ' + $base + '.*.log')
  $process = Start-Process -FilePath $TrtExec -ArgumentList $arguments -NoNewWindow -Wait -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
  if ($process.ExitCode -ne 0) { throw ('trtexec failed with exit code ' + $process.ExitCode + '. Read stdout/stderr logs.') }
  if (-not (Test-Path -LiteralPath $temp -PathType Leaf) -or (Get-Item -LiteralPath $temp).Length -le 0) { throw 'trtexec did not produce a nonempty engine.' }
  if (Test-Path -LiteralPath $Engine) { throw 'Output appeared during build; not overwriting it.' }
  Move-Item -LiteralPath $temp -Destination $Engine -ErrorAction Stop
  Write-Host ('Saved: ' + $Engine)
  Write-Host 'Build success is not application/model validation. Load the new engine in the diagnostic app and check its logs.'
} finally {
  $env:PATH = $oldPath
  if (Test-Path -LiteralPath $temp -PathType Leaf) { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue }
}
