set(srcs "esp_schedule_example_main.c"
         "network/app_network.c"
    )
set(priv_include_dirs "network")
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS ${priv_include_dirs}
                       PRIV_REQUIRES esp_schedule nvs_flash esp_netif esp_event esp_wifi)
