call C:\espressif\esp-idf\v5.5.1\esp-idf\export.bat
if not exist sdkconfig (
    idf.py set-target esp32s3
) else (
    findstr /C:"CONFIG_IDF_TARGET=\"esp32s3\"" sdkconfig >nul || idf.py set-target esp32s3
)
idf.py build
