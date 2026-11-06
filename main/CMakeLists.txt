idf_component_register(
    SRCS 
        "main.c"
    INCLUDE_DIRS 
        "."
    REQUIRES 
        iot_service 
        driver 
        freertos
)

# Crée l'image LittleFS à partir du dossier web/ et la flashe sur la partition 'storage'
littlefs_create_partition_image(storage ../web FLASH_IN_PROJECT)
