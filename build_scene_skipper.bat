@echo off
setlocal
rem Build scene_skipper.dll. Run from the "x64 Native Tools Command Prompt for VS".

rem Work in this script's own folder, wherever the prompt is.
cd /d "%~dp0"

where cl.exe >nul 2>nul || (echo cl.exe not found - use the x64 Native Tools prompt & exit /b 1)
where rc.exe >nul 2>nul || (echo rc.exe not found & exit /b 1)

rem The resources: the version (author, product and version) and the default
rem settings, scene_skipper.ini byte for byte.
rc /nologo /fo scene_skipper.res scene_skipper.rc
if errorlevel 1 (echo RC FAILED & exit /b 1)

rem scene_skipper.map turns an offset in a crash line back into a function
rem name.
cl /nologo /LD /O2 /EHsc /W4 /w14459 scene_skipper.cpp scene_skipper.res ^
   /link /OUT:scene_skipper.dll /MAP:scene_skipper.map
if errorlevel 1 (echo BUILD FAILED & exit /b 1)

del /q scene_skipper.obj scene_skipper.exp scene_skipper.lib ^
       scene_skipper.res 2>nul

echo.
echo Built scene_skipper.dll
exit /b 0
