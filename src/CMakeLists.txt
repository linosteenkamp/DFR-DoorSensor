set(SRCS
    "main.c"
    "contact_map.c"
    "debounce.c"
    "door_contact.c"
)

# Managed-component include path, added unconditionally so it resolves in the
# zigbee env. In a non-zigbee build the Zigbee bodies are gated by #ifdef USE_ZIGBEE.
set(ZB_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp-zigbee-lib/include"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "."
        "../include"
        ${ZB_INCLUDE_DIR}
    REQUIRES
        espressif__esp-zigbee-lib
        espressif__esp-zboss-lib
        esp_timer
        nvs_flash
        esp_event
)
