@echo off
rem Build scene_skipper.dll.
rem Run from "x64 Native Tools Command Prompt for VS".

where cl.exe >nul 2>nul || (echo cl.exe not found - use the x64 Native Tools prompt & exit /b 1)
where rc.exe >nul 2>nul || (echo rc.exe not found & exit /b 1)

rem The version resource, so the DLL carries its author, product and version.
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
