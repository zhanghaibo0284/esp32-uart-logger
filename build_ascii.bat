@echo off
set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.3.1"
set "PYTHONUTF8=1"
call "C:\Espressif\idf_cmd_init.bat"
cd /d F:\esp32_uart_log
idf.py build