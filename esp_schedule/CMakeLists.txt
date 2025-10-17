set(component_srcs "src/esp_schedule.c"
                   "src/esp_schedule_nvs.c"
                   "glue/esp/nvs.c"
                   "glue/esp/timer.c")
set(component_include_dirs "include/common"
                          "include/esp"
                          "glue")

idf_component_register(SRCS "${component_srcs}"
                       INCLUDE_DIRS "${component_include_dirs}"
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES "rmaker_common"
                       REQUIRES "nvs_flash" "esp_daylight")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-function")
