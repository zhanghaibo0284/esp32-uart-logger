set(src_dirs
    MYIIC
    LCD
    LED
    TOUCH
    XL9555)

set(include_dirs
    MYIIC
    LCD
    LED
    TOUCH
    XL9555)

idf_component_register(SRC_DIRS ${src_dirs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES driver esp_lcd)

target_compile_options(${COMPONENT_LIB} PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
