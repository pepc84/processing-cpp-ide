@echo off
setlocal EnableDelayedExpansion
:: =============================================================================
:: cpp-dev -- Windows Setup
:: Double-click this file. It installs MSYS2 if needed, then builds cpp-dev.exe.
:: =============================================================================

echo.
echo  +============================================+
echo  ^|   cpp-dev -- Windows Setup         ^|
echo  +============================================+
echo.

:: ---------------------------------------------------------------------------
:: 1. Find or install MSYS2
:: ---------------------------------------------------------------------------
set MSYS2=
for %%P in (
    "C:\msys64"
    "C:\msys2"
    "C:\tools\msys64"
    "%LOCALAPPDATA%\msys64"
    "%USERPROFILE%\msys64"
) do (
    if exist "%%~P\usr\bin\bash.exe" (
        if "!MSYS2!"=="" set "MSYS2=%%~P"
    )
)

if "!MSYS2!" NEQ "" goto :have_msys2

:: --- MSYS2 not found -- download and install it ----------------------------
echo [INFO] MSYS2 not found. Downloading installer (~90 MB)...
echo.
set "INST=%TEMP%\msys2-installer.exe"
powershell -NoProfile -Command ^
    "$p='SilentlyContinue';$ProgressPreference=$p; [Net.ServicePointManager]::SecurityProtocol='Tls12'; Invoke-WebRequest -Uri 'https://github.com/msys2/msys2-installer/releases/download/2024-01-13/msys2-x86_64-20240113.exe' -OutFile '%INST%'"

if not exist "%INST%" (
    echo [ERR] Download failed. Check your connection or install MSYS2 manually from https://www.msys2.org/
    pause & exit /b 1
)

echo [INFO] Installing MSYS2 to C:\msys64 ...
"%INST%" install --root C:\msys64 --confirm-command --accept-messages
if not exist "C:\msys64\usr\bin\bash.exe" (
    :: Try legacy NSIS installer flag
    "%INST%" /S /D=C:\msys64
)
del /f /q "%INST%" 2>nul

if exist "C:\msys64\usr\bin\bash.exe" (
    set "MSYS2=C:\msys64"
    echo [OK]  MSYS2 installed at C:\msys64
    goto :have_msys2
)
echo [ERR] MSYS2 installation failed. Install manually from https://www.msys2.org/
pause & exit /b 1

:have_msys2
echo [OK]  MSYS2: !MSYS2!
set "BASH=!MSYS2!\usr\bin\bash.exe"
set "MINGW_BIN=!MSYS2!\mingw64\bin"
set "PATH=!MINGW_BIN!;!MSYS2!\usr\bin;%PATH%"

:: ---------------------------------------------------------------------------
:: 2. Update pacman db once, then install all build deps
:: ---------------------------------------------------------------------------
echo.
echo [INFO] Updating package database...
:: Remove stale lock file if present (safe when no other pacman is running)
if exist "!MSYS2!ar\lib\pacman\db.lck" (
    echo [INFO] Removing stale pacman lock...
    del /f /q "!MSYS2!ar\lib\pacman\db.lck" 2>nul
)
"!BASH!" -lc "pacman -Sy --noconfirm 2>&1 | tail -5"

echo [INFO] Installing build tools and GL libraries...
"!BASH!" -lc "pacman -S --needed --noconfirm mingw-w64-x86_64-gcc mingw-w64-x86_64-glfw mingw-w64-x86_64-glew mingw-w64-x86_64-freeglut mingw-w64-x86_64-make mingw-w64-x86_64-pkg-config 2>&1 | grep -v warning"
echo [OK]  Build tools ready.

:: ---------------------------------------------------------------------------
:: 3. Download stb headers
:: ---------------------------------------------------------------------------
echo.
echo [INFO] Downloading stb headers...
if not exist "src" mkdir src

:: Try multiple download methods: MSYS2 curl, system curl, then PowerShell
if not exist "src\stb_truetype.h" (
    echo [INFO] Downloading stb_truetype.h...
    "!BASH!" -lc "curl -fsSL https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h -o src/stb_truetype.h 2>/dev/null" 2>nul
    if not exist "src\stb_truetype.h" (
        curl -fsSL "https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h" -o "src\stb_truetype.h" 2>nul
    )
    if not exist "src\stb_truetype.h" (
        powershell -NoProfile -ExecutionPolicy Bypass -Command ^
            "[Net.ServicePointManager]::SecurityProtocol='Tls12,Tls13';" ^
            "Invoke-WebRequest 'https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h' -OutFile 'src\stb_truetype.h'" 2>nul
    )
    if exist "src\stb_truetype.h" ( echo [OK]  stb_truetype.h ) else ( echo [WARN] stb_truetype.h download failed -- add it to src\ manually )
) else ( echo [OK]  stb_truetype.h already present )

if not exist "src\stb_image.h" (
    echo [INFO] Downloading stb_image.h...
    "!BASH!" -lc "curl -fsSL https://raw.githubusercontent.com/nothings/stb/master/stb_image.h -o src/stb_image.h 2>/dev/null" 2>nul
    if not exist "src\stb_image.h" (
        curl -fsSL "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" -o "src\stb_image.h" 2>nul
    )
    if not exist "src\stb_image.h" (
        powershell -NoProfile -ExecutionPolicy Bypass -Command ^
            "[Net.ServicePointManager]::SecurityProtocol='Tls12,Tls13';" ^
            "Invoke-WebRequest 'https://raw.githubusercontent.com/nothings/stb/master/stb_image.h' -OutFile 'src\stb_image.h'" 2>nul
    )
    if exist "src\stb_image.h" ( echo [OK]  stb_image.h ) else ( echo [WARN] stb_image.h download failed -- add it to src\ manually )
) else ( echo [OK]  stb_image.h already present )

:: ---------------------------------------------------------------------------
:: 4. Copy a font
:: ---------------------------------------------------------------------------
if not exist "default.ttf" (
    set "FONT="
    for %%F in (
        "%WINDIR%\Fonts\consola.ttf"
        "%WINDIR%\Fonts\cour.ttf"
        "%WINDIR%\Fonts\arial.ttf"
        "!MSYS2!\mingw64\share\fonts\DejaVuSansMono.ttf"
    ) do (
        if exist "%%~F" if "!FONT!"=="" set "FONT=%%~F"
    )
    if "!FONT!" NEQ "" (
        copy "!FONT!" default.ttf >nul
        echo [OK]  Font copied: !FONT!
    ) else (
        echo [WARN] No font found. Place any .ttf here as default.ttf
    )
) else ( echo [OK]  default.ttf already present )

:: ---------------------------------------------------------------------------
:: 5. Write src\main.cpp
:: ---------------------------------------------------------------------------
if not exist "src\main.cpp" (
    (
        echo #include "Processing.h"
        echo #include ^<string^>
        echo int main^(int argc, char** argv^) {
        echo     for ^(int i = 1; i ^< argc; i++^)
        echo         if ^(std::string^(argv[i]^) == "--debug"^)
        echo             Processing::enableDebugConsole^(^);
        echo     Processing::run^(^);
        echo     return 0;
        echo }
    ) > src\main.cpp
    echo [OK]  src\main.cpp written
) else ( echo [OK]  src\main.cpp already present )

:: ---------------------------------------------------------------------------
:: 6. Copy / download source files into src\
:: ---------------------------------------------------------------------------
echo.
echo [INFO] Checking and copying source files...
if not exist "src" mkdir src

:: First: copy any source files found next to setup.bat into src\
for %%F in (Processing.h Processing.cpp Platform.h IDE.cpp Processing_defaults.cpp) do (
    if not exist "src\%%F" if exist "%%F" (
        copy "%%F" "src\%%F" >nul
        echo [OK]  Copied %%F into src\
    )
)

:: Check which required files are still missing
set MISSING=0
for %%F in (src\Processing.h src\Processing.cpp src\Platform.h src\IDE.cpp) do (
    if not exist "%%F" set /a MISSING+=1
)

:: If files are missing, print a clear error explaining what to do
if !MISSING! GTR 0 (
    echo.
    echo [ERR] Source files are missing from the src\ folder.
    echo.
    echo   Required files:
    for %%F in (src\Processing.h src\Processing.cpp src\Platform.h src\IDE.cpp src\Processing_defaults.cpp) do (
        if not exist "%%F" ( echo     MISSING: %%F ) else ( echo     OK:      %%F )
    )
    echo.
    echo   How to fix:
    echo     1. Place Processing.h, Processing.cpp, Platform.h, IDE.cpp
    echo        and Processing_defaults.cpp next to setup.bat
    echo     2. Double-click setup.bat again
    echo.
    pause & exit /b 1
)

:: All files present -- confirm
for %%F in (src\Processing.h src\Processing.cpp src\Platform.h src\IDE.cpp src\Processing_defaults.cpp) do (
    echo [OK]  Found: %%F
)

:: ---------------------------------------------------------------------------
:: 7. Create project folder structure (files\ and lib\)
:: ---------------------------------------------------------------------------
echo.
echo [INFO] Setting up project folders...
if not exist "files" mkdir files
if not exist "lib"   mkdir lib

:: Move logo.jpg into files\ if it is in the root
if exist "logo.jpg" if not exist "files\logo.jpg" (
    copy "logo.jpg" "files\logo.jpg" >nul
    echo [OK]  logo.jpg -^> files\logo.jpg
)
if exist "files\logo.jpg" ( echo [OK]  files\logo.jpg present )

:: Copy sample sketches into files\ if they exist next to setup.bat
for %%S in (Geometry.cpp Mixture.cpp Mandelbrot.cpp StoringInput.cpp) do (
    if exist "%%S" if not exist "files\%%S" (
        copy "%%S" "files\%%S" >nul
        echo [OK]  Sample: %%S -^> files\
    )
)


echo [OK]  Project folders ready ^(files\ lib\^)

:: Create examples\ folder and populate it
if not exist "examples" mkdir examples

for %%S in (Geometry.cpp Mixture.cpp Mandelbrot.cpp StoringInput.cpp) do (
    if exist "%%S" if not exist "examples\%%S" (
        copy "%%S" "examples\%%S" >nul
        echo [OK]  Example: %%S -^> examples\
    )
    if exist "examples\%%S" if not exist "%%S" (
        echo [OK]  examples\%%S present
    )
)
echo [OK]  examples\ folder ready

:: ---------------------------------------------------------------------------
:: 11. Write build scripts
:: ---------------------------------------------------------------------------
echo.
echo [INFO] Writing build scripts...

(
    echo @echo off
    echo echo [build] Compiling IDE...
    echo "!MINGW_BIN!\g++.exe" -std=c++17 ^
        src\Processing.cpp ^
        src\IDE.cpp ^
        src\Processing_defaults.cpp ^
        src\main.cpp ^
        -o cpp-dev.exe ^
        -lglfw3 -lglew32 -lopengl32 -lglu32 ^
        -lcomdlg32 -lshell32 -lole32 -luuid ^
        -pthread -O2 ^
        -D_USE_MATH_DEFINES ^
        -mwindows
    echo if %%ERRORLEVEL%% neq 0 ^( echo [ERR] Build failed. ^& pause ^& exit /b 1 ^)
    echo echo [build] Done: cpp-dev.exe
) > buildIDE.bat
echo [OK]  buildIDE.bat

(
    echo @echo off
    echo set SKETCH=%%1
    echo if "%%SKETCH%%"=="" set SKETCH=src\MySketch.cpp
    echo set OUT=%%2
    echo if "%%OUT%%"=="" set OUT=SketchApp
    echo echo [build] %%SKETCH%% -^> %%OUT%%.exe
    echo "!MINGW_BIN!\g++.exe" -std=c++17 ^
        src\Processing.cpp ^
        "%%SKETCH%%" ^
        src\Processing_defaults.cpp ^
        src\main.cpp ^
        -o "%%OUT%%.exe" ^
        -lglfw3 -lglew32 -lopengl32 -lglu32 ^
        -mwindows -pthread -O2 ^
        -D_USE_MATH_DEFINES
    echo if %%ERRORLEVEL%% neq 0 ^( echo [ERR] Build failed. ^& pause ^& exit /b 1 ^)
    echo echo [build] Done: %%OUT%%.exe
) > build.bat
echo [OK]  build.bat

:: Windows: .bat files only (no .sh generated here)

:: ---------------------------------------------------------------------------
:: 11. Build the IDE
:: ---------------------------------------------------------------------------
echo.
echo [INFO] Building IDE...
"!MINGW_BIN!\g++.exe" -std=c++17 ^
    src\Processing.cpp ^
    src\IDE.cpp ^
    src\Processing_defaults.cpp ^
    src\main.cpp ^
    -o cpp-dev.exe ^
    -lglfw3 -lglew32 -lopengl32 -lglu32 ^
    -lcomdlg32 -lshell32 -lole32 -luuid ^
    -mwindows -pthread -O2 ^
    -D_USE_MATH_DEFINES

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERR] Build failed -- see errors above.
    pause & exit /b 1
)
echo [OK]  cpp-dev.exe built

:: ---------------------------------------------------------------------------
:: 11. Collect DLLs
:: ---------------------------------------------------------------------------
echo.
echo [INFO] Collecting runtime DLLs...
for %%D in (
    libglfw3.dll glfw3.dll
    glew32.dll libglew32.dll
    libgcc_s_seh-1.dll
    libstdc++-6.dll
    libwinpthread-1.dll
) do (
    if exist "!MINGW_BIN!\%%D" (
        if not exist "%%D" (
            copy "!MINGW_BIN!\%%D" . >nul
            echo [OK]  %%D
        )
    )
)

:: Auto-collect any additional DLLs the binary links against (via bash/objdump)
"!BASH!" -lc "cd '$(cygpath -u "%CD%")' && objdump -p cpp-dev.exe 2>/dev/null | grep 'DLL Name' | awk '{print $3}' | while read dll; do [ -f /mingw64/bin/$dll ] && [ ! -f ./$dll ] && cp /mingw64/bin/$dll . && echo "[OK]  $dll (auto)"; done"

:: ---------------------------------------------------------------------------
:: 11. Done
:: ---------------------------------------------------------------------------
echo.
echo  +============================================+
echo  ^|   Setup complete!                         ^|
echo  +============================================+
echo.
echo   Run IDE:       double-click cpp-dev.exe
echo   Build sketch:  build.bat MySketch.cpp
echo   Rebuild IDE:   buildIDE.bat
echo.
pause
:: Launch IDE
:: Run "cpp-dev.exe --debug" from a terminal to see error output
start "cpp-dev IDE" cpp-dev.exe
