<#
  Assemble the self-contained app folder ($MTN_HOME\app) that the installer ships. Nothing in it relies on what is
  installed on the build machine: Python, Node, headless Chrome, ffmpeg and the C++ runtime all live inside it.

    powershell -ExecutionPolicy Bypass -File installer\assemble_app.ps1 [-ToolRepo <Music Tool checkout>] [-Fresh]

  app\Sora.exe, ui\              the window (build.bat gui)
  app\engine\                    mtn.exe + OpenVINO + ONNX Runtime/DirectML + models + ffmpeg.exe + VC++ runtime
  app\python\                    Python 3.11 embeddable + librosa/numpy/scipy/... in Lib\site-packages
  app\node\                      Node 24 (node.exe, npm, npx) for the renderer
  app\tool\                      the Python Music Tool's helpers + renderer (with node_modules + headless Chrome)
  app\pipeline\mt_pipeline.py    song.json -> videos/lyrics/audio, tagged, in the user's folder
#>
param(
    [string]$ToolRepo = "$HOME\music-tool",
    [switch]$Fresh
)
$ErrorActionPreference = 'Stop'
$Src = Split-Path -Parent $PSScriptRoot
$MtnHome = if ($env:MTN_HOME) { $env:MTN_HOME } else { Join-Path $HOME 'music-native' }
$App = Join-Path $MtnHome 'app'
$Dl = Join-Path $MtnHome 'downloads'
$Build = Join-Path $MtnHome 'build'
$Models = Join-Path $MtnHome 'models'
New-Item -ItemType Directory -Force $App, $Dl | Out-Null
function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }

# ---------------------------------------------------------------- engine
Step 'engine (mtn + OpenVINO + models + ffmpeg + VC++ runtime)'
$Eng = Join-Path $App 'engine'
New-Item -ItemType Directory -Force "$Eng\models" | Out-Null
foreach ($f in 'mtn.exe', 'openvino.dll', 'openvino_intel_cpu_plugin.dll', 'openvino_intel_gpu_plugin.dll',
               'openvino_ir_frontend.dll', 'openvino_auto_plugin.dll', 'tbb12.dll', 'tbbmalloc.dll', 'tbbbind_2_5.dll',
               'onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'DirectML.dll') {
    Copy-Item -Force (Join-Path $Build $f) $Eng
}
# OpenVINO set, for the CPU and Intel GPUs (Demucs fp32 also runs on the iGPU; the fp32 aligner IR is left out: not
# faster on these iGPUs, 1.3 GB)
foreach ($n in 'htdemucs_core', 'w2v_large_int8', 'beat_this') {
    foreach ($e in 'xml', 'bin', 'json') { Copy-Item -Force "$Models\$n.$e" "$Eng\models\" }
}
# DirectML set, for discrete GPUs (AMD / NVIDIA / Intel Arc): Demucs + Beat This! fp32, the aligner fp16
foreach ($f in 'htdemucs_core.onnx', 'beat_this.onnx', 'w2v_large_fp16.onnx', 'w2v_large_fp16.json') {
    Copy-Item -Force "$Models\$f" "$Eng\models\"
}
$ff = (Get-Command ffmpeg -ErrorAction Stop).Source
$ffReal = (Get-Item $ff).Target; if (-not $ffReal) { $ffReal = $ff }
Copy-Item -Force $ffReal "$Eng\ffmpeg.exe"
# app-local VC++ runtime: mtn and OpenVINO are /MD, mtn uses OpenMP (vcomp140)
$Redist = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC' -Directory |
          Where-Object { Test-Path "$($_.FullName)\x64" } | Sort-Object Name -Descending | Select-Object -First 1
$Crt = Get-ChildItem "$($Redist.FullName)\x64" -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
$Omp = Get-ChildItem "$($Redist.FullName)\x64" -Directory -Filter 'Microsoft.VC*.OpenMP' | Select-Object -First 1
Copy-Item -Force "$($Crt.FullName)\*.dll" $Eng
Copy-Item -Force "$($Omp.FullName)\*.dll" $Eng

# ---------------------------------------------------------------- python
Step 'Python 3.11 (embeddable) + packages'
$Py = Join-Path $App 'python'
if ($Fresh -and (Test-Path $Py)) { Remove-Item -Recurse -Force $Py }
if (-not (Test-Path "$Py\python.exe")) {
    $zip = "$Dl\python-3.11.9-embed-amd64.zip"
    if (-not (Test-Path $zip)) { Invoke-WebRequest 'https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip' -OutFile $zip }
    Expand-Archive -Force $zip $Py
    # the embeddable build ignores site-packages unless its ._pth says so
    Set-Content -Encoding ascii "$Py\python311._pth" "python311.zip`r`n.`r`nLib\site-packages`r`nimport site`r`n"
}
$Site = "$Py\Lib\site-packages"
New-Item -ItemType Directory -Force $Site | Out-Null
# same versions as the Python tool's requirements.txt; only what make.py / build_edit / pick_short / qa_edit import
uv pip install --python 3.11 --target $Site --quiet `
    numpy==2.4.6 scipy==1.17.1 librosa==0.11.0 soundfile==0.14.0 pyyaml==6.0.3 requests==2.34.2
if ($LASTEXITCODE -ne 0) { throw 'pip install failed' }

# ---------------------------------------------------------------- node
Step 'Node 24'
$Node = Join-Path $App 'node'
$NodeSrc = if ($env:MTN_NODE) { $env:MTN_NODE } else { Join-Path $env:LOCALAPPDATA 'nvm\v24.21.0' }  # a Node 24 install
New-Item -ItemType Directory -Force $Node | Out-Null
foreach ($f in 'node.exe', 'npm.cmd', 'npx.cmd', 'npm', 'npx') { if (Test-Path "$NodeSrc\$f") { Copy-Item -Force "$NodeSrc\$f" $Node } }
if (-not (Test-Path "$Node\node_modules\npm")) { Copy-Item -Recurse -Force "$NodeSrc\node_modules\npm" "$Node\node_modules\npm" }

# ---------------------------------------------------------------- the Python tool's helpers + renderer
Step "tool (helpers + renderer from $ToolRepo)"
$Tool = Join-Path $App 'tool'
New-Item -ItemType Directory -Force "$Tool\tools", "$Tool\analysis", "$Tool\renderer\public\edits", "$Tool\renderer\public\audio", "$Tool\output" | Out-Null
foreach ($f in 'make.py', 'build_edit.py', 'pick_short.py', 'qa_edit.py', 'test_layout.py') { Copy-Item -Force "$ToolRepo\tools\$f" "$Tool\tools\" }
Copy-Item -Force "$ToolRepo\analysis\timing_recipe.json" "$Tool\analysis\"
foreach ($f in 'package.json', 'package-lock.json', 'remotion.config.ts', 'tsconfig.json') { Copy-Item -Force "$ToolRepo\renderer\$f" "$Tool\renderer\" }
Copy-Item -Recurse -Force "$ToolRepo\renderer\src" "$Tool\renderer\"
Copy-Item -Recurse -Force "$ToolRepo\renderer\public\fonts" "$Tool\renderer\public\"
Set-Content -Encoding ascii "$Tool\VERSION.txt" ("music-tool " + (git -C $ToolRepo rev-parse --short HEAD) + " (+ local changes: " + ((git -C $ToolRepo status --short) -join ', ') + ")")
if (-not (Test-Path "$Tool\renderer\node_modules\@remotion\cli")) {
    # reuse the dev checkout's install (incl. Remotion's headless Chrome), then drop dev-only packages
    robocopy "$ToolRepo\renderer\node_modules" "$Tool\renderer\node_modules" /E /NFL /NDL /NJH /NJS /NP | Out-Null
    Push-Location "$Tool\renderer"
    $env:PATH = "$Node;$env:PATH"
    # packages the renderer's package.json lists but its source never imports: half the size, and butterchurn-presets
    # alone has 220-character file names that break Windows' 260-character path limit once installed
    & "$Node\npm.cmd" uninstall --no-audit --no-fund butterchurn butterchurn-presets gl-transitions lottie-web `
        @remotion/lottie animejs gsap @vfx-js/core postprocessing @react-three/postprocessing @react-three/drei `
        @remotion/transitions @remotion/captions @remotion/google-fonts
    & "$Node\npm.cmd" prune --omit=dev --no-audit --no-fund  # (uninstall re-adds dev packages)
    & "$Node\npx.cmd" remotion browser ensure
    Pop-Location
}

# ---------------------------------------------------------------- pipeline + GUI
Step 'pipeline + window'
New-Item -ItemType Directory -Force "$App\pipeline", "$App\ui" | Out-Null
Copy-Item -Force "$Src\app\pipeline\mt_pipeline.py" "$App\pipeline\"
Copy-Item -Force "$Build\Sora.exe" $App
if (Test-Path "$App\MusicTool.exe") { Remove-Item -Force "$App\MusicTool.exe" }  # the app's name before Sora
Copy-Item -Recurse -Force "$Src\app\ui\*" "$App\ui\"
Copy-Item -Force "$Src\README.md" $App

$size = (Get-ChildItem -Recurse $App | Measure-Object -Property Length -Sum).Sum / 1GB
Write-Host ("app assembled: {0} ({1:N2} GB)" -f $App, $size) -ForegroundColor Green
