# ESP Scheduling

[![Component Registry](https://components.espressif.com/components/espressif/esp_schedule/badge.svg)](https://components.espressif.com/components/espressif/esp_schedule)

This component is used internally by ESP RainMaker to implement schedules.

> Note: By default, the time is w.r.t. UTC. If the timezone has been set, then the time is w.r.t. the specified timezone.

## Test code:

```
#include <esp_schedule.h>

void app_schedule_trigger_cb(esp_schedule_handle_t handle, void *priv_data)
{
    printf("priv_data: %.*s\n", (char *)priv_data);
}

static char *priv_data_global = "from app";

void app_schedule_set()
{
    esp_schedule_config_t schedule_config = {
        .name = "test",
        .trigger.type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK,
        .trigger.hours = 13,
        .trigger.minutes = 30,
        .trigger.day.repeat_days = ESP_SCHEDULE_DAY_MONDAY | ESP_SCHEDULE_DAY_THURSDAY,
        .trigger_cb = app_schedule_trigger_cb,
        .priv_data = priv_data_global,
    };
    esp_schedule_create(&schedule_config);
}

void app_schedule_init()
{
    uint8_t schedule_count;
    esp_schedule_handle_t *schedule_list = esp_schedule_init(true, NULL, &schedule_count);
    if (schedule_count > 0 && schedule_list != NULL) {
        ESP_LOGI(TAG, "Found %d schedule(s) in NVS.", schedule_count);
        for (size_t i = 0; i < schedule_count; i++) {
            esp_schedule_config_t schedule_config;
            esp_schedule_get(schedule_list[i], &schedule_config);
            schedule_config.trigger_cb = app_schedule_trigger_cb;
            schedule_config.priv_data = priv_data_global;
            esp_schedule_edit(schedule_list[i], &schedule_config);
        }
    }
}
```

## Glue Layers

This component makes use of the following glue abstraction layers under `glue`:
- `glue_log.h`: Logging
- `glue_mem.h`: Memory allocation
- `glue_nvs.h`: Non-Volatile Storage
- `glue_time.h`: Time provider and synchronization
- `glue_timer.h`: Timer implementation

### As an ESP-IDF component

When using this component normally, the default glue implementations are used:
- Logging: `glue/esp/glue_log_impl.h` 
- Memory allocation: `glue/esp/glue_mem_impl.h`
- Non-Volatile Storage: `glue/esp/nvs.c`
- Time provider and synchronization: `glue/esp/time.c`
- Timer implementation: `glue/esp/timer.c`

### Custom glue implementations

If the underlying implementations are required to be changed, then you would need to implement a custom `CMakeLists.txt` for this component:
1. Common non-glue sources and include directories can be included using `esp_schedule_variables.cmake`.
2. Append your glue sources and include directories to the variables provided.
3. Use the variables to build your target library (e.g., passing them to `idf_component_register`).

The [default implementation](#as-an-esp-idf-component) does this in this component's `CMakeLists.txt` with the default glue implementations.