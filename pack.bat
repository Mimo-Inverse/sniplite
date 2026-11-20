@echo off
rem The three files of a release, into build\dist. Run build.bat first.
rem
rem   sniplite_win_setup_amd64.exe                    the installer. Its name never changes: installed
rem                                                   copies ask for it by name when they update
rem   sniplite_<version>_portable_x64.exe             sniplite.exe by itself: nothing to install, it runs
rem                                                   from wherever it is put
rem   sniplite_<version>_portable_with_OCR_x64.exe    the same program carrying the text recognition
rem                                                   module: the module's zip is put after it, and it
rem                                                   unpacks that beside itself when text is first
rem                                                   read (src\ocr_get.c)
rem
rem The module is zipped here, with the tar that comes with Windows, out of build\ocr: onnxruntime.dll,
rem the two models, the four files of the Visual C++ runtime that DLL needs and the licenses of its
rem parts, with res\ocr-notice.txt as NOTICE.txt.
rem
rem   build\sniplite_ocr.zip            the whole module: what Settings downloads and what the program
rem                                     with OCR carries
rem   build\sniplite_ocr_runtime.zip    the runtime files and the notice: what completes a module from
rem                                     before they came with it
rem
rem This project publishes both once, under a tag of their own, which src\ocr_get.c names with the
rem size of each: the zips made here must have those sizes, so the program and what is published
rem cannot drift apart. In both, msvcp140.dll is the last of the runtime files: src\ocr_get.c says why.
rem
rem A changed module is published under a new tag, with its hashes in src\ocr_get.c.
setlocal
cd /d "%~dp0"
if not exist build\sniplite.exe (echo build\sniplite.exe is missing: run build.bat first & exit /b 1)
if not exist build\sniplite_win_setup_amd64.exe (echo the installer is missing: run build.bat first & exit /b 1)
for /f "tokens=3" %%v in ('findstr /C:"#define SNIPLITE_VERSION" src\common.h') do set V=%%v
set V=%V:L"=%
set V=%V:"=%
set RUNTIME=ocr/vcruntime140.dll ocr/vcruntime140_1.dll ocr/msvcp140_1.dll ocr/msvcp140.dll
set MODULE=ocr/PP-OCRv6_det_tiny.onnx ocr/PP-OCRv6_rec_tiny.onnx ocr/onnxruntime.dll %RUNTIME%
set LICENSES=ocr/LICENSE-onnxruntime.txt ocr/LICENSE-PaddleOCR.txt ocr/ThirdPartyNotices-onnxruntime.txt
set ZIP=build\sniplite_ocr.zip
set RUNTIME_ZIP=build\sniplite_ocr_runtime.zip
set TAR=%SystemRoot%\System32\tar.exe
set D=build\dist
if exist %D% rmdir /s /q %D%
mkdir %D% || exit /b 1

for %%f in (%MODULE% %LICENSES%) do if not exist build\ocr\%%~nxf (
  echo build\ocr\%%~nxf is missing: the module's files belong in build\ocr
  exit /b 1
)
rem through more, the notice's lines end with CR LF whatever the checkout made of them: its size counts
"%SystemRoot%\System32\more.com" < res\ocr-notice.txt > build\ocr\NOTICE.txt || exit /b 1
"%TAR%" -a -c -f %ZIP% -C build %MODULE% ocr/NOTICE.txt %LICENSES% || exit /b 1
"%TAR%" -a -c -f %RUNTIME_ZIP% -C build %RUNTIME% ocr/NOTICE.txt || exit /b 1
call :sized %ZIP% MODULE_BYTES || exit /b 1
call :sized %RUNTIME_ZIP% RUNTIME_BYTES || exit /b 1

copy /y build\sniplite_win_setup_amd64.exe %D% >nul || exit /b 1
copy /y build\sniplite.exe %D%\sniplite_%V%_portable_x64.exe >nul || exit /b 1
copy /b /y build\sniplite.exe + %ZIP% %D%\sniplite_%V%_portable_with_OCR_x64.exe >nul || exit /b 1

for %%f in (%D%\* %ZIP% %RUNTIME_ZIP%) do echo %%~nxf  %%~zf bytes
exit /b 0

rem %1 has the size src\ocr_get.c names as %2, or the module is not the published one
:sized
for /f "tokens=3" %%v in ('findstr /C:"#define %2" src\ocr_get.c') do set BYTES=%%v
set BYTES=%BYTES:u=%
for %%f in (%1) do if not "%%~zf"=="%BYTES%" (
  echo %1 is %%~zf bytes, src\ocr_get.c names %BYTES% as %2
  exit /b 1
)
exit /b 0
