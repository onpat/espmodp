idf_component_register(SRCS "input.cpp" "playlist.cpp" "http_server.cpp" "sound.cpp" "main.cpp" "lcd.cpp" "textbox.cpp" "bitmap.hpp"
INCLUDE_DIRS "."
                         REQUIRES driver esp_lcd heap libxm libsac esp_timer esp_driver_gptimer esp_driver_dac esp_driver_i2s esp_wifi esp_http_server nvs_flash cjson fatfs)

target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -ffast-math)
