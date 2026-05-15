idf_component_register(
    SRCS "src/passive_buzzer.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer
)