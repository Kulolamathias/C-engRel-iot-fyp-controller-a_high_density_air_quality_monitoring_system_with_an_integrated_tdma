idf_component_register(
    SRCS 
        "src/lcd_i2c.c" 
                    
    INCLUDE_DIRS 
        "include"
    
    PRIV_INCLUDE_DIRS
        "priv_include"
                    
    PRIV_REQUIRES 
        esp_timer
        driver 
)