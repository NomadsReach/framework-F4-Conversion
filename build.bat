@echo off
setlocal enabledelayedexpansion

cls
color 0A
echo.
echo ============================================================================
echo  PrismaUI_F4 - Build
echo ============================================================================
echo.

set "SCRIPT_DIR=%~dp0"
set "BUILD_LOG=%SCRIPT_DIR%build.log"

if exist "%BUILD_LOG%" del "%BUILD_LOG%"

echo [%date% %time%] === BUILD SESSION STARTED === >> "%BUILD_LOG%"

where xmake >nul 2>&1
if errorlevel 1 (
    color 0C
    echo ERROR: xmake not found
    color 0A
    echo.
    pause
    exit /b 1
)

if not exist "%SCRIPT_DIR%xmake.lua" (
    color 0C
    echo ERROR: xmake.lua not found
    color 0A
    echo.
    pause
    exit /b 1
)

REM ============================================================================
REM  RUNTIME SELECTION
REM ============================================================================

:SELECT_RUNTIME
cls
color 0A
echo.
echo Available runtimes:
echo   1 - Fallout 4 Original
echo   2 - Fallout 4 Next Gen
echo   3 - Fallout 4 VR
echo.
set /p "RUNTIME=Enter 1, 2, or 3: "

if "%RUNTIME%"=="1" (
    set "PRISMA_TARGET=og"
) else if "%RUNTIME%"=="2" (
    set "PRISMA_TARGET=ng"
) else if "%RUNTIME%"=="3" (
    set "PRISMA_TARGET=vr"
) else (
    echo Invalid choice.
    timeout /t 2 /nobreak
    goto SELECT_RUNTIME
)

if /i "%PRISMA_TARGET%"=="vr" (
    if not defined F4VR_COMMON_FRAMEWORK_PATH (
        echo.
        echo Enter the ArthurHub F4VR-CommonFramework checkout path:
        set /p "F4VR_COMMON_FRAMEWORK_PATH="
    )
    if not exist "!F4VR_COMMON_FRAMEWORK_PATH!\external\openvr\openvr.h" (
        color 0C
        echo ERROR: ArthurHub CommonFramework OpenVR headers were not found.
        color 0A
        pause
        exit /b 1
    )
    if not exist "!F4VR_COMMON_FRAMEWORK_PATH!\external\openvr\openvr_api.lib" (
        color 0C
        echo ERROR: ArthurHub CommonFramework OpenVR library was not found.
        color 0A
        pause
        exit /b 1
    )
)

REM ============================================================================
REM  PROJECT SELECTION
REM ============================================================================

:SELECT_PROJECT
cls
color 0A
echo.
echo Available projects:
echo   1 - PrismaUI_F4
echo   2 - PrismaUI-F4-Example-Plugin
echo.
set /p "PROJ=Enter 1 or 2: "

if "%PROJ%"=="1" (
    if /i "%PRISMA_TARGET%"=="vr" (
        set "PROJ_NAME=PrismaUI_F4VR"
        set "PROJ_TARGET=PrismaUI_F4VR"
    ) else (
        set "PROJ_NAME=PrismaUI_F4"
        set "PROJ_TARGET=PrismaUI_F4"
    )
    set "DLL_NAME=PrismaUI_F4.dll"
) else if "%PROJ%"=="2" (
    if /i "%PRISMA_TARGET%"=="vr" (
        set "PROJ_NAME=PrismaUI-F4VR-Example-Plugin"
        set "PROJ_TARGET=PrismaUI-F4VR-Example-Plugin"
    ) else (
        set "PROJ_NAME=PrismaUI-F4-Example-Plugin"
        set "PROJ_TARGET=PrismaUI-F4-Example-Plugin"
    )
    set "DLL_NAME=PrismaUI-F4-Example.dll"
) else (
    echo Invalid choice.
    timeout /t 2 /nobreak
    goto SELECT_PROJECT
)

REM ============================================================================
REM  CLEAN BUILD
REM ============================================================================

cls
color 0A
echo.
echo Clean build? (y/n)
set /p "CLEAN="

if /i "%CLEAN%"=="y" (
    echo Cleaning...
    if exist "%SCRIPT_DIR%build\windows" rmdir /s /q "%SCRIPT_DIR%build\windows" 2>nul
    if exist "%SCRIPT_DIR%build\.gens" rmdir /s /q "%SCRIPT_DIR%build\.gens" 2>nul
)

REM ============================================================================
REM  BUILD
REM ============================================================================

cls
color 0A
echo.
echo Configuring...
cd /d "%SCRIPT_DIR%"
if /i "%PRISMA_TARGET%"=="vr" (
    xmake f --plat=windows --arch=x64 -m release --fallout_f4=n --fallout_f4ng=n --fallout_f4vr=y --f4se_xbyak=y >>"%BUILD_LOG%" 2>&1
) else (
    xmake f --plat=windows --arch=x64 -m release >>"%BUILD_LOG%" 2>&1
)

if errorlevel 1 (
    color 0C
    echo Configuration failed
    color 0A
    pause
    exit /b 1
)

echo Building %PROJ_TARGET%...
xmake build -r -j 2 "%PROJ_TARGET%" >>"%BUILD_LOG%" 2>&1

if errorlevel 1 (
    color 0C
    echo Build failed
    color 0A
    pause
    exit /b 1
)

REM ============================================================================
REM  FIND DLL
REM ============================================================================

set "DLL_PATH="
for /r "%SCRIPT_DIR%build" %%F in ("%DLL_NAME%") do (
    if exist "%%F" set "DLL_PATH=%%F"
)

if not defined DLL_PATH (
    color 0C
    echo DLL not found after build
    color 0A
    pause
    exit /b 1
)

for %%F in ("%DLL_PATH%") do set "DLL_SIZE=%%~zF"

cls
color 0A
echo.
echo ============================================================================
echo  BUILD SUCCESSFUL
echo ============================================================================
echo.
echo DLL: %DLL_NAME%
echo Size: %DLL_SIZE% bytes
echo Found: %DLL_PATH%
echo.

if /i "%PRISMA_TARGET%"=="vr" (
    echo FO4VR deployment is disabled.
    echo The portable distribution was staged under dist\.
    goto POST_BUILD_MENU
)

REM ============================================================================
REM  DEPLOY
REM ============================================================================

echo Deploy? (y/n)
set /p "DEPLOY_YES="

if /i not "%DEPLOY_YES%"=="y" (
    echo Skipped.
    pause
    exit /b 0
)

echo.
echo Deployment type:
echo   1 - DLL only to F4SE\Plugins
echo   2 - Full (DLL + assets + libs + resources + icons^)
echo.
set /p "DEPLOY_TYPE="

if "%DEPLOY_TYPE%"=="1" (
    echo.
    echo Enter F4SE Plugins path:
    set /p "DEPLOY_PATH="
) else if "%DEPLOY_TYPE%"=="2" (
    echo.
    echo Enter base deployment path:
    set /p "DEPLOY_PATH="
) else (
    echo Invalid.
    pause
    exit /b 1
)

if not defined DEPLOY_PATH (
    echo No path entered.
    pause
    exit /b 1
)

REM ============================================================================
REM  COPY FILES
REM ============================================================================

if not exist "%DEPLOY_PATH%" mkdir "%DEPLOY_PATH%"

echo.
echo Copying files...
echo.

if "%DEPLOY_TYPE%"=="1" (
    REM DLL only
    echo Copying DLL...
    copy /Y "%DLL_PATH%" "%DEPLOY_PATH%\%DLL_NAME%" >nul 2>&1

    if exist "%DEPLOY_PATH%\%DLL_NAME%" (
        color 0A
        echo [OK] DLL deployed
        echo Location: %DEPLOY_PATH%
    ) else (
        color 0C
        echo [ERROR] Failed to copy DLL
    )
) else (
    REM Full deployment
    set "MOD_PATH=%DEPLOY_PATH%\PrismaUI_F4"
    if not exist "%MOD_PATH%" mkdir "%MOD_PATH%"

    REM DLL
    echo [1/6] Copying DLL...
    set "DLL_DEST=%MOD_PATH%\F4SE\Plugins"
    if not exist "%DLL_DEST%" mkdir "%DLL_DEST%"
    copy /Y "%DLL_PATH%" "%DLL_DEST%\%DLL_NAME%" >nul 2>&1
    if exist "%DLL_DEST%\%DLL_NAME%" (
        echo   [OK] DLL
    ) else (
        echo   [ERROR] DLL failed
    )

    REM Assets
    echo [2/6] Copying assets...
    if exist "%SCRIPT_DIR%assets" (
        xcopy "%SCRIPT_DIR%assets\*" "%MOD_PATH%\assets" /E /Y /I >nul 2>&1
        echo   [OK] Assets
    ) else (
        echo   [SKIP] No assets
    )

    REM Ultralight libs
    echo [3/6] Copying Ultralight libraries...
    if exist "%SCRIPT_DIR%build\ultralight-1.4.0\bin" (
        if not exist "%MOD_PATH%\libs" mkdir "%MOD_PATH%\libs"
        xcopy "%SCRIPT_DIR%build\ultralight-1.4.0\bin\*.dll" "%MOD_PATH%\libs" /Y >nul 2>&1
        echo   [OK] Libraries
    ) else (
        echo   [SKIP] No libraries
    )

    REM Ultralight resources
    echo [4/6] Copying Ultralight resources...
    if exist "%SCRIPT_DIR%build\ultralight-1.4.0\resources" (
        if not exist "%MOD_PATH%\resources" mkdir "%MOD_PATH%\resources"
        xcopy "%SCRIPT_DIR%build\ultralight-1.4.0\resources\*" "%MOD_PATH%\resources" /E /Y >nul 2>&1
        echo   [OK] Resources
    ) else (
        echo   [SKIP] No resources
    )

    REM Icons
    echo [5/6] Copying icons...
    if exist "%SCRIPT_DIR%Icons" (
        if not exist "%MOD_PATH%\icons" mkdir "%MOD_PATH%\icons"
        xcopy "%SCRIPT_DIR%Icons\*" "%MOD_PATH%\icons" /E /Y >nul 2>&1
        echo   [OK] Icons
    ) else (
        echo   [SKIP] No icons
    )

    REM Resources (skip source files)
    echo [6/6] Copying resources...
    if exist "%SCRIPT_DIR%res" (
        if not exist "%MOD_PATH%\res" mkdir "%MOD_PATH%\res"
        for /r "%SCRIPT_DIR%res" %%F in (*) do (
            set "FILE=%%~nxF"
            if not "!FILE:~-4!"==".cpp" (
                if not "!FILE:~-2!"==".h" (
                    if not "!FILE:~-3!"==".in" (
                        copy /Y "%%F" "%MOD_PATH%\res\" >nul 2>&1
                    )
                )
            )
        )
        echo   [OK] Resources
    ) else (
        echo   [SKIP] No resources
    )

    color 0A
    echo.
    echo [OK] Deployment complete
    echo Location: %MOD_PATH%
)

:POST_BUILD_MENU
echo.
echo.
echo 1 - Build again
echo 2 - View log
echo 3 - Exit
echo.
set /p "CHOICE="

if "%CHOICE%"=="1" goto SELECT_RUNTIME
if "%CHOICE%"=="2" type "%BUILD_LOG%" && pause && goto SELECT_RUNTIME
if "%CHOICE%"=="3" exit /b 0

endlocal
