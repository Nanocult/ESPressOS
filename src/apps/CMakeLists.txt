# apps/clock/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# Include ESP-IDF build system functions
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(app_clock)

# Custom target to build the .espapp
add_custom_target(build_espapp ALL
    COMMAND ${CMAKE_COMMAND} -E echo "Building Clock App..."
    # 1. Compile object with PIC
    COMMAND xtensa-esp32s3-elf-gcc 
        -c ${CMAKE_CURRENT_SOURCE_DIR}/app_clock.c 
        -o ${CMAKE_CURRENT_BINARY_DIR}/app_clock.o
        -fPIC -mlongcalls -mtext-section-literals 
        -ffunction-sections -fdata-sections 
        -I${CMAKE_SOURCE_DIR}/../../include # Path to kernel_api.h
        -nostdlib -O2
    
    # 2. Link with custom linker script
    COMMAND xtensa-esp32s3-elf-gcc 
        ${CMAKE_CURRENT_BINARY_DIR}/app_clock.o 
        -o ${CMAKE_CURRENT_BINARY_DIR}/app_clock.elf
        -T ${CMAKE_SOURCE_DIR}/../../tools/espapp.ld 
        -Wl,--gc-sections -static -nostdlib -lgcc
        
    # 3. Run post-build tool to sign and package
    COMMAND python ${CMAKE_SOURCE_DIR}/../../tools/build_espapp.py 
        ${CMAKE_CURRENT_BINARY_DIR}/app_clock.elf 
        ${CMAKE_CURRENT_BINARY_DIR}/clock.espapp 
        ${CMAKE_SOURCE_DIR}/../../keys/app_signing_key.pem
)
