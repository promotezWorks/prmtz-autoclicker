@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d "C:\Users\broyl\OneDrive\Desktop\Projects\tools-and-utilities\prmtz-autoclicker\src"
"C:\Users\broyl\.goose\goose-v2026.4-windows-x64\goose.exe" build main.goose -o prmtz-autoclicker.exe
