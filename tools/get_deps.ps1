<#
  Fetch the build dependencies into $env:MTN_HOME (default $HOME\music-native), outside OneDrive.
    powershell -ExecutionPolicy Bypass -File tools\get_deps.ps1
  - export-venv: Python 3.11 + torch (CPU) + demucs + beat_this + openvino, used only to export the models
  - third_party: the OpenVINO C++ SDK (taken from the openvino wheel: headers, import libs, DLLs) + header-only libs
#>
$ErrorActionPreference = 'Stop'
$Home_ = if ($env:MTN_HOME) { $env:MTN_HOME } else { Join-Path $HOME 'music-native' }
$TP = Join-Path $Home_ 'third_party'
$Venv = Join-Path $Home_ 'export-venv'
New-Item -ItemType Directory -Force $TP, (Join-Path $Home_ 'models') | Out-Null
$OvVer = '2026.4.1'

if (-not (Test-Path "$Venv\Scripts\python.exe")) {
    uv venv --python 3.11 $Venv
}
uv pip install --python "$Venv\Scripts\python.exe" torch==2.8.0 torchaudio==2.8.0 demucs==4.1.0 `
    "beat_this @ git+https://github.com/CPJKU/beat_this@b95c8ab0c58c2d9fcfd40508ae8dffbc05ac4f5c" `
    "openvino==$OvVer" soundfile librosa==0.11.0 `
    --extra-index-url https://download.pytorch.org/whl/cpu --index-strategy unsafe-best-match
if ($LASTEXITCODE -ne 0) { throw 'pip install failed' }

$Ov = Join-Path $Venv 'Lib\site-packages\openvino'
New-Item -ItemType Directory -Force "$TP\openvino" | Out-Null
Copy-Item -Recurse -Force "$Ov\include", "$Ov\libs" "$TP\openvino\"

$H = @{
    'pocketfft_hdronly.hpp' = 'https://raw.githubusercontent.com/mreineck/pocketfft/cpp/pocketfft_hdronly.h'
    'json.hpp'              = 'https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp'
}
foreach ($k in $H.Keys) {
    if (-not (Test-Path "$TP\$k")) { Invoke-WebRequest $H[$k] -OutFile "$TP\$k" }
}
Write-Host "deps ready in $Home_. Next: $Venv\Scripts\python tools\export_models.py $Home_\models ; build.bat"
