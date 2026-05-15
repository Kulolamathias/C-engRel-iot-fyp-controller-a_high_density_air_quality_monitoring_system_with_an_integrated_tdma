idf_component_register(
    SRCS 
        "main.c"
                       
    INCLUDE_DIRS 
        "."

    REQUIRES 
        gas_sensors
        lcd_i2c
        buzzer
        network
        driver
        esp_timer
        

)
