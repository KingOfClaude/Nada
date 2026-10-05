@echo off
rem Run this from the "Developer Command Prompt for VS"
rc /nologo Nada.rc
cl /nologo /EHsc /std:c++17 /MT /DUNICODE /D_UNICODE Nada.cpp Nada.res /Fe:Nada.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib
