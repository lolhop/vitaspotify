@echo off
setlocal EnableExtensions

set "DIR=%~dp0"
set "HELPER=%DIR%vitaspotify-auth-helper.exe"
set "OUT=%USERPROFILE%\Desktop\vitaspotify auth.json"

cls
echo.
echo ==============================================
echo   VitaSpotify - create auth.json
echo ==============================================
echo.

if not exist "%HELPER%" (
  echo ERROR: vitaspotify-auth-helper.exe is missing from this folder.
  echo.
  echo The zip you received should include:
  echo   windows\vitaspotify-auth-helper.exe
  echo   windows\Get VitaSpotify Login.bat
  echo.
  echo If you only have the .bat, re-download the full VitaSpotify zip
  echo from whoever shared the app (not the .bat file alone).
  echo.
  pause
  exit /b 1
)

echo This will create a login file on your Desktop:
echo   %OUT%
echo.
echo STEPS:
echo   1. Leave this window open.
echo   2. Open Spotify on your phone (same Wi-Fi as this PC).
echo   3. Play any song - tap Connect to a device (speaker icon).
echo   4. Choose:  CSpot player
echo   5. Wait until you see SUCCESS below (may take 10-30 seconds).
echo.
echo Starting helper...
echo.

"%HELPER%" -c "%OUT%"
set "STATUS=%ERRORLEVEL%"

echo.
if exist "%OUT%" (
  for %%A in ("%OUT%") do if %%~zA GTR 0 (
    echo ==============================================
    echo   SUCCESS
    echo ==============================================
    echo.
    echo Created: %OUT%
    echo.
    echo NEXT - copy to your PS Vita:
    echo   1. VitaShell - enable USB or FTP
    echo   2. Put the file at: ux0:data/vitaspotify/auth.json
    echo   3. VitaSpotify - Log in (auth.json^)
    echo.
    goto :done
  )
)

echo ==============================================
echo   NOT CREATED YET
echo ==============================================
echo.
echo auth.json was not created.
echo Did you pick CSpot player in the Spotify app?
echo Run this script again and complete the Connect step.
echo.

:done
pause
exit /b %STATUS%
