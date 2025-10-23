set(component_srcs "src/esp_schedule.c"
                   "src/esp_schedule_nvs.c")

idf_component_register(SRCS "${component_srcs}"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES "rmaker_common"
                       PRIV_REQUIRES nvs_flash)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
