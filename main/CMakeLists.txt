# List of source files
set(SOURCES
    "deck_base.cpp"
    "battery_measurement.cpp"
    "c3_keyboard.cpp"
    "ssh_terminal.cpp"
)

# List of include directories
set(INCLUDE_DIRS
    "."
    "include"
)

# Required components
set(REQUIRED_COMPONENTS
    lvgl
    nvs_flash
    esp_lcd_touch_gt911
    fatfs
    sdmmc
    esp_wifi
    esp_netif
    esp_event
    esp_timer
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES ${REQUIRED_COMPONENTS}
)

# Add version number as compile definition
target_compile_definitions(${COMPONENT_LIB} PRIVATE POCKETSSH_VERSION="${PROJECT_VER}")
