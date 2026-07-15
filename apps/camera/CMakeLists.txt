# Camera app requires ESP-Camera component
set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/esp32-camera)

add_custom_target(build_espapp ALL
    COMMAND ${CMAKE_COMMAND} -E echo "Building Camera App..."
    COMMAND xtensa-esp32s3-elf-gcc 
        -c ${CMAKE_CURRENT_SOURCE_DIR}/app_camera.c 
        -o ${CMAKE_CURRENT_BINARY_DIR}/app_camera.o
        -fPIC -mlongcalls -mtext-section-literals 
        -ffunction-sections -fdata-sections 
        -I${CMAKE_SOURCE_DIR}/../../include
        -I${IDF_PATH}/components/esp32-camera/driver/include
        -nostdlib -O2
    
    COMMAND xtensa-esp32s3-elf-gcc 
        ${CMAKE_CURRENT_BINARY_DIR}/app_camera.o 
        -o ${CMAKE_CURRENT_BINARY_DIR}/camera.elf
        -T ${CMAKE_SOURCE_DIR}/../../tools/espapp.ld 
        -Wl,--gc-sections -static -nostdlib -lgcc
        
    COMMAND python ${CMAKE_SOURCE_DIR}/../../tools/build_espapp.py 
        ${CMAKE_CURRENT_BINARY_DIR}/camera.elf 
        ${CMAKE_CURRENT_BINARY_DIR}/camera.espapp 
        ${CMAKE_SOURCE_DIR}/../../keys/app_signing_key.pem
)
