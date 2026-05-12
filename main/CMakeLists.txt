idf_component_register(
    SRCS 
        "main.c"
                       
    INCLUDE_DIRS 
        "."

    REQUIRES 
        gas_sensors
        network
        driver
        esp_timer
        

)
