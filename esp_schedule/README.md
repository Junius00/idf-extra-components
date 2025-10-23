# ESP Scheduling

[![Component Registry](https://components.espressif.com/components/espressif/esp_schedule/badge.svg)](https://components.espressif.com/components/espressif/esp_schedule)

This component is used to implement scheduling for:

- **One-shot events** with a relative time difference (e.g., 30 seconds into the future)
- **Periodic events** based on a certain time[^1] on days of the week (e.g., every Monday or Wednesday)
- **Periodic/one-shot events** on a certain time[^1] based on the date:
  - e.g., *(periodic)* every 23rd of January to April
  - e.g., *(one-shot)* 9th of August, 2026
- **Periodic events** at an offset from sunrise/sunset

[^1]: By default, the time is w.r.t. UTC. If the timezone has been set, then the time is w.r.t. the specified timezone.

## Example Usage

See the comprehensive example in [`examples/get_started/`](examples/get_started/) for a complete demonstration of all ESP Schedule features, including:

- **Days of Week Scheduling** - Recurring events on specific weekdays
- **Date-based Scheduling** - Monthly and yearly recurring events
- **Relative Scheduling** - One-time delayed events
- **Solar Scheduling** - Sunrise/sunset based events with location coordinates and day-of-week filtering
- **Schedule Persistence** - NVS storage and recovery
- **Callback Handling** - Trigger and timestamp callbacks
- **Schedule Management** - Create, edit, enable, and disable schedules

The example includes detailed documentation, build instructions, and demonstrates all schedule types with practical use cases.

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