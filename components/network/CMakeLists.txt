idf_component_register(
    SRCS 
        "src/network.c"
    INCLUDE_DIRS 
        "include"
    REQUIRES 
        nvs_flash 
        esp_wifi 
        esp_netif 
        esp_event 
        mqtt 
        efuse
        json
        # esp_sntp 
        # esp_mac
)