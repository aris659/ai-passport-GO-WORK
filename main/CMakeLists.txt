# 本地 WiFi 凭据不入库：首次构建自动从模板生成（已存在则不覆盖）
if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/wifi_config.h)
    configure_file(${CMAKE_CURRENT_LIST_DIR}/wifi_config.example.h
                   ${CMAKE_CURRENT_LIST_DIR}/wifi_config.h COPYONLY)
endif()

idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_pomodoro.c"
         "pomodoro_model.c"
         "pomodoro_store.c"
         "pomodoro_date.c"
         "pomodoro_stats.c"
         "pomodoro_blob.c"
         "pomodoro_time.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash esp_timer esp_wifi esp_netif esp_event lwip
)
