include(${CMAKE_CURRENT_LIST_DIR}/../../esp_schedule_variables.cmake)

idf_component_register(SRCS "main.c"
                    PRIV_INCLUDE_DIRS ${ESP_SCHEDULE_PRIV_INCLUDE_DIRS}
                    PRIV_REQUIRES ${ESP_SCHEDULE_PRIV_REQUIRES} "esp_schedule" "unity"
                    )
