# apps/audio/CMakeLists.txt
set(HELIX_SRC_DIR ${CMAKE_SOURCE_DIR}/../../lib/helix-mp3/src)

file(GLOB HELIX_SRCS "${HELIX_SRC_DIR}/*.c")

add_custom_target(build_espapp ALL
    # 1. Compile App Code
    COMMAND xtensa-esp32s3-elf-gcc -c ${CMAKE_CURRENT_SOURCE_DIR}/app_audio.c -o app_audio.o ${PIC_FLAGS}
    
    # 2. Compile Helix Code (Must use same PIC flags!)
    COMMAND xtensa-esp32s3-elf-gcc -c ${HELIX_SRCS} -I${HELIX_SRC_DIR} ${PIC_FLAGS} -O2
    
    # 3. Link everything together
    COMMAND xtensa-esp32s3-elf-gcc app_audio.o *.o -o app_audio.elf -T ${LINKER_SCRIPT} -nostdlib -lgcc
    
    # 4. Package
    COMMAND python ${BUILD_TOOL} app_audio.elf audio.espapp ${SIGNING_KEY}
)
