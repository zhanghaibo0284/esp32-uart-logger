@echo off
set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.3.1"
set "PYTHONUTF8=1"
set "IDF_TOOLS_PATH=C:\Espressif"
call "C:\Espressif\idf_cmd_init.bat"
cd /d X:\
idf.py -B X:/build2 build
