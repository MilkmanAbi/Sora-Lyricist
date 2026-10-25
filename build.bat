@echo off
rem Build mtn.exe (the native analysis engine) into %MTN_HOME%\build\ and stage the OpenVINO DLLs next to it.
rem The source lives here (Desktop, OneDrive-synced); dependencies, models, build output and the chunk cache live in
rem %MTN_HOME% (default %USERPROFILE%\music-native) so OneDrive never syncs gigabytes of binaries.
rem   build.bat          release build
rem   build.bat debug    debug symbols, no optimisation
rem   build.bat dev      release build as mtn_dev.exe (when mtn.exe is busy, e.g. a benchmark is running)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if "%MTN_HOME%"=="" set MTN_HOME=%USERPROFILE%\music-native
set TP=%MTN_HOME%\third_party
set OUT=%MTN_HOME%\build
if not exist "%TP%\openvino\include\openvino\openvino.hpp" (echo missing %TP%\openvino - run tools\get_deps.ps1 & exit /b 1)
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
if /i "%1"=="gui" goto gui
set OPT=/O2 /Oi /GL /DNDEBUG
set LOPT=/LTCG /OPT:REF /OPT:ICF
if /i "%1"=="debug" (set OPT=/Od /Zi & set LOPT=/DEBUG)
set EXE=mtn.exe
if /i "%1"=="dev" set EXE=mtn_dev.exe
rem /MD: OpenVINO is built against the DLL runtime. /openmp: STFT frames and mel bands run on all cores.
rem /arch:AVX2 is the floor for the target machines (any PC from the last ~10 years).
rem ONNX Runtime + DirectML: networks on discrete GPUs (AMD / NVIDIA / Intel Arc). Delay-loaded: only runs that use a
rem DirectML device load it.
set ORT=%TP%\ort\ortpkg
set DML=%TP%\ort\dmlpkg\bin\x64-win
cl /nologo /utf-8 /std:c++20 /EHsc /MD /W3 /openmp /arch:AVX2 /fp:fast %OPT% /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING ^
   /I"%TP%" /I"%TP%\openvino\include" /I"%ORT%\build\native\include" /Fo"%OUT%\obj\\" ^
   src\main.cpp src\common.cpp src\cache.cpp src\audio.cpp src\dsp.cpp src\infer.cpp src\separate.cpp ^
   src\beats.cpp src\align.cpp src\lyrics.cpp src\lyrics_web.cpp src\http.cpp src\gpu.cpp ^
   /Fe:"%OUT%\%EXE%" /link %LOPT% /LIBPATH:"%TP%\openvino\libs" openvino.lib ^
   "%ORT%\runtimes\win-x64\native\onnxruntime.lib" delayimp.lib /DELAYLOAD:onnxruntime.dll /DELAYLOAD:d3d12.dll ^
   /DELAYLOAD:dxgi.dll || exit /b 1
rem runtime DLLs: OpenVINO core, CPU + GPU plugins, IR reader, TBB; ONNX Runtime (DirectML build) + DirectML
for %%d in (openvino.dll openvino_intel_cpu_plugin.dll openvino_intel_gpu_plugin.dll openvino_ir_frontend.dll ^
            openvino_auto_plugin.dll tbb12.dll tbbmalloc.dll tbbbind_2_5.dll) do (
  if not exist "%OUT%\%%d" copy /y "%TP%\openvino\libs\%%d" "%OUT%\" >nul
)
for %%d in (onnxruntime.dll onnxruntime_providers_shared.dll) do (
  if not exist "%OUT%\%%d" copy /y "%ORT%\runtimes\win-x64\native\%%d" "%OUT%\" >nul
)
if not exist "%OUT%\DirectML.dll" copy /y "%DML%\DirectML.dll" "%OUT%\" >nul
if not exist "%OUT%\models" mklink /J "%OUT%\models" "%MTN_HOME%\models" >nul
echo built %OUT%\%EXE%
exit /b 0

:gui
rem Sora.exe, the window: static CRT (/MT) + static WebView2 loader, so it needs no DLLs of its own
set WV=%TP%\webview2\pkg\build\native
if not exist "%OUT%\gui" mkdir "%OUT%\gui"
pushd app\gui
rc /nologo /fo "%OUT%\gui\sora.res" sora.rc || (popd & exit /b 1)
popd
cl /nologo /utf-8 /std:c++20 /EHsc /MT /W3 /O2 /DNDEBUG /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /I"%TP%" /I"%WV%\include" /Fo"%OUT%\gui\\" app\gui\main.cpp "%OUT%\gui\sora.res" ^
   /Fe:"%OUT%\Sora.exe" /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF "%WV%\x64\WebView2LoaderStatic.lib" || exit /b 1
echo built %OUT%\Sora.exe
