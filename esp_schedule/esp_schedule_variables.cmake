# This file is used to set the common non-glue variables for the esp_schedule component.
# Include this file and extend the variables to add glue sources and include directories.

# Source files
set(ESP_SCHEDULE_SRCS "src/esp_schedule.c"
                       "src/esp_schedule_nvs.c")

# Include directories
set(ESP_SCHEDULE_INCLUDE_DIRS "include/common")

# Private include directories
set(ESP_SCHEDULE_PRIV_INCLUDE_DIRS "src"
                                   "glue")

# Private requirements
set(ESP_SCHEDULE_PRIV_REQUIRES "esp_daylight")