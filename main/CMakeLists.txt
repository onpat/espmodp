idf_component_register(SRCS "http_server.cpp" "sound.cpp" "main.cpp" "lcd.cpp" "textbox.cpp" "bitmap.hpp"
INCLUDE_DIRS "."
                       EMBED_FILES "roadblast.xm"
                       REQUIRES driver esp_lcd heap libxm esp_timer esp_driver_gptimer esp_driver_dac esp_driver_i2s esp_wifi esp_http_server nvs_flash cjson)
